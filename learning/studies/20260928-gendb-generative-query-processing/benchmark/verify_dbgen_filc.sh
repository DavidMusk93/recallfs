#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-/data00/benchmarks/gendb-tpch-sf10-20260928}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
FILC_VERSION="0.684"
FILC_ARCHIVE_SHA256="eefb594bcbc1261a18dfa8b50041674635f53df2b5fe067915b5652adaed4e3f"
FILC_URL="https://github.com/pizlonator/fil-c/releases/download/v${FILC_VERSION}/filc-${FILC_VERSION}-linux-x86_64.tar.xz"
CONTAINER_IMAGE="debian@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a"
TOOL_ROOT="${FILC_TOOL_ROOT:-${ROOT}/.tmp/fil-c/fil-c-${FILC_VERSION}}"
ARCHIVE="${TOOL_ROOT}/filc-${FILC_VERSION}-linux-x86_64.tar.xz"
DIST="${TOOL_ROOT}/dist"
FILCC="${DIST}/build/bin/filcc"
SOURCE="${ROOT}/tpch-dbgen"
WORK="${TOOL_ROOT}/dbgen-smoke"
FILC_WORK="${WORK}/filc"
NATIVE_WORK="${WORK}/native"
EVIDENCE_DIR="${ROOT}/evidence"
EVIDENCE="${EVIDENCE_DIR}/filc-dbgen-smoke.json"
LOG="${EVIDENCE_DIR}/filc-dbgen-smoke.log"

