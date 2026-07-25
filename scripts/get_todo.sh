#!/usr/bin/env bash
# Fetch a single todo by id.
#   Usage: ./get_todo.sh <id>
#   Example: ./get_todo.sh 1
# Requires a JWT in the TOKEN env var (see login.sh / register.sh).
# Override the base URL with the BASE_URL env var.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"

if [[ -z "${TOKEN:-}" ]]; then
  echo "TOKEN is not set; log in first: TOKEN=\$(./login.sh <user> <pass> | sed 's/.*\"token\":\"\\([^\"]*\\)\".*/\\1/')" >&2
  exit 1
fi

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <id>" >&2
  exit 1
fi

curl -sS -H "Authorization: Bearer ${TOKEN}" "${BASE_URL}/api/todos/$1"
echo
