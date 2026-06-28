#!/usr/bin/env bash
# List all todos.
#   Usage: ./list_todos.sh
# Override the base URL with the BASE_URL env var.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"

curl -sS "${BASE_URL}/api/todos"
echo
