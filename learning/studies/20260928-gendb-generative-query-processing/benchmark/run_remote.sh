#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-/data00/benchmarks/gendb-tpch-sf10-20260928}"
THREADS="${THREADS:-64}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
WARMUPS="${WARMUPS:-1}"
RUNS="${RUNS:-10}"
BENCHMARK_TIMEOUT_SECONDS="${BENCHMARK_TIMEOUT_SECONDS:-1800}"
ABLATION_TIMEOUT_SECONDS="${ABLATION_TIMEOUT_SECONDS:-2400}"
EVIDENCE_DIR="${ROOT}/evidence"
PREPARATION_KEY_FILE="${EVIDENCE_DIR}/preparation-key.txt"
PREPARATION_MANIFEST="${EVIDENCE_DIR}/preparation-manifest.json"
ARTIFACT_MANIFEST="${EVIDENCE_DIR}/artifact-sha256.txt"
SOURCE_MANIFEST="${EVIDENCE_DIR}/source-sha256.txt"
REFERENCE_MANIFEST="${EVIDENCE_DIR}/reference-sha256.txt"
HARNESS_MANIFEST="${EVIDENCE_DIR}/harness-sha256.txt"
FILC_EVIDENCE="${EVIDENCE_DIR}/filc-dbgen-smoke.json"
RUN_OUTPUT="${ROOT}/evidence/run"
COMPLETE_MANIFEST="${RUN_OUTPUT}/complete.json"
LOCK_FILE="${ROOT}/.benchmark.lock"

export OMP_NUM_THREADS="${THREADS}"
export OMP_PROC_BIND="spread"
export OMP_PLACES="cores"
export PYTHONUNBUFFERED="1"

mkdir -p "${RUN_OUTPUT}"
command -v flock >/dev/null
exec 9> "${LOCK_FILE}"
if ! flock -n 9; then
    echo "Another benchmark prepare/run process holds ${LOCK_FILE}." >&2
    exit 1
fi
rm -f \
    "${COMPLETE_MANIFEST}" \
    "${COMPLETE_MANIFEST}.tmp"
RUN_COMPLETED=0
cleanup_incomplete_run() {
    if [[ "${RUN_COMPLETED}" -ne 1 ]]; then
        rm -f "${COMPLETE_MANIFEST}" "${COMPLETE_MANIFEST}.tmp"
    fi
}
trap cleanup_incomplete_run EXIT

