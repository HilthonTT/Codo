#!/usr/bin/env bash
# Fetch a single todo by id.
#   Usage: ./get_todo.sh <id>
#   Example: ./get_todo.sh 1
# Override the base URL with the BASE_URL env var.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <id>" >&2
  exit 1
fi

curl -sS "${BASE_URL}/api/todos/$1"
echo
