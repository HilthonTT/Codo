#!/usr/bin/env bash
# Replace an existing todo.
#   Usage: ./update_todo.sh <id> "<title>" [completed]
#   Example: ./update_todo.sh 1 "buy oat milk" true
# Override the base URL with the BASE_URL env var.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <id> \"<title>\" [completed]" >&2
  exit 1
fi

id="$1"
title="$2"
completed="${3:-false}"

# Escape backslashes and double quotes so the title is valid JSON.
escaped_title=${title//\\/\\\\}
escaped_title=${escaped_title//\"/\\\"}

curl -sS -X PUT "${BASE_URL}/api/todos/${id}" \
  -H "Content-Type: application/json" \
  -d "{\"title\":\"${escaped_title}\",\"completed\":${completed}}"
echo
