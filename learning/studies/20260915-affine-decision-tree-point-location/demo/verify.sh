#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(git -C "$script_dir" rev-parse --show-toplevel)
study_rel=learning/studies/20260915-affine-decision-tree-point-location
demo_rel=$study_rel/demo
demo_dir=$repo_root/$demo_rel
manifest_rel=$demo_rel/source-manifest.sha256
manifest=$repo_root/$manifest_rel
build_root=$repo_root/.tmp/affine-decision-tree/verify

ZIG=${ZIG:-$(command -v zig || true)}
FILCC=${FILCC:-$repo_root/.tmp/fil-c/bin/filcc}
FILRUN=${FILRUN:-$repo_root/.tmp/fil-c/bin/filrun}
CMAKE=${CMAKE:-$(command -v cmake || true)}
CTEST=${CTEST:-$(command -v ctest || true)}
NINJA=${NINJA:-$(command -v ninja || true)}
PYENV=${PYENV:-$(command -v pyenv || true)}
UV=${UV:-$(command -v uv || true)}
CLANG_FORMAT=${CLANG_FORMAT:-$(command -v clang-format || true)}
RUBY=${RUBY:-$(command -v ruby || true)}
if command -v sha256sum >/dev/null 2>&1; then
    SHA256=$(command -v sha256sum)
    SHA256_ARGS=
else
    SHA256=$(command -v shasum || true)
    SHA256_ARGS='-a 256'
fi

require_executable() {
    if [ -z "$2" ] || [ ! -x "$2" ]; then
        printf 'missing executable for %s: %s\n' "$1" "$2" >&2
        exit 1
    fi
}

require_executable zig "$ZIG"
require_executable filcc "$FILCC"
require_executable filrun "$FILRUN"
require_executable cmake "$CMAKE"
require_executable ctest "$CTEST"
require_executable ninja "$NINJA"
require_executable pyenv "$PYENV"
require_executable uv "$UV"
require_executable clang-format "$CLANG_FORMAT"
require_executable ruby "$RUBY"
require_executable sha256 "$SHA256"

sha256() {
    # SHA256_ARGS is intentionally split: shasum needs two arguments here.
    "$SHA256" $SHA256_ARGS "$@"
}

sha256_check() {
    "$SHA256" $SHA256_ARGS -c "$1"
}

rm -rf "$build_root"
mkdir -p "$build_root"

