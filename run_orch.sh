#!/usr/bin/env bash

set -euo pipefail

iterations="${1:-30}"
work_items="${2:-120}"
pause_ms="${3:-2000}"

exec ./orchestrator "$iterations" "$work_items" "$pause_ms"
