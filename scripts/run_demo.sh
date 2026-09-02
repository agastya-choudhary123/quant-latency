#!/usr/bin/env bash
# End-to-end demo: build, generate data, backtest (C++), route (paper engine),
# sweep + analytics (Python). Run from the project root: scripts/run_demo.sh
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> building C++ engine"
make all

echo; echo "==> generating synthetic funding series (1 year, 8h intervals)"
python3 python/generate_data.py data/funding.csv -n 1095 --seed 7

echo; echo "==> C++ backtest (naive params: entry 10% annual)"
./bin/backtest data/funding.csv data/trades_naive.csv 10000 0.10 0.03 21

echo; echo "==> parameter sweep (research: find a profitable config)"
( cd python && python3 sweep.py ../data/funding.csv --top 5 )

echo; echo "==> C++ backtest (tuned params: entry 20% annual, hold to decay)"
./bin/backtest data/funding.csv data/trades_tuned.csv 10000 0.20 0.00 45

echo; echo "==> paper engine: same signals, latency-routed across 3 venues"
./bin/paper data/funding.csv data/trades_paper.csv 10000 0.20 0.00 45

echo; echo "==> analytics + equity-curve plot"
python3 python/analytics.py data/trades_tuned.csv --plot data/equity_curve.png

echo; echo "==> done. Artifacts in data/  (equity_curve.png, trades_*.csv)"
