#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-/data00/benchmarks/gendb-tpch-sf10-20260928}"
THREADS="${THREADS:-64}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
PREPARE_STAGE_TIMEOUT_SECONDS="${PREPARE_STAGE_TIMEOUT_SECONDS:-1800}"
FILC_TIMEOUT_SECONDS="${FILC_TIMEOUT_SECONDS:-1800}"
GENDB_COMMIT="b7418071fc51aacd219964e4a68e593234db9237"
DBGEN_COMMIT="32f1c1b92d1664dba542e927d23d86ffa57aa253"
DUCKDB_VERSION="1.2.1"
RUN_DIR="output/tpc-h/2026-02-26T06-27-28"
PAPER_ROOT="${ROOT}/GenDB"
PAPER_RUN="${PAPER_ROOT}/${RUN_DIR}"
DATA_DIR="${ROOT}/data/sf10"
GENDB_DIR="${ROOT}/gendb/sf10"
DUCKDB_PATH="${ROOT}/duckdb/tpch-sf10.duckdb"
BIN_DIR="${ROOT}/bin"
EVIDENCE_DIR="${ROOT}/evidence"
HARNESS_DIR="${ROOT}/harness"
PREPARATION_KEY_FILE="${EVIDENCE_DIR}/preparation-key.txt"
PREPARATION_MANIFEST_FILE="${EVIDENCE_DIR}/preparation-manifest.json"
ARTIFACT_MANIFEST_FILE="${EVIDENCE_DIR}/artifact-sha256.txt"
SOURCE_MANIFEST_FILE="${EVIDENCE_DIR}/source-sha256.txt"
REFERENCE_MANIFEST_FILE="${EVIDENCE_DIR}/reference-sha256.txt"
HARNESS_MANIFEST_FILE="${EVIDENCE_DIR}/harness-sha256.txt"
FILC_EVIDENCE_FILE="${EVIDENCE_DIR}/filc-dbgen-smoke.json"
LOCK_FILE="${ROOT}/.benchmark.lock"

DBGEN_FLAGS="CC=gcc MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH"
INGEST_FLAGS="-O3 -march=native -mtune=native -DNDEBUG -std=c++17 -pthread"
QUERY_FLAGS="-O3 -march=native -mtune=native -DNDEBUG -flto -std=c++17 -Wall -fopenmp -DGENDB_PROFILE"
QUERY_LDFLAGS="-lstdc++fs"

mkdir -p "${BIN_DIR}" "${EVIDENCE_DIR}" "${GENDB_DIR}" "${ROOT}/duckdb"
command -v flock >/dev/null
exec 9> "${LOCK_FILE}"
if ! flock -n 9; then
    echo "Another benchmark prepare/run process holds ${LOCK_FILE}." >&2
    exit 1