actual_inputs=$build_root/actual-inputs.txt
manifest_inputs=$build_root/manifest-inputs.txt
find "$demo_dir" -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.py' -o -name '*.sh' \
    -o -name 'CMakeLists.txt' -o -name '.clang-format' -o -name '.python-version' \
    -o -name 'pyproject.toml' -o -name 'uv.lock' \) \
    -not -path '*/.venv/*' -not -path '*/__pycache__/*' |
    sed "s|^$repo_root/||" | LC_ALL=C sort >"$actual_inputs"
awk '{print $2}' "$manifest" | LC_ALL=C sort >"$manifest_inputs"
if ! cmp -s "$actual_inputs" "$manifest_inputs"; then
    printf '%s\n' 'source manifest input set is stale:' >&2
    diff -u "$manifest_inputs" "$actual_inputs" >&2 || true
    exit 1
fi

set -x

cd "$repo_root"
sha256_check "$manifest_rel"

printf '%s\n' '== toolchain =='
printf 'repo_root=%s\n' "$repo_root"
printf 'sha256=%s\n' "$SHA256"
printf 'zig=%s\n' "$ZIG"
"$ZIG" version
sha256 "$ZIG"
printf 'filcc=%s\n' "$FILCC"
"$FILCC" --version
printf 'filrun=%s\n' "$FILRUN"
sha256 "$FILRUN"
printf 'cmake=%s\n' "$CMAKE"
"$CMAKE" --version
printf 'ctest=%s\n' "$CTEST"
"$CTEST" --version
printf 'ninja=%s\n' "$NINJA"
"$NINJA" --version
printf 'pyenv=%s\n' "$PYENV"
"$PYENV" --version
python_prefix=$(PYENV_VERSION=3.13.12 "$PYENV" prefix)
python=$python_prefix/bin/python3
require_executable python "$python"
printf 'python=%s\n' "$python"
"$python" --version
printf 'uv=%s\n' "$UV"
"$UV" --version
printf 'clang_format=%s\n' "$CLANG_FORMAT"
"$CLANG_FORMAT" --version
printf 'ruby=%s\n' "$RUBY"
"$RUBY" --version

include_dir=$demo_dir/include
odt_source=$demo_dir/src/odt.c
example_source=$demo_dir/src/point_location_example.c
main_source=$demo_dir/src/main.c
test_source=$demo_dir/tests/odt_test.c

printf '%s\n' '== FIL-C =='
"$FILCC" -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -I "$include_dir" "$odt_source" "$example_source" "$test_source" \
    -lm -o "$build_root/filc-odt-test"
sha256 "$build_root/filc-odt-test"
"$FILRUN" "$build_root/filc-odt-test"

printf '%s\n' '== Zig 0.16.0 release =='
test "$("$ZIG" version)" = 0.16.0
"$ZIG" cc -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -I "$include_dir" "$odt_source" "$example_source" "$test_source" \
    -lm -o "$build_root/zig-odt-test"
"$ZIG" cc -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -I "$include_dir" "$odt_source" "$example_source" "$main_source" \
    -lm -o "$build_root/zig-point-location"
sha256 "$build_root/zig-odt-test" "$build_root/zig-point-location"
"$build_root/zig-odt-test"
"$build_root/zig-point-location"
printf '2 2\n5 5\n-1 4\n' | "$build_root/zig-point-location" --machine

printf '%s\n' '== Zig ASan/UBSan =='
"$ZIG" cc -std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
    -Wall -Wextra -Wpedantic -Werror \
    -I "$include_dir" "$odt_source" "$example_source" "$test_source" \
    -lm -o "$build_root/zig-odt-test-sanitized"
sha256 "$build_root/zig-odt-test-sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    "$build_root/zig-odt-test-sanitized"

printf '%s\n' '== Python 3.13.12 differential and CLI tests =='
(
    cd "$demo_dir/python"
    UV_PROJECT_ENVIRONMENT="$build_root/python-venv" \
        "$UV" sync --frozen --python "$python"
    POINT_LOCATION_BIN="$build_root/zig-point-location" \
        UV_PROJECT_ENVIRONMENT="$build_root/python-venv" \
        "$UV" run --frozen --python "$python" python -m unittest discover -s tests -v
)

printf '%s\n' '== CMake/CTest =='
"$CMAKE" -S "$demo_dir" -B "$build_root/cmake" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MAKE_PROGRAM="$NINJA"
"$CMAKE" --build "$build_root/cmake"
"$CTEST" --test-dir "$build_root/cmake" --output-on-failure
sha256 "$build_root/cmake/odt_test" "$build_root/cmake/point_location"

printf '%s\n' '== formatting =='
"$CLANG_FORMAT" --dry-run --Werror \
    "$demo_dir/include/odt.h" "$demo_dir/include/point_location_example.h" \
    "$demo_dir/src/main.c" "$demo_dir/src/odt.c" "$demo_dir/src/point_location_example.c" \
    "$demo_dir/tests/odt_test.c"

printf '%s\n' '== document DAG =='
"$RUBY" - "$repo_root" "$study_rel" <<'RUBY'
require "yaml"

repo_root, study_rel = ARGV
markdown_paths = IO.popen(
  ["git", "-C", repo_root, "ls-files", "--cached", "--others", "--exclude-standard",
   "--", "*.md"],
  &:read
).lines(chomp: true).sort

documents = {}
ids = {}
markdown_paths.each do |path|
  absolute_path = File.join(repo_root, path)
  next unless File.file?(absolute_path)

  text = File.read(absolute_path)
  match = text.match(/\A---\n(.*?)\n---\n/m)
  next unless match

  metadata = YAML.load(match[1])
  next unless metadata.is_a?(Hash) && metadata["doc_id"]

  doc_id = metadata.fetch("doc_id")
  abort("duplicate doc_id #{doc_id}: #{ids.fetch(doc_id)} and #{path}") if ids.key?(doc_id)
  ids[doc_id] = path
  documents[path] = metadata
end

path_to_id = ids.invert
edges = Hash.new { |hash, key| hash[key] = [] }
documents.each do |path, metadata|
  Array(metadata["depends_on"]).each do |dependency|
    dependency_path =
      if File.file?(File.join(repo_root, dependency))
        dependency
      elsif ids.key?(dependency)
        ids.fetch(dependency)
      else
        abort("unresolved dependency #{dependency.inspect} in #{path}")
      end
    edges[metadata.fetch("doc_id")] << path_to_id.fetch(dependency_path) if path_to_id.key?(dependency_path)
  end

  Array(metadata["verified_by"]).each do |verifier|
    next unless verifier.is_a?(String) && verifier.include?("/") && !verifier.include?("://")
    abort("unresolved verifier path #{verifier.inspect} in #{path}") \
      unless File.exist?(File.join(repo_root, verifier))
  end
end

state = {}
visit = lambda do |doc_id, stack|
  case state[doc_id]
  when :active
    abort("dependency cycle: #{(stack + [doc_id]).join(' -> ')}")
  when :done
    next
  end

  state[doc_id] = :active
  edges[doc_id].each { |dependency| visit.call(dependency, stack + [doc_id]) }
  state[doc_id] = :done
end
ids.each_key { |doc_id| visit.call(doc_id, []) }

archive_path = "learning/sources/20260915-affine-decision-tree-point-location.md"
study_documents = documents.keys.select do |path|
  path == archive_path || path.start_with?("#{study_rel}/")
end
abort("expected 5 study documents, found #{study_documents.length}") unless study_documents.length == 5

puts "document DAG passed: #{ids.length} unique doc IDs scanned"
puts "study subgraph passed: #{study_documents.length} documents; dependencies and verifier paths resolve; no cycles"
RUBY

printf '%s\n' 'verification passed'
