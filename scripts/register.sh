#!/usr/bin/env bash
# Register a new user. Prints the JSON response, which includes a JWT.
#   Usage: ./register.sh <username> <password>
#   Example: TOKEN=$(./register.sh alice wonderland123 | sed 's/.*"token":"\([^"]*\)".*/\1/')
# Override the base URL with the BASE_URL env var.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <username> <password>" >&2
  exit 1
fi

curl -sS -X POST "${BASE_URL}/api/auth/register" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$1\",\"password\":\"$2\"}"
echo
