#!/usr/bin/env bash

set -euo pipefail

count="${1:-1}"

if [[ ! -d worker_kill_requests ]]; then
    echo "worker_kill_requests introuvable: le maître n'est probablement pas prêt" >&2
    exit 1
fi

if ! [[ "$count" =~ ^[0-9]+$ ]] || [[ "$count" -lt 1 ]]; then
    echo "Usage: $0 [nombre_de_workers_a_arreter>=1]" >&2
    exit 1
fi

for ((i = 0; i < count; ++i)); do
    ticket="worker_kill_requests/request.$$.${i}.$(date +%s%N)"
    : > "$ticket"
    echo "arrêt demandé, ticket: $ticket"
done

echo "$count worker(s) logiques seront désactivés au début de la prochaine itération."
