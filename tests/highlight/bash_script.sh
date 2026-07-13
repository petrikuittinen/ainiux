#!/usr/bin/env bash
set -euo pipefail

name=${1:-world}
printf 'Hello, %s!\n' "$name"

cat <<EOF
Home: $HOME
EOF