if [[ "${PYTHON_BIN}" != /* ]]; then
    PYTHON_BIN="$(command -v "${PYTHON_BIN}")"
fi
for command in curl docker gcc git make patchelf sha256sum tar; do
    command -v "${command}" >/dev/null
done
test -x "${PYTHON_BIN}"
test "$(git -C "${SOURCE}" rev-parse HEAD)" = \
    "32f1c1b92d1664dba542e927d23d86ffa57aa253"
test -z "$(git -C "${SOURCE}" status --porcelain --untracked-files=no)"

mkdir -p "${TOOL_ROOT}" "${EVIDENCE_DIR}"
exec > >(tee "${LOG}") 2>&1

if [[ ! -s "${ARCHIVE}" ]]; then
    curl -fL --retry 5 --connect-timeout 30 --max-time 900 \
        "${FILC_URL}" -o "${ARCHIVE}.tmp"
    mv "${ARCHIVE}.tmp" "${ARCHIVE}"
fi
printf '%s  %s\n' "${FILC_ARCHIVE_SHA256}" "${ARCHIVE}" | sha256sum -c -

rm -rf "${DIST}"
mkdir -p "${DIST}"
tar -xJf "${ARCHIVE}" -C "${DIST}" --strip-components=1
(
    cd "${DIST}"
    ./setup.sh
)
test -x "${FILCC}"
FILCC_SHA256="$(sha256sum "${FILCC}" | awk '{print $1}')"

if ! docker image inspect "${CONTAINER_IMAGE}" >/dev/null 2>&1; then
    docker pull "${CONTAINER_IMAGE}"
fi

HOST_LD="$(command -v ld)"
HOST_MAKE="$(command -v make)"
HOST_LIBBFD="$(ldd "${HOST_LD}" | awk '/libbfd/{print $3; exit}')"
test -x "${HOST_LD}"
test -x "${HOST_MAKE}"
test -f "${HOST_LIBBFD}"

rm -rf "${WORK}"
mkdir -p "${FILC_WORK}" "${NATIVE_WORK}"
git -C "${SOURCE}" archive HEAD | tar -x -C "${FILC_WORK}"
git -C "${SOURCE}" archive HEAD | tar -x -C "${NATIVE_WORK}"

cat > "${WORK}/filc-negative-control.c" <<'EOF'
#include <stdlib.h>

int main(void)
{
    int *value = malloc(sizeof(*value));
    if (!value)
        return 2;
    free(value);
    *value = 7;
    return 0;
}
EOF

echo "== FIL-C instrumentation negative control =="
docker run --rm \
    -v "${HOST_MAKE}:/usr/bin/make:ro" \
    -v "${HOST_LD}:/usr/bin/ld:ro" \
    -v "${HOST_LIBBFD}:${HOST_LIBBFD}:ro" \
    -v "${ROOT}:${ROOT}" \
    -w "${WORK}" \
    "${CONTAINER_IMAGE}" \
    "${FILCC}" -O0 -o filc-negative-control filc-negative-control.c
set +e
docker run --rm \
    -v "${ROOT}:${ROOT}" \
    -w "${WORK}" \
    "${CONTAINER_IMAGE}" \
    ./filc-negative-control > "${WORK}/filc-negative-control.log" 2>&1
NEGATIVE_CONTROL_STATUS=$?
set -e
if [[ "${NEGATIVE_CONTROL_STATUS}" -eq 0 ]]; then
    echo "FIL-C negative control unexpectedly succeeded." >&2
    exit 1
fi
grep -q '^filc safety error:' "${WORK}/filc-negative-control.log"

gcc -O0 -o "${WORK}/native-negative-control" \
    "${WORK}/filc-negative-control.c"
set +e
"${WORK}/native-negative-control"
NATIVE_NEGATIVE_CONTROL_STATUS=$?
set -e
if [[ "${NATIVE_NEGATIVE_CONTROL_STATUS}" -ne 0 ]]; then
    echo "Native negative-control baseline did not complete normally." >&2
    exit 1
fi

echo "== FIL-C build and execution =="
docker run --rm \
    -v "${HOST_MAKE}:/usr/bin/make:ro" \
    -v "${HOST_LD}:/usr/bin/ld:ro" \
    -v "${HOST_LIBBFD}:${HOST_LIBBFD}:ro" \
    -v "${ROOT}:${ROOT}" \
    -w "${FILC_WORK}" \
    "${CONTAINER_IMAGE}" \
    sh -c "make -f makefile -j8 CC=${FILCC} MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH"
(
    cd "${FILC_WORK}"
    docker run --rm \
        -v "${ROOT}:${ROOT}" \
        -w "${FILC_WORK}" \
        "${CONTAINER_IMAGE}" \
        ./dbgen -s 0.01 -f
    mkdir output
    mv ./*.tbl output/
)

echo "== Native oracle =="
make -C "${NATIVE_WORK}" -f makefile -j8 \
    CC=gcc MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH
(
    cd "${NATIVE_WORK}"
    ./dbgen -s 0.01 -f
    mkdir output
    mv ./*.tbl output/
)

(
    cd "${FILC_WORK}/output"
    sha256sum ./*.tbl | sort -k2
) > "${WORK}/filc-sha256.txt"
(
    cd "${NATIVE_WORK}/output"
    sha256sum ./*.tbl | sort -k2
) > "${WORK}/native-sha256.txt"
diff -u "${WORK}/filc-sha256.txt" "${WORK}/native-sha256.txt"

"${PYTHON_BIN}" - \
    "${EVIDENCE}.tmp" \
    "${FILC_ARCHIVE_SHA256}" \
    "${FILCC_SHA256}" \
    "${CONTAINER_IMAGE}" \
    "$(git -C "${SOURCE}" rev-parse HEAD)" \
    "${TOOL_ROOT}" \
    "${NEGATIVE_CONTROL_STATUS}" \
    "${NATIVE_NEGATIVE_CONTROL_STATUS}" \
    "${WORK}/filc-sha256.txt" \
    "${FILC_WORK}/output" <<'PY'
import json
import sys
from pathlib import Path

(
    output,
    archive_sha,
    filcc_sha,
    image,
    commit,
    tool_root,
    negative_control_status,
    native_negative_control_status,
    manifest_path,
    data_dir,
) = sys.argv[1:]
rows = {}
for path in sorted(Path(data_dir).glob("*.tbl")):
    with path.open("rb") as source:
        rows[path.name] = sum(1 for _ in source)
expected_rows = {
    "customer.tbl": 1500,
    "lineitem.tbl": 60175,
    "nation.tbl": 25,
    "orders.tbl": 15000,
    "part.tbl": 2000,
    "partsupp.tbl": 8000,
    "region.tbl": 5,
    "supplier.tbl": 100,
}
if rows != expected_rows:
    raise SystemExit(f"unexpected SF0.01 row counts: {rows}")

manifest = {}
for line in Path(manifest_path).read_text(encoding="utf-8").splitlines():
    digest, name = line.split(maxsplit=1)
    manifest[Path(name).name] = digest

Path(output).write_text(
    json.dumps(
        {
            "schema_version": 1,
            "status": "pass",
            "scope": "TPC-H dbgen SF0.01 executed path",
            "filc_version": "0.684",
            "filc_archive_sha256": archive_sha,
            "filcc_sha256": filcc_sha,
            "filc_tool_root": tool_root,
            "container_image": image,
            "dbgen_commit": commit,
            "dbgen_flags": (
                "MACHINE=MAC DATABASE=ORACLE WORKLOAD=TPCH; "
                "CC differs only between FIL-C and native oracle"
            ),
            "negative_control": "heap-use-after-free",
            "negative_control_detected": True,
            "negative_control_exit_status": int(negative_control_status),
            "native_negative_control_exit_status": int(
                native_negative_control_status
            ),
            "filc_matches_native_sha256": True,
            "native_oracle": "GCC build from the same clean pinned source",
            "row_counts": rows,
            "output_sha256": manifest,
            "performance_evidence": False,
        },
        indent=2,
        sort_keys=True,
    )
    + "\n",
    encoding="utf-8",
)
PY
mv "${EVIDENCE}.tmp" "${EVIDENCE}"

echo "FIL-C dbgen smoke passed: ${EVIDENCE}"
