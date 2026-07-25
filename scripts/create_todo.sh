#!/usr/bin/env bash
# Create a todo.
#   Usage: ./create_todo.sh "<title>" [completed]
#   Example: ./create_todo.sh "buy milk"
#            ./create_todo.sh "walk the dog" true
# Requires a JWT in the TOKEN env var (see login.sh / register.sh).
# Override the base URL with the BASE_URL env var.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"

if [[ -z "${TOKEN:-}" ]]; then
  echo "TOKEN is not set; log in first: TOKEN=\$(./login.sh <user> <pass> | sed 's/.*\"token\":\"\\([^\"]*\\)\".*/\\1/')" >&2
  exit 1
fi

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 \"<title>\" [completed]" >&2
  exit 1
fi

title="$1"
completed="${2:-false}"

# Escape backslashes and double quotes so the title is valid JSON.
escaped_title=${title//\\/\\\\}
escaped_title=${escaped_title//\"/\\\"}

curl -sS -X POST "${BASE_URL}/api/todos" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer ${TOKEN}" \
  -d "{\"title\":\"${escaped_title}\",\"completed\":${completed}}"
echo
