#!/usr/bin/env bash

set -euo pipefail

REMOTE_HOST="d2"
REMOTE_ROOT="/root/Documents"
PROJECT_PATTERN="*"
DEST_DIR="projects"

usage() {
  cat <<'EOF'
Usage:
  bash ops.sh [options]

Options:
  -p, --project-pattern <pattern>  Project glob under remote root. Default: *
  -r, --remote-root <path>         Remote root directory. Default: /root/Documents
  -H, --host <host>                Remote host. Default: d2
  -d, --dest <path>                Local destination directory. Default: projects
  -h, --help                       Show this help message.

Examples:
  bash ops.sh
  bash ops.sh -p stream_engine
  bash ops.sh --project-pattern 'stream_*'
  bash ops.sh --host v23 --remote-root /data24/otf --project-pattern stream_engine
EOF
}

die() {
  echo "error: $*" >&2
  echo >&2
  usage >&2
  exit 2
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -p|--project-pattern)
        [[ $# -ge 2 ]] || die "$1 requires a value"
        PROJECT_PATTERN="$2"
        shift 2
        ;;
      -r|--remote-root)
        [[ $# -ge 2 ]] || die "$1 requires a value"
        REMOTE_ROOT="$2"
        shift 2
        ;;
      -H|--host)
        [[ $# -ge 2 ]] || die "$1 requires a value"
        REMOTE_HOST="$2"
        shift 2
        ;;
      -d|--dest)
        [[ $# -ge 2 ]] || die "$1 requires a value"
        DEST_DIR="$2"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      --)
        shift
        break
        ;;
      -*)
        die "unknown option: $1"
        ;;
      *)
        die "unexpected argument: $1"
        ;;
    esac
  done
}

validate_args() {
  [[ -n "$REMOTE_HOST" ]] || die "--host cannot be empty"
  [[ -n "$REMOTE_ROOT" ]] || die "--remote-root cannot be empty"
  [[ -n "$PROJECT_PATTERN" ]] || die "--project-pattern cannot be empty"
  [[ -n "$DEST_DIR" ]] || die "--dest cannot be empty"
}

main() {
  parse_args "$@"
  validate_args

  local remote_path="${REMOTE_ROOT%/}/./${PROJECT_PATTERN}/docs"

  echo "sync: ${REMOTE_HOST}:${remote_path} -> ${DEST_DIR}/"
  rsync -avrR "${REMOTE_HOST}:${remote_path}" "${DEST_DIR}/"
}

main "$@"
