#!/usr/bin/env bash
# Build and run the host unit tests. Exit status is the tests' own verdict,
# so `tests/run_tests.sh && git commit ...` is a safe gate.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -O1 -Wall -Wextra -Werror \
	"$here/test_distance_model.cpp" "$here/../src/Sound/DistanceModel.cpp" \
	-o "$out/test_distance_model"
"$out/test_distance_model"
python3 -m unittest "$here/test_distance_bake.py"
