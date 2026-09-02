#!/usr/bin/env bash
# Run the full test suite: C++ unit tests + Python unit tests + C++/Python
# cross-validation. Run from anywhere.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> C++ unit tests"
make test

echo; echo "==> Python tests + C++/Python cross-validation"
python3 python/test_python.py
