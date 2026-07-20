#!/usr/bin/env bash
# Delete a todo by id.
#   Usage: ./delete_todo.sh <id>
#   Example: ./delete_todo.sh 1
# Prints the HTTP status code (204 on success, 404 if not found).
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

status=$(curl -sS -o /dev/null -w "%{http_code}" -X DELETE \
  -H "Authorization: Bearer ${TOKEN}" "${BASE_URL}/api/todos/$1")
echo "HTTP ${status}"