if [[ "${PYTHON_BIN}" != /* ]]; then
    PYTHON_BIN="$(command -v "${PYTHON_BIN}")"
fi
test -x "${PYTHON_BIN}"
for file in \
    "${PREPARATION_KEY_FILE}" \
    "${PREPARATION_MANIFEST}" \
    "${ARTIFACT_MANIFEST}" \
    "${SOURCE_MANIFEST}" \
    "${REFERENCE_MANIFEST}" \
    "${HARNESS_MANIFEST}" \
    "${FILC_EVIDENCE}" \
    "${EVIDENCE_DIR}/duckdb-setup.json"; do
    test -s "${file}" || {
        echo "Missing preparation evidence: ${file}" >&2
        exit 1
    }
done

mapfile -t PREPARATION_FIELDS < <(
    "${PYTHON_BIN}" - "${PREPARATION_MANIFEST}" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1]))
if manifest.get("schema_version") != 2 or manifest.get("status") != "complete":
    raise SystemExit("invalid preparation manifest status or schema")
for field in (
    "preparation_key",
    "gendb_commit",
    "dbgen_commit",
    "query_flags",
    "query_ldflags",
    "artifact_manifest_sha256",
    "source_manifest_sha256",
    "reference_manifest_sha256",
    "harness_manifest_sha256",
    "duckdb_setup_sha256",
    "duckdb_database_sha256",
    "python_bin",
    "duckdb_version",
    "filc_evidence_sha256",
    "cpu_identity_sha256",
    "hostname",
    "python_executable_sha256",
    "duckdb_engine_version",
):
    print(manifest[field])
PY
)
PREPARATION_KEY="${PREPARATION_FIELDS[0]}"
GENDB_COMMIT="${PREPARATION_FIELDS[1]}"
DBGEN_COMMIT="${PREPARATION_FIELDS[2]}"
COMPILE_FLAGS="${PREPARATION_FIELDS[3]} ${PREPARATION_FIELDS[4]}"

current_cpu_identity_sha256() {
    lscpu | awk -F: '
        /Architecture|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|Model name|L[123].*cache/ {
            gsub(/^[ \t]+|[ \t]+$/, "", $1)
            gsub(/^[ \t]+|[ \t]+$/, "", $2)
            print $1 "=" $2
        }
    ' | sha256sum | awk '{print $1}'
}

verify_prepared_inputs() {
    test "$(<"${PREPARATION_KEY_FILE}")" = "${PREPARATION_KEY}"
    test "${PYTHON_BIN}" = "${PREPARATION_FIELDS[11]}"
    test "$("${PYTHON_BIN}" -c 'import duckdb; print(duckdb.__version__)')" = \
        "${PREPARATION_FIELDS[12]}"
    test "$(git -C "${ROOT}/GenDB" rev-parse HEAD)" = "${GENDB_COMMIT}"
    test "$(git -C "${ROOT}/tpch-dbgen" rev-parse HEAD)" = "${DBGEN_COMMIT}"
    test -z "$(git -C "${ROOT}/GenDB" status --porcelain --untracked-files=no)"
    test -z "$(git -C "${ROOT}/tpch-dbgen" status --porcelain --untracked-files=no)"
    test "$(current_cpu_identity_sha256)" = "${PREPARATION_FIELDS[14]}"
    test "$(hostname)" = "${PREPARATION_FIELDS[15]}"
    test "$(sha256sum "${PYTHON_BIN}" | awk '{print $1}')" = \
        "${PREPARATION_FIELDS[16]}"
    test "$("${PYTHON_BIN}" -c \
        'import duckdb; print(duckdb.sql("PRAGMA version").fetchone()[0])')" = \
        "${PREPARATION_FIELDS[17]}"

    for pair in \
        "${ARTIFACT_MANIFEST}:${PREPARATION_FIELDS[5]}" \
        "${SOURCE_MANIFEST}:${PREPARATION_FIELDS[6]}" \
        "${REFERENCE_MANIFEST}:${PREPARATION_FIELDS[7]}" \
        "${HARNESS_MANIFEST}:${PREPARATION_FIELDS[8]}" \
        "${EVIDENCE_DIR}/duckdb-setup.json:${PREPARATION_FIELDS[9]}" \
        "${FILC_EVIDENCE}:${PREPARATION_FIELDS[13]}"; do
        file="${pair%%:*}"
        expected="${pair##*:}"
        test "$(sha256sum "${file}" | awk '{print $1}')" = "${expected}"
    done
    sha256sum --check --quiet "${ARTIFACT_MANIFEST}"
    sha256sum --check --quiet "${SOURCE_MANIFEST}"
    sha256sum --check --quiet "${REFERENCE_MANIFEST}"
    sha256sum --check --quiet "${HARNESS_MANIFEST}"
    printf '%s  %s\n' \
        "${PREPARATION_FIELDS[10]}" \
        "${ROOT}/duckdb/tpch-sf10.duckdb" |
        sha256sum --check --quiet -
}

verify_prepared_inputs
rm -f \
    "${RUN_OUTPUT}/benchmark-results.json" \
    "${RUN_OUTPUT}/ablation-results.json"
exec > >(tee "${ROOT}/evidence/benchmark.log") 2>&1

echo "== PMU access probe =="
command -v perf >/dev/null
if ! perf stat -x, -e task-clock,cycles,instructions,cache-misses \
    true 2>&1 | tee "${ROOT}/evidence/perf-access.txt"; then
    grep -Eqi 'not supported|no permission|permission denied' \
        "${ROOT}/evidence/perf-access.txt"
fi

echo "== GenDB vs DuckDB =="
timeout --signal=TERM "${BENCHMARK_TIMEOUT_SECONDS}" \
    numactl --cpunodebind=0,1 --interleave=all \
    taskset -c 0-63 \
    "${PYTHON_BIN}" "${ROOT}/harness/tpch_bench.py" run \
        --database "${ROOT}/duckdb/tpch-sf10.duckdb" \
        --gendb-dir "${ROOT}/gendb/sf10" \
        --bin-dir "${ROOT}/bin" \
        --reference-dir "${ROOT}/GenDB/benchmarks/tpc-h/query_results" \
        --output "${RUN_OUTPUT}" \
        --threads "${THREADS}" \
        --warmups "${WARMUPS}" \
        --runs "${RUNS}" \
        --gendb-commit "${GENDB_COMMIT}" \
        --dbgen-commit "${DBGEN_COMMIT}" \
        --compile-flags="${COMPILE_FLAGS}" \
        --record-correctness-failures

echo "== GenDB iteration ablation =="
timeout --signal=TERM "${ABLATION_TIMEOUT_SECONDS}" \
    numactl --cpunodebind=0,1 --interleave=all \
    taskset -c 0-63 \
    "${PYTHON_BIN}" "${ROOT}/harness/tpch_bench.py" ablation \
        --database "${ROOT}/duckdb/tpch-sf10.duckdb" \
        --gendb-dir "${ROOT}/gendb/sf10" \
        --bin-dir "${ROOT}/bin/iterations" \
        --output "${RUN_OUTPUT}" \
        --threads "${THREADS}" \
        --warmups "${WARMUPS}" \
        --runs "${RUNS}"

BENCHMARK_SHA256="$(sha256sum "${RUN_OUTPUT}/benchmark-results.json" | awk '{print $1}')"
ABLATION_SHA256="$(sha256sum "${RUN_OUTPUT}/ablation-results.json" | awk '{print $1}')"
BENCHMARK_STATUS="$(
    "${PYTHON_BIN}" -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["status"])' \
        "${RUN_OUTPUT}/benchmark-results.json"
)"
ABLATION_STATUS="$(
    "${PYTHON_BIN}" -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["status"])' \
        "${RUN_OUTPUT}/ablation-results.json"
)"
case "${BENCHMARK_STATUS}" in
    pass|correctness_failures_recorded) ;;
    *)
        echo "Unsupported benchmark status: ${BENCHMARK_STATUS}" >&2
        exit 1
        ;;
esac
case "${ABLATION_STATUS}" in
    pass|bounded_numeric_failures_recorded) ;;
    *)
        echo "Unsupported ablation status: ${ABLATION_STATUS}" >&2
        exit 1
        ;;
esac
verify_prepared_inputs
if [[ "${BENCHMARK_STATUS}" == "pass" && "${ABLATION_STATUS}" == "pass" ]]; then
    COMPLETION_STATUS="complete"
else
    COMPLETION_STATUS="complete_with_correctness_failures"
fi
cat > "${COMPLETE_MANIFEST}.tmp" <<EOF
{
  "schema_version": 1,
  "status": "${COMPLETION_STATUS}",
  "benchmark_status": "${BENCHMARK_STATUS}",
  "ablation_status": "${ABLATION_STATUS}",
  "preparation_key": "${PREPARATION_KEY}",
  "benchmark_results_sha256": "${BENCHMARK_SHA256}",
  "ablation_results_sha256": "${ABLATION_SHA256}"
}
EOF
mv "${COMPLETE_MANIFEST}.tmp" "${COMPLETE_MANIFEST}"
RUN_COMPLETED=1
trap - EXIT

echo "Benchmark complete: ${RUN_OUTPUT}"
