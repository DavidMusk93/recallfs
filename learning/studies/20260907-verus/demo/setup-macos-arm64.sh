#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    echo "This setup script supports macOS arm64 only." >&2
    echo "Use the official Verus installation guide on other platforms." >&2
    exit 2
fi

demo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$demo_dir" rev-parse --show-toplevel)"
tmp_dir="$repo_root/.tmp"

verus_version="0.2026.09.06.8dea4a2"
verus_archive="verus-${verus_version}-arm64-macos.zip"
verus_sha256="23be2e8113b50bc91729377d8ac032f658cc5187e389a319d8458f9c37b07b4a"
verus_url="https://github.com/verus-lang/verus/releases/download/release/${verus_version}/${verus_archive}"
verus_download_dir="$tmp_dir/verus-download"
verus_toolchain_dir="$tmp_dir/verus-toolchain"

z3_version="4.16.0"
z3_archive="z3-${z3_version}-arm64-osx-15.7.3.zip"
z3_sha256="41828fa07d5cb77bfaee326e8e6dac074f26329c09c633f9e66012bb917cf8ae"
z3_url="https://github.com/Z3Prover/z3/releases/download/z3-${z3_version}/${z3_archive}"
z3_dir="$tmp_dir/verus-z3"
z3_extract_dir="$z3_dir/${z3_archive%.zip}"

verify_sha256() {
    local file="$1"
    local expected="$2"
    printf '%s  %s\n' "$expected" "$file" | shasum -a 256 --check --status
}

download_verified() {
    local url="$1"
    local output="$2"
    local sha256="$3"

    if [[ -f "$output" ]] && verify_sha256 "$output" "$sha256"; then
        echo "using verified download: $output"
        return
    fi

    rm -f "$output"
    curl -fL --retry 3 --connect-timeout 15 -o "$output" "$url"
    if ! verify_sha256 "$output" "$sha256"; then
        echo "SHA-256 mismatch: $output" >&2
        exit 1
    fi
}

mkdir -p "$verus_download_dir" "$verus_toolchain_dir" "$z3_dir"

download_verified "$verus_url" "$verus_download_dir/$verus_archive" "$verus_sha256"
rm -rf "$verus_toolchain_dir/verus-arm64-macos"
unzip -q "$verus_download_dir/$verus_archive" -d "$verus_toolchain_dir"
xattr -dr com.apple.quarantine "$verus_toolchain_dir/verus-arm64-macos" 2>/dev/null || true

if ! rustup toolchain list | grep -Fq "1.98.0-aarch64-apple-darwin"; then
    rustup toolchain install 1.98.0-aarch64-apple-darwin --profile minimal
fi

download_verified "$z3_url" "$z3_dir/$z3_archive" "$z3_sha256"
rm -rf "$z3_extract_dir"
unzip -q "$z3_dir/$z3_archive" -d "$z3_dir"
cp "$z3_extract_dir/bin/z3" "$z3_dir/z3"
chmod +x "$z3_dir/z3"

"$verus_toolchain_dir/verus-arm64-macos/verus" --version
"$z3_dir/z3" --version