fi
if [[ "${PYTHON_BIN}" != /* ]]; then
    PYTHON_BIN="$(command -v "${PYTHON_BIN}")"
fi
test -x "${PYTHON_BIN}"

run_bounded() {
    timeout --signal=TERM --kill-after=30s \
        "${PREPARE_STAGE_TIMEOUT_SECONDS}" "$@"
}

CURRENT_PREPARATION_KEY=""
if [[ -f "${PREPARATION_KEY_FILE}" ]]; then
    CURRENT_PREPARATION_KEY="$(<"${PREPARATION_KEY_FILE}")"
    mv "${PREPARATION_KEY_FILE}" "${PREPARATION_KEY_FILE}.previous"
elif [[ -f "${PREPARATION_KEY_FILE}.previous" ]]; then
    CURRENT_PREPARATION_KEY="$(<"${PREPARATION_KEY_FILE}.previous")"
fi
if [[ -f "${PREPARATION_MANIFEST_FILE}" ]]; then
    mv "${PREPARATION_MANIFEST_FILE}" "${PREPARATION_MANIFEST_FILE}.previous"
fi
rm -f "${EVIDENCE_DIR}/run/complete.json"
exec > >(tee "${EVIDENCE_DIR}/prepare.log") 2>&1

echo "== Provision pinned sources and data =="
if [[ ! -d "${PAPER_ROOT}/.git" ]]; then
    timeout --signal=TERM 300 git clone --branch arxiv-03-02-2026 \
        https://github.com/SolidLao/GenDB.git "${PAPER_ROOT}"
fi
if ! git -C "${PAPER_ROOT}" cat-file -e "${GENDB_COMMIT}^{commit}"; then
    timeout --signal=TERM 300 \
        git -C "${PAPER_ROOT}" fetch origin "${GENDB_COMMIT}"
fi
git -C "${PAPER_ROOT}" checkout --detach "${GENDB_COMMIT}"
test -z "$(git -C "${PAPER_ROOT}" status --porcelain --untracked-files=no)"

if [[ ! -d "${ROOT}/tpch-dbgen/.git" ]]; then
    timeout --signal=TERM 300 \
        git clone https://github.com/electrum/tpch-dbgen.git "${ROOT}/tpch-dbgen"
fi
if ! git -C "${ROOT}/tpch-dbgen" cat-file -e "${DBGEN_COMMIT}^{commit}"; then
    timeout --signal=TERM 300 \
        git -C "${ROOT}/tpch-dbgen" fetch origin "${DBGEN_COMMIT}"
fi
git -C "${ROOT}/tpch-dbgen" checkout --detach "${DBGEN_COMMIT}"
test -z "$(git -C "${ROOT}/tpch-dbgen" status --porcelain --untracked-files=no)"

echo "== FIL-C dbgen correctness gate =="
timeout --signal=TERM --kill-after=30s "${FILC_TIMEOUT_SECONDS}" \
    env PYTHON_BIN="${PYTHON_BIN}" \
    "${HARNESS_DIR}/verify_dbgen_filc.sh" "${ROOT}"

if [[ ! -s "${DATA_DIR}/lineitem.tbl" ]]; then
    run_bounded make -C "${ROOT}/tpch-dbgen" -f makefile clean
    run_bounded make -C "${ROOT}/tpch-dbgen" -f makefile -j8 \
        CC=gcc MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH
    (
        cd "${ROOT}/tpch-dbgen"
        run_bounded ./dbgen -s 10 -f
        mkdir -p "${DATA_DIR}"
        mv -f ./*.tbl "${DATA_DIR}/"
    )
fi

echo "== Environment =="
date --iso-8601=seconds
uname -a
lscpu
numactl --hardware
g++ --version
"${PYTHON_BIN}" - "${DUCKDB_VERSION}" <<'PY'
import duckdb
import platform
import sys

print("python", platform.python_version())
print("duckdb-python", duckdb.__version__)
print("duckdb-engine", duckdb.sql("PRAGMA version").fetchone())
if duckdb.__version__ != sys.argv[1]:
    raise SystemExit(
        f"expected duckdb-python {sys.argv[1]}, got {duckdb.__version__}"
    )
PY

echo "== Input manifest =="
declare -A EXPECTED_ROWS=(
    [nation]=25
    [region]=5
    [supplier]=100000
    [part]=2000000
    [partsupp]=8000000
    [customer]=1500000
    [orders]=15000000
    [lineitem]=59986052
)
for table in "${!EXPECTED_ROWS[@]}"; do
    source_file="${DATA_DIR}/${table}.tbl"
    test -s "${source_file}"
    actual_rows="$(wc -l < "${source_file}")"
    if [[ "${actual_rows}" -ne "${EXPECTED_ROWS[${table}]}" ]]; then
        echo "${table}: expected ${EXPECTED_ROWS[${table}]} rows, got ${actual_rows}" >&2
        exit 1
    fi
done
(
    cd "${DATA_DIR}"
    sha256sum ./*.tbl | sort -k2
) | tee "${EVIDENCE_DIR}/input-sha256.txt"
for file in "${DATA_DIR}"/*.tbl; do
    printf "%s\t%s\t%s\n" \
        "$(basename "${file}")" \
        "$(wc -l < "${file}")" \
        "$(stat -c %s "${file}")"
done | sort | tee "${EVIDENCE_DIR}/input-rows-bytes.txt"

lscpu | awk -F: '
    /Architecture|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|Model name|L[123].*cache/ {
        gsub(/^[ \t]+|[ \t]+$/, "", $1)
        gsub(/^[ \t]+|[ \t]+$/, "", $2)
        print $1 "=" $2
    }
' > "${EVIDENCE_DIR}/cpu-identity.txt"

PREPARATION_KEY="$(
    {
        printf '%s\n' \
            "gendb=${GENDB_COMMIT}" \
            "dbgen=${DBGEN_COMMIT}" \
            "dbgen_flags=${DBGEN_FLAGS}" \
            "ingest_flags=${INGEST_FLAGS}" \
            "query_flags=${QUERY_FLAGS}" \
            "query_ldflags=${QUERY_LDFLAGS}" \
            "duckdb=$("${PYTHON_BIN}" -c 'import duckdb; print(duckdb.__version__)')" \
            "compiler=$(g++ -dumpfullversion -dumpversion)" \
            "filc_evidence=$(sha256sum "${FILC_EVIDENCE_FILE}" | awk '{print $1}')"
        awk '{print $1}' "${EVIDENCE_DIR}/input-sha256.txt"
        {
            find "${PAPER_RUN}/generated_ingest" \
                "${PAPER_RUN}/queries/Q1" \
                "${PAPER_RUN}/queries/Q3" \
                "${PAPER_RUN}/queries/Q6" \
                "${PAPER_RUN}/queries/Q9" \
                "${PAPER_RUN}/queries/Q18" \
                "${PAPER_ROOT}/src/gendb/utils" \
                -type f \( -name '*.cpp' -o -name '*.h' \) -print0
            printf '%s\0' "${PAPER_ROOT}/benchmarks/tpc-h/schema.sql"
        } | sort -z | xargs -0 sha256sum | awk '{print $1}'
        cat "${EVIDENCE_DIR}/cpu-identity.txt"
    } | sha256sum | awk '{print $1}'
)"

if [[ -s "${GENDB_DIR}/lineitem/l_shipdate.bin" || -s "${DUCKDB_PATH}" ]] &&
   [[ "${CURRENT_PREPARATION_KEY}" != "${PREPARATION_KEY}" ]]; then
    echo "Prepared artifacts do not match the current provenance key." >&2
    echo "current=${CURRENT_PREPARATION_KEY:-missing}" >&2
    echo "expected=${PREPARATION_KEY}" >&2
    echo "Use a clean ROOT instead of reusing this directory." >&2
    exit 1
fi
if [[ -s "${GENDB_DIR}/lineitem/l_shipdate.bin" || -s "${DUCKDB_PATH}" ]]; then
    test -s "${ARTIFACT_MANIFEST_FILE}"
    sha256sum --check --quiet "${ARTIFACT_MANIFEST_FILE}"
    test -s "${EVIDENCE_DIR}/duckdb-setup.json"
    EXPECTED_DUCKDB_SHA="$(
        "${PYTHON_BIN}" -c \
            'import json,sys; print(json.load(open(sys.argv[1]))["database_sha256"])' \
            "${EVIDENCE_DIR}/duckdb-setup.json"
    )"
    printf '%s  %s\n' "${EXPECTED_DUCKDB_SHA}" "${DUCKDB_PATH}" |
        sha256sum --check --quiet -
fi

echo "== Build GenDB storage tools =="
run_bounded g++ ${INGEST_FLAGS} \
    -o "${BIN_DIR}/ingest" \
    "${PAPER_RUN}/generated_ingest/ingest.cpp"
run_bounded g++ ${INGEST_FLAGS} \
    -o "${BIN_DIR}/build_indexes" \
    "${PAPER_RUN}/generated_ingest/build_indexes.cpp"
run_bounded g++ ${INGEST_FLAGS} \
    -o "${BIN_DIR}/build_ext_compact" \
    "${PAPER_RUN}/queries/Q1/iter_3/build_ext_compact.cpp"
run_bounded g++ ${INGEST_FLAGS} \
    -o "${BIN_DIR}/build_ext_oky_nibble" \
    "${PAPER_RUN}/queries/Q9/iter_2/build_ext_oky_nibble.cpp"

echo "== Build GenDB storage =="
if [[ ! -s "${GENDB_DIR}/lineitem/l_shipdate.bin" ]]; then
    run_bounded "${BIN_DIR}/ingest" "${DATA_DIR}" "${GENDB_DIR}"
else
    echo "Base columns already exist; skipping ingest."
fi
if [[ ! -s "${GENDB_DIR}/indexes/lineitem_orderkey_sorted.bin" ]]; then
    run_bounded "${BIN_DIR}/build_indexes" "${GENDB_DIR}"
else
    echo "Base indexes already exist; skipping index build."
fi
if [[ ! -s "${GENDB_DIR}/column_versions/lineitem.l_extendedprice.int32/price.bin" ]]; then
    run_bounded "${BIN_DIR}/build_ext_compact" "${GENDB_DIR}"
else
    echo "Compact Q1 columns already exist; skipping extension build."
fi
if [[ ! -s "${GENDB_DIR}/column_versions/orders.o_orderkey_to_year_nibble/oky_nibble.bin" ]]; then
    mkdir -p "${GENDB_DIR}/column_versions/orders.o_orderkey_to_year_nibble"
    run_bounded "${BIN_DIR}/build_ext_oky_nibble" "${GENDB_DIR}"
else
    echo "Q9 order-year nibble already exists; skipping extension build."
fi

echo "== Build selected GenDB query binaries =="
Q1_VECTOR_REPORT_TMP="/tmp/gendb_q1_vectorization_$$.txt"
rm -f "${Q1_VECTOR_REPORT_TMP}"
while read -r query_id query iteration; do
    extra_flags=()
    if [[ "${query}" == "q1" ]]; then
        extra_flags+=("-fopt-info-vec-optimized=${Q1_VECTOR_REPORT_TMP}")
    fi
    run_bounded g++ ${QUERY_FLAGS} \
        "${extra_flags[@]}" \
        -I"${PAPER_ROOT}/src/gendb/utils" \
        -o "${BIN_DIR}/${query}" \
        "${PAPER_RUN}/queries/${query_id}/iter_${iteration}/${query}.cpp" \
        ${QUERY_LDFLAGS}
done <<'EOF'
Q1 q1 3
Q3 q3 1
Q6 q6 0
Q9 q9 2
Q18 q18 1
EOF
mv "${Q1_VECTOR_REPORT_TMP}" "${EVIDENCE_DIR}/q1-vectorization.txt"

echo "== Build GenDB iteration binaries for benefit decomposition =="
for query in Q1 Q3 Q6 Q9 Q18; do
    mkdir -p "${BIN_DIR}/iterations/${query}"
    for source in "${PAPER_RUN}/queries/${query}"/iter_*/"${query,,}.cpp"; do
        iteration="$(basename "$(dirname "${source}")")"
        run_bounded g++ ${QUERY_FLAGS} \
            -I"${PAPER_ROOT}/src/gendb/utils" \
            -o "${BIN_DIR}/iterations/${query}/${iteration}" \
            "${source}" \
            ${QUERY_LDFLAGS}
    done
done

echo "== Verify observable query binaries =="
for query in q1 q3 q6 q9 q18; do
    file "${BIN_DIR}/${query}"
    nm -C "${BIN_DIR}/${query}" > "${EVIDENCE_DIR}/${query}-symbols.txt"
    grep -Eq ' main$' "${EVIDENCE_DIR}/${query}-symbols.txt"
    grep -Eq 'omp_get|GOMP_parallel' "${EVIDENCE_DIR}/${query}-symbols.txt"
done

echo "== Verify Q1 numeric and code-generation contracts =="
"${PYTHON_BIN}" "${HARNESS_DIR}/q1_numeric_contract.py" \
    --output "${EVIDENCE_DIR}/q1-numeric-contract.json"
sha256sum \
    "${PAPER_RUN}/queries/Q1/iter_3/q1.cpp" \
    "${BIN_DIR}/q1" \
    > "${EVIDENCE_DIR}/q1-source-binary-sha256.txt"
objdump --demangle -d --no-show-raw-insn -Mintel "${BIN_DIR}/q1" |
    sed -n '/<main>:/,/^$/p' > "${EVIDENCE_DIR}/q1-main-disassembly.txt"
objdump --demangle -d --no-show-raw-insn -Mintel "${BIN_DIR}/q1" |
    sed -n '/<main\._omp_fn\.0>:/,/^$/p' \
        > "${EVIDENCE_DIR}/q1-kernel-disassembly.txt"
test -s "${EVIDENCE_DIR}/q1-main-disassembly.txt"
test -s "${EVIDENCE_DIR}/q1-kernel-disassembly.txt"

{
    if compgen -G '/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor' > /dev/null; then
        sort -u /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
    else
        echo "unavailable: cpufreq governor is not exposed by this KVM guest"
    fi
} > "${EVIDENCE_DIR}/frequency-policy.txt"

echo "== Build DuckDB database from the same input =="
if [[ ! -s "${DUCKDB_PATH}" ]]; then
    run_bounded numactl --cpunodebind=0,1 --interleave=all \
        taskset -c 0-63 \
        "${PYTHON_BIN}" "${HARNESS_DIR}/tpch_bench.py" setup-duckdb \
            --data-dir "${DATA_DIR}" \
            --database "${DUCKDB_PATH}" \
            --schema "${PAPER_ROOT}/benchmarks/tpc-h/schema.sql" \
            --evidence "${EVIDENCE_DIR}/duckdb-setup.json" \
            --threads "${THREADS}"
else
    echo "DuckDB database already exists; skipping load."
fi

echo "== Artifact manifest =="
{
    find "${BIN_DIR}" -type f -print0
    find "${GENDB_DIR}" -type f -print0
} | sort -z | xargs -0 sha256sum > "${ARTIFACT_MANIFEST_FILE}"

{
    find "${PAPER_RUN}/generated_ingest" \
        "${PAPER_RUN}/queries/Q1" \
        "${PAPER_RUN}/queries/Q3" \
        "${PAPER_RUN}/queries/Q6" \
        "${PAPER_RUN}/queries/Q9" \
        "${PAPER_RUN}/queries/Q18" \
        "${PAPER_ROOT}/src/gendb/utils" \
        -type f \( -name '*.cpp' -o -name '*.h' \) -print0
    printf '%s\0' "${PAPER_ROOT}/benchmarks/tpc-h/schema.sql"
} | sort -z | xargs -0 sha256sum > "${SOURCE_MANIFEST_FILE}"

find "${PAPER_ROOT}/benchmarks/tpc-h/query_results" -type f -name 'Q*.csv' \
    -print0 | sort -z | xargs -0 sha256sum > "${REFERENCE_MANIFEST_FILE}"

sha256sum \
    "${HARNESS_DIR}/prepare_remote.sh" \
    "${HARNESS_DIR}/q1_numeric_contract.py" \
    "${HARNESS_DIR}/run_remote.sh" \
    "${HARNESS_DIR}/test_run_remote.sh" \
    "${HARNESS_DIR}/test_tpch_bench.py" \
    "${HARNESS_DIR}/tpch_bench.py" \
    "${HARNESS_DIR}/verify_dbgen_filc.sh" \
    > "${HARNESS_MANIFEST_FILE}"

cat > "${EVIDENCE_DIR}/revisions-and-flags.txt" <<EOF
gendb_commit=${GENDB_COMMIT}
dbgen_commit=${DBGEN_COMMIT}
dbgen_flags=${DBGEN_FLAGS}
ingest_flags=${INGEST_FLAGS}
query_flags=${QUERY_FLAGS}
query_ldflags=${QUERY_LDFLAGS}
threads=${THREADS}
numa_policy=cpunodebind=0,1 interleave=all
cpu_affinity=0-63
python_bin=${PYTHON_BIN}
EOF

EXPECTED_DUCKDB_SHA="$(
    "${PYTHON_BIN}" -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["database_sha256"])' \
        "${EVIDENCE_DIR}/duckdb-setup.json"
)"
"${PYTHON_BIN}" - \
    "${PREPARATION_MANIFEST_FILE}.tmp" \
    "${PREPARATION_KEY}" \
    "${GENDB_COMMIT}" \
    "${DBGEN_COMMIT}" \
    "${DBGEN_FLAGS}" \
    "${INGEST_FLAGS}" \
    "${QUERY_FLAGS}" \
    "${QUERY_LDFLAGS}" \
    "${PYTHON_BIN}" \
    "${ARTIFACT_MANIFEST_FILE}" \
    "${SOURCE_MANIFEST_FILE}" \
    "${REFERENCE_MANIFEST_FILE}" \
    "${HARNESS_MANIFEST_FILE}" \
    "${EVIDENCE_DIR}/duckdb-setup.json" \
    "${EXPECTED_DUCKDB_SHA}" \
    "${FILC_EVIDENCE_FILE}" \
    "${EVIDENCE_DIR}/cpu-identity.txt" <<'PY'
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path

