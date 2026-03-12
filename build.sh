#!/usr/bin/env bash

set -euo pipefail

compiler="$(command -v c++ || command -v g++)"

if [[ -z "$compiler" ]]; then
	echo "Aucun compilateur C++ trouvé" >&2
	exit 1
fi

echo "Compilateur C++: $compiler"
"$compiler" orchestrator.cc -O3 -std=c++17 -o orchestrator
"$compiler" worker.cc -O3 -std=c++17 -o worker