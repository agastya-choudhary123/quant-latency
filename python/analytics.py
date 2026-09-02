"""Performance analytics + plots for a trades CSV produced by either backtester.

Computes the same stats as cpp/include/strategy.hpp compute_stats (used to
cross-check), plus an equity-curve / drawdown plot saved to PNG.
"""
from __future__ import annotations
import argparse
import numpy as np
import pandas as pd


def compute_stats(trades: pd.DataFrame) -> dict:
    if trades.empty:
        return {"num_trades": 0, "total_pnl": 0.0, "gross_funding": 0.0,
                "total_fees": 0.0, "total_slippage": 0.0, "win_rate": 0.0,
                "avg_pnl": 0.0, "max_drawdown": 0.0, "sharpe": 0.0}
    pnl = trades["pnl"].to_numpy()
    cum = np.cumsum(pnl)
    peak = np.maximum.accumulate(cum)
    max_dd = float(np.max(peak - cum))
    sd = float(np.std(pnl, ddof=1)) if len(pnl) >= 2 else 0.0
    sharpe = float(np.mean(pnl) / sd) if sd > 0 else 0.0
    return {
        "num_trades": int(len(trades)),
        "total_pnl": float(pnl.sum()),
        "gross_funding": float(trades["funding_collected"].sum()),
        "total_fees": float(trades["fees"].sum()),
        "total_slippage": float(trades["slippage"].sum()),
        "win_rate": float((pnl > 0).mean()),
        "avg_pnl": float(pnl.mean()),
        "max_drawdown": max_dd,
        "sharpe": sharpe,
    }


def print_stats(s: dict) -> None:
    print("=== funding-arb performance ===")
    print(f"trades          : {s['num_trades']}")
    print(f"gross funding   : {s['gross_funding']:.2f}")
    print(f"fees            : {s['total_fees']:.2f}")
    print(f"slippage        : {s['total_slippage']:.2f}")
    print(f"net pnl         : {s['total_pnl']:.2f}")
    print(f"win rate        : {s['win_rate']*100:.1f}%")
    print(f"avg pnl/trade   : {s['avg_pnl']:.2f}")
    print(f"max drawdown    : {s['max_drawdown']:.2f}")
    print(f"per-trade sharpe: {s['sharpe']:.4f}")


def plot(trades: pd.DataFrame, out_png: str) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    pnl = trades["pnl"].to_numpy()
    cum = np.cumsum(pnl)
    peak = np.maximum.accumulate(cum)
    dd = peak - cum

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    ax1.plot(cum, color="tab:blue", lw=1.5)
    ax1.set_title("Cumulative PnL (funding-arb)")
    ax1.set_ylabel("PnL (quote ccy)")
    ax1.grid(alpha=0.3)
    ax2.fill_between(range(len(dd)), 0, -dd, color="tab:red", alpha=0.5)
    ax2.set_title("Drawdown")
    ax2.set_ylabel("Drawdown")
    ax2.set_xlabel("Trade #")
    ax2.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    print(f"wrote plot -> {out_png}")


def main() -> None:
    ap = argparse.ArgumentParser(description="analytics for a trades CSV")
    ap.add_argument("trades_csv")
    ap.add_argument("--plot", metavar="PNG", default=None)
    args = ap.parse_args()
    trades = pd.read_csv(args.trades_csv)
    print_stats(compute_stats(trades))
    if args.plot and not trades.empty:
        plot(trades, args.plot)


if __name__ == "__main__":
    main()