(
    output,
    preparation_key,
    gendb_commit,
    dbgen_commit,
    dbgen_flags,
    ingest_flags,
    query_flags,
    query_ldflags,
    python_bin,
    artifact_manifest,
    source_manifest,
    reference_manifest,
    harness_manifest,
    duckdb_setup,
    duckdb_database_sha256,
    filc_evidence,
    cpu_identity,
) = sys.argv[1:]


def sha256(path: str) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


payload = {
    "schema_version": 2,
    "status": "complete",
    "preparation_key": preparation_key,
    "gendb_commit": gendb_commit,
    "dbgen_commit": dbgen_commit,
    "dbgen_flags": dbgen_flags,
    "ingest_flags": ingest_flags,
    "query_flags": query_flags,
    "query_ldflags": query_ldflags,
    "compiler": subprocess.check_output(
        ["g++", "--version"], text=True
    ).splitlines()[0],
    "python_bin": python_bin,
    "python_version": platform.python_version(),
    "duckdb_version": __import__("duckdb").__version__,
    "artifact_manifest_sha256": sha256(artifact_manifest),
    "source_manifest_sha256": sha256(source_manifest),
    "reference_manifest_sha256": sha256(reference_manifest),
    "harness_manifest_sha256": sha256(harness_manifest),
    "duckdb_setup_sha256": sha256(duckdb_setup),
    "duckdb_database_sha256": duckdb_database_sha256,
    "filc_evidence_sha256": sha256(filc_evidence),
    "cpu_identity_sha256": sha256(cpu_identity),
    "hostname": platform.node(),
    "python_executable_sha256": sha256(python_bin),
    "duckdb_engine_version": __import__("duckdb").sql(
        "PRAGMA version"
    ).fetchone()[0],
}
Path(output).write_text(
    json.dumps(payload, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY
mv "${PREPARATION_MANIFEST_FILE}.tmp" "${PREPARATION_MANIFEST_FILE}"
printf '%s\n' "${PREPARATION_KEY}" > "${PREPARATION_KEY_FILE}.tmp"
mv "${PREPARATION_KEY_FILE}.tmp" "${PREPARATION_KEY_FILE}"
rm -f \
    "${PREPARATION_KEY_FILE}.previous" \
    "${PREPARATION_MANIFEST_FILE}.previous"

echo "Preparation complete: ${ROOT}"
