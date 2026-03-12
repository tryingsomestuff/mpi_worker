#!/usr/bin/env bash

set -euo pipefail

count="${1:-1}"

if [[ ! -x ./worker ]]; then
	echo "binaire worker introuvable: lancez d'abord ./build.sh" >&2
	exit 1
fi

mkdir -p worker_logs

if [[ ! -d worker_requests ]]; then
	echo "worker_requests introuvable: le maître n'est probablement pas prêt" >&2
	exit 1
fi

if ! [[ "$count" =~ ^[0-9]+$ ]] || [[ "$count" -lt 1 ]]; then
	echo "Usage: $0 [nombre_de_workers>=1]" >&2
	exit 1
fi

for ((i = 0; i < count; ++i)); do
	ticket="worker_requests/request.$$.${i}.$(date +%s%N)"
	log_file="worker_logs/worker.$$.${i}.$(date +%s%N).log"
	: > "$ticket"
	echo "worker demandé, ticket: $ticket"
	nohup ./worker > "$log_file" 2>&1 &
	echo "worker lancé, log: $log_file"
done

echo "$count worker(s) demandé(s). Ils seront pris en compte au début de la prochaine itération."

