#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
demo_dir="$(cd "$script_dir/.." && pwd)"
study_dir="$(cd "$demo_dir/.." && pwd)"
compose_file="$demo_dir/compose.yaml"
canonical_evidence_dir="$study_dir/evidence"
evidence_dir="${EVIDENCE_DIR:-$canonical_evidence_dir}"
default_bench_runs=7
default_versions="17.10-bookworm@sha256:9b18b78397054fce88a9552e9d5a3ad5bb7fd258c5b3cc1c5028e46373d6ea8f 18.4-bookworm@sha256:882236b897e39051d2368c5ccc6cda944904723506b2dfc97f2a8f5bc9afa382"
default_depth_rows="100 1000 10000 100000 1000000"
default_cardinalities="100 1000 10000 100000"
default_cardinality_total_rows=1000000
bench_runs="${BENCH_RUNS:-$default_bench_runs}"
read -r -a versions <<<"${PG_VERSIONS:-$default_versions}"
read -r -a depth_rows_per_partition \
    <<<"${DEPTH_ROWS_PER_PARTITION:-$default_depth_rows}"
read -r -a cardinalities <<<"${CARDINALITIES:-$default_cardinalities}"
cardinality_total_rows="${CARDINALITY_TOTAL_ROWS:-$default_cardinality_total_rows}"
session_options="-c enable_seqscan=off -c max_parallel_workers_per_gather=0 -c jit=off"
compose_project_name="postgres-distinct-study-$$"
current_image=""
output_dir=""
backup_dir=""
lock_dir="${evidence_dir}.lock"

for command in docker jq; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'required command not found: %s\n' "$command" >&2
        exit 1
    fi
done

if [[ "$evidence_dir" == "$canonical_evidence_dir" ]] &&
   [[ "${BENCH_RUNS:-$default_bench_runs}" != "$default_bench_runs" ||
      "${PG_VERSIONS:-$default_versions}" != "$default_versions" ||
      "${DEPTH_ROWS_PER_PARTITION:-$default_depth_rows}" != "$default_depth_rows" ||
      "${CARDINALITIES:-$default_cardinalities}" != "$default_cardinalities" ||
      "${CARDINALITY_TOTAL_ROWS:-$default_cardinality_total_rows}" != "$default_cardinality_total_rows" ]]; then
    printf 'noncanonical settings require an explicit EVIDENCE_DIR\n' >&2
    exit 1
fi

if docker compose version >/dev/null 2>&1; then
    compose_command=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
    compose_command=(docker-compose)
else
    printf 'required command not found: docker compose or docker-compose\n' >&2
    exit 1
fi

mkdir -p "$(dirname "$evidence_dir")"
if ! mkdir "$lock_dir" 2>/dev/null; then
    printf 'evidence directory is locked by another run: %s\n' "$lock_dir" >&2
    exit 1
fi
trap 'rmdir "$lock_dir" 2>/dev/null || true' EXIT

output_dir="$(mktemp -d "${evidence_dir}.run.XXXXXX")"
if [[ -d "$evidence_dir" ]]; then
    cp -R "$evidence_dir/." "$output_dir/"
fi
mkdir -p "$output_dir/plans"
rm -f "$output_dir/benchmark.csv" \
      "$output_dir/complete.txt" \
      "$output_dir/correctness.csv" \
      "$output_dir/image-digests.txt" \
      "$output_dir/plan-metrics.csv" \
      "$output_dir/pgbench.log"
rm -f "$output_dir/plans/"*.json

printf '%s\n' \
    'server_version,scenario,total_rows,ndv,query,runs,latency_avg_ms,tps,result_count' \
    >"$output_dir/benchmark.csv"
printf '%s\n' \
    'server_version,scenario,total_rows,ndv,query,execution_ms,index_rows,index_searches,heap_fetches,shared_hit_blocks,shared_read_blocks,node_types' \
    >"$output_dir/plan-metrics.csv"
printf '%s\n' \
    'server_version,scenario,total_rows,ndv,symmetric_difference_count,ordinary_expected_difference_count,loose_expected_difference_count' \
    >"$output_dir/correctness.csv"

compose() {
    PG_IMAGE="$current_image" "${compose_command[@]}" \
        -p "$compose_project_name" -f "$compose_file" "$@"
}

cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$current_image" ]]; then
        if ! compose down --volumes --remove-orphans; then
            printf 'failed to clean up Compose project %s\n' \
                "$compose_project_name" >&2
            if [[ "$status" -eq 0 ]]; then
                status=1
            fi
        fi
    fi
    if [[ -n "$output_dir" && -d "$output_dir" ]]; then
        rm -rf "$output_dir"
    fi
    if [[ -n "$backup_dir" && -d "$backup_dir" && ! -e "$evidence_dir" ]]; then
        mv "$backup_dir" "$evidence_dir"
    fi
    rmdir "$lock_dir" 2>/dev/null || true
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_postgres() {
    local attempt
    for ((attempt = 1; attempt <= 90; attempt++)); do
        if compose exec -T postgres pg_isready -U postgres -d distinct_study \
            >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    printf 'PostgreSQL did not become ready for image %s\n' "$current_image" >&2
    return 1
}

psql_exec() {
    compose exec -T -e PGOPTIONS="$session_options" postgres \
        psql -X -v ON_ERROR_STOP=1 -U postgres -d distinct_study "$@"
}

is_number() {
    [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]]
}

record_plan() {
    local server_version="$1"
    local scenario="$2"
    local total_rows="$3"
    local ndv="$4"
    local query_name="$5"
    local query_file="$demo_dir/sql/$query_name.sql"
    local plan_file="$output_dir/plans/pg${server_version%%.*}-${scenario}-n${total_rows}-d${ndv}-${query_name}.json"
    local query
    local execution_ms
    local index_rows
    local index_searches
    local heap_fetches
    local shared_hit_blocks
    local shared_read_blocks
    local node_types
    local metrics
    local expected_searches

    query="$(<"$query_file")"
    psql_exec -Atc "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) $query" >"$plan_file"

    metrics="$(jq -r '
        [
          .[0]["Execution Time"],
          ([.. | objects | select(.["Node Type"] == "Index Only Scan") |
            ((.["Actual Rows"] // 0) * (.["Actual Loops"] // 1))] |
            add // 0 | round),
          ([.. | objects | select(has("Index Searches")) |
            .["Index Searches"]] |
            if length == 0 then "NA" else add end),
          ([.. | objects | select(has("Heap Fetches")) |
            .["Heap Fetches"]] | add // 0),
          (.[0].Plan["Shared Hit Blocks"] // 0),
          (.[0].Plan["Shared Read Blocks"] // 0),
          ([.. | objects | .["Node Type"]? // empty] | unique | join(">"))
        ] | @tsv
    ' "$plan_file")"
    IFS=$'\t' read -r execution_ms index_rows index_searches heap_fetches \
        shared_hit_blocks shared_read_blocks node_types <<<"$metrics"

    if ! is_number "$execution_ms" ||
       [[ ! "$index_rows" =~ ^[0-9]+$ ||
          ! "$heap_fetches" =~ ^[0-9]+$ ||
          ! "$shared_hit_blocks" =~ ^[0-9]+$ ||
          ! "$shared_read_blocks" =~ ^[0-9]+$ ]] ||
       [[ "$node_types" != *"Index Only Scan"* ]] ||
       [[ "$heap_fetches" -ne 0 ]]; then
        printf 'invalid plan metrics for %s: %s\n' "$query_name" "$metrics" >&2
        return 1
    fi

    if [[ "$query_name" == "distinct" ]]; then
        if [[ "$node_types" != *"Unique"* || "$index_rows" -ne "$total_rows" ]]; then
            printf 'unexpected DISTINCT plan for rows=%s ndv=%s: %s\n' \
                "$total_rows" "$ndv" "$metrics" >&2
            return 1
        fi
    elif [[ "$node_types" != *"Recursive Union"* ||
            "$index_rows" -lt "$ndv" ||
            "$index_rows" -gt $((ndv + 1)) ]]; then
        printf 'unexpected loose scan plan for rows=%s ndv=%s: %s\n' \
            "$total_rows" "$ndv" "$metrics" >&2
        return 1
    fi

    if [[ "${server_version%%.*}" -ge 18 ]]; then
        expected_searches=1
        if [[ "$query_name" == "loose-index-scan" ]]; then
            expected_searches=$((ndv + 1))
        fi
        if [[ "$index_searches" != "$expected_searches" ]]; then
            printf 'unexpected index search count for %s: got=%s expected=%s\n' \
                "$query_name" "$index_searches" "$expected_searches" >&2
            return 1
        fi
    elif [[ "$index_searches" != "NA" ]]; then
        printf 'PostgreSQL %s unexpectedly reported Index Searches\n' \
            "$server_version" >&2
        return 1
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' \
        "$server_version" "$scenario" "$total_rows" "$ndv" "$query_name" \
        "$execution_ms" "$index_rows" "$index_searches" "$heap_fetches" \
        "$shared_hit_blocks" "$shared_read_blocks" "$node_types" \
        >>"$output_dir/plan-metrics.csv"
}

benchmark_query() {
    local server_version="$1"
    local scenario="$2"
    local total_rows="$3"
    local ndv="$4"
    local query_name="$5"
    local result_count="$6"
    local query_file="/study/sql/$query_name.sql"
    local output
    local latency_avg
    local processed
    local failed
    local tps

    psql_exec -Atf "$query_file" >/dev/null
    output="$(
        compose exec -T -e PGOPTIONS="$session_options" postgres \
            pgbench -n -c 1 -j 1 -t "$bench_runs" -f "$query_file" \
            -U postgres distinct_study
    )"
    printf '\n[%s %s n=%s d=%s %s]\n%s\n' \
        "$server_version" "$scenario" "$total_rows" "$ndv" "$query_name" \
        "$output" >>"$output_dir/pgbench.log"

    latency_avg="$(awk '/latency average/ {print $(NF-1)}' <<<"$output")"
    processed="$(awk '/transactions actually processed/ {print $NF}' <<<"$output")"
    failed="$(awk '/failed transactions/ {print $5}' <<<"$output")"
    tps="$(awk '/tps =/ {print $3; exit}' <<<"$output")"
    if ! is_number "$latency_avg" ||
       ! is_number "$tps" ||
       [[
          "$processed" != "$bench_runs/$bench_runs" ||
          "$failed" != "0" ]]; then
        printf 'invalid pgbench result for %s: processed=%s failed=%s latency=%s tps=%s\n' \
            "$query_name" "$processed" "$failed" "$latency_avg" "$tps" >&2
        return 1
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$server_version" "$scenario" "$total_rows" "$ndv" "$query_name" \
        "$bench_runs" "$latency_avg" "$tps" "$result_count" \
        >>"$output_dir/benchmark.csv"

    record_plan "$server_version" "$scenario" "$total_rows" "$ndv" "$query_name"
}

run_scenario() {
    local server_version="$1"
    local scenario="$2"
    local total_rows="$3"
    local ndv="$4"
    local result_count
    local difference_count
    local ordinary_expected_difference_count
    local loose_expected_difference_count

    printf 'loading PostgreSQL %s scenario=%s rows=%s ndv=%s\n' \
        "$server_version" "$scenario" "$total_rows" "$ndv"
    psql_exec \
        -v total_rows="$total_rows" \
        -v ndv="$ndv" \
        -f /study/sql/load.sql >/dev/null

    IFS='|' read -r result_count difference_count \
        ordinary_expected_difference_count loose_expected_difference_count \
        < <(psql_exec -v ndv="$ndv" -Atf /study/sql/equivalence.sql)
    if [[ "$result_count" != "$ndv" ]]; then
        printf 'queries returned %s rows; expected %s\n' \
            "$result_count" "$ndv" >&2
        return 1
    fi
    if [[ "$difference_count" != "0" ]]; then
        printf 'query results differ for scenario=%s rows=%s ndv=%s\n' \
            "$scenario" "$total_rows" "$ndv" >&2
        return 1
    fi
    if [[ "$ordinary_expected_difference_count" != "0" ||
          "$loose_expected_difference_count" != "0" ]]; then
        printf 'query result differs from expected keys for scenario=%s rows=%s ndv=%s\n' \
            "$scenario" "$total_rows" "$ndv" >&2
        return 1
    fi
    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$server_version" "$scenario" "$total_rows" "$ndv" \
        "$difference_count" "$ordinary_expected_difference_count" \
        "$loose_expected_difference_count" >>"$output_dir/correctness.csv"

    benchmark_query \
        "$server_version" "$scenario" "$total_rows" "$ndv" distinct \
        "$result_count"
    benchmark_query \
        "$server_version" "$scenario" "$total_rows" "$ndv" loose-index-scan \
        "$result_count"
}

{
    printf 'captured_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'host_uname=%s\n' "$(uname -a)"
    printf 'docker_client=%s\n' "$(docker version --format '{{.Client.Version}}')"
    docker info --format 'docker_server={{.ServerVersion}} os={{.OperatingSystem}} arch={{.Architecture}} cpus={{.NCPU}} memory={{.MemTotal}}'
} >"$output_dir/environment.txt"

for version in "${versions[@]}"; do
    current_image="postgres:$version"
    compose pull postgres
    docker image inspect \
        --format 'image={{index .RepoTags 0}} digest={{index .RepoDigests 0}} id={{.Id}}' \
        "$current_image" >>"$output_dir/image-digests.txt"
    compose up -d postgres
    wait_for_postgres

    server_version="$(psql_exec -Atc 'SHOW server_version')"
    {
        printf '\npostgres_image=%s\n' "$current_image"
        printf 'postgres_server_version=%s\n' "$server_version"
        psql_exec -Atc "SELECT name || '=' || setting FROM pg_settings WHERE name IN ('block_size', 'jit', 'max_parallel_workers_per_gather', 'shared_buffers', 'track_io_timing', 'work_mem') ORDER BY name"
    } >>"$output_dir/environment.txt"

    psql_exec -f /study/sql/schema.sql >/dev/null

    for rows_per_partition in "${depth_rows_per_partition[@]}"; do
        run_scenario \
            "$server_version" \
            depth \
            "$((rows_per_partition * 10))" \
            10
    done

    for ndv in "${cardinalities[@]}"; do
        run_scenario "$server_version" cardinality "$cardinality_total_rows" "$ndv"
    done

    compose down --volumes --remove-orphans
    current_image=""
done

expected_scenarios=$((
    ${#versions[@]} *
    (${#depth_rows_per_partition[@]} + ${#cardinalities[@]})
))
expected_measurements=$((expected_scenarios * 2))
actual_plans="$(find "$output_dir/plans" -name '*.json' -type f | wc -l | tr -d ' ')"
actual_benchmarks="$(($(wc -l <"$output_dir/benchmark.csv") - 1))"
actual_correctness="$(($(wc -l <"$output_dir/correctness.csv") - 1))"
actual_plan_metrics="$(($(wc -l <"$output_dir/plan-metrics.csv") - 1))"

if [[ "$actual_plans" -ne "$expected_measurements" ||
      "$actual_benchmarks" -ne "$expected_measurements" ||
      "$actual_correctness" -ne "$expected_scenarios" ||
      "$actual_plan_metrics" -ne "$expected_measurements" ]]; then
    printf 'incomplete evidence: plans=%s benchmarks=%s correctness=%s plan_metrics=%s\n' \
        "$actual_plans" "$actual_benchmarks" "$actual_correctness" \
        "$actual_plan_metrics" >&2
    exit 1
fi

find "$output_dir/plans" -name '*.json' -type f -print0 |
    xargs -0 -n1 jq empty
if ! awk -F, 'NR > 1 && ($5 != 0 || $6 != 0 || $7 != 0) { exit 1 }' \
        "$output_dir/correctness.csv" ||
   ! awk -F, 'NR > 1 && $9 != $4 { exit 1 }' \
        "$output_dir/benchmark.csv" ||
   ! awk -F, 'NR > 1 && $9 != 0 { exit 1 }' \
        "$output_dir/plan-metrics.csv"; then
    printf 'generated evidence failed semantic validation\n' >&2
    exit 1
fi
printf 'status=complete\nversions=%s\nscenarios=%s\nmeasurements=%s\n' \
    "${#versions[@]}" "$expected_scenarios" "$expected_measurements" \
    >"$output_dir/complete.txt"
chmod 755 "$output_dir" "$output_dir/plans"

backup_dir="${evidence_dir}.previous.$$"
if [[ -d "$evidence_dir" ]]; then
    mv "$evidence_dir" "$backup_dir"
fi
if ! mv "$output_dir" "$evidence_dir"; then
    if [[ -d "$backup_dir" ]]; then
        mv "$backup_dir" "$evidence_dir"
    fi
    exit 1
fi
output_dir=""
rm -rf "$backup_dir"
backup_dir=""

printf 'evidence written to %s\n' "$evidence_dir"
