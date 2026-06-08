#!/usr/bin/env python3
"""
Join ADIOS2 measurements with DTLMod predictions and quantify model accuracy,
SPLIT BY REGIME (the affine model behaves very differently in each):

  bandwidth regime  (>= 1 MB / rank)  : transfer time dominates -> DTLMod should match
  latency regime    (<  1 MB / rank)  : fixed overheads dominate -> DTLMod's blind spot

Outputs: results/comparison.csv, results/accuracy_summary.txt, analysis/fig_*.png
"""
import csv, statistics, os
from collections import defaultdict
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = "/home/user/dtlmod-verification"
FIGDIR = f"{ROOT}/analysis"
BW_REGIME = 1 << 20   # >= 1 MB per rank counts as bandwidth-dominated
FLOAT = ("open_s", "step_s", "close_s", "bw_GBps", "min_step_s", "med_step_s")
INT = ("nranks", "bytes_per_rank", "steps")


def load(path, keycol):
    rows = []
    for r in csv.DictReader(open(path)):
        if r.get("marker") != "RESULT":
            continue
        try:
            for k in INT: r[k] = int(r[k])
            for k in FLOAT: r[k] = float(r[k])
        except (ValueError, KeyError):
            continue
        r["_engine"] = r[keycol]
        rows.append(r)
    return rows


def amedian(rows):
    g = defaultdict(lambda: defaultdict(list))
    for r in rows:
        k = (r["_engine"], r["role"], r["nranks"], r["bytes_per_rank"])
        for m in ("med_step_s", "min_step_s", "bw_GBps", "open_s", "close_s"):
            g[k][m].append(r[m])
    return {k: {m: statistics.median(v) for m, v in d.items()} for k, d in g.items()}


def dmap(rows):
    return {(r["_engine"], r["role"], r["nranks"], r["bytes_per_rank"]):
            {"med_step_s": r["med_step_s"], "bw_GBps": r["bw_GBps"]} for r in rows}


def main():
    A = amedian(load(f"{ROOT}/results/adios_raw.csv", "engine"))
    D = dmap(load(f"{ROOT}/results/dtl_raw.csv", "tag"))

    rows = []
    for k in sorted(A):
        eng, role, rk, nb = k
        if k not in D:
            continue
        a, d = A[k], D[k]
        se = (d["med_step_s"] - a["med_step_s"]) / a["med_step_s"] if a["med_step_s"] else float("nan")
        be = (d["bw_GBps"] - a["bw_GBps"]) / a["bw_GBps"] if a["bw_GBps"] else float("nan")
        rows.append(dict(engine=eng, role=role, nranks=rk, bytes=nb,
                         a_step=a["med_step_s"], d_step=d["med_step_s"], step_relerr=se,
                         a_bw=a["bw_GBps"], d_bw=d["bw_GBps"], bw_relerr=be))

    with open(f"{ROOT}/results/comparison.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["engine", "role", "nranks", "bytes_per_rank", "adios_step_s",
                    "dtl_step_s", "step_relerr", "adios_bw_GBps", "dtl_bw_GBps", "bw_relerr"])
        for r in rows:
            w.writerow([r["engine"], r["role"], r["nranks"], r["bytes"], r["a_step"],
                        r["d_step"], r["step_relerr"], r["a_bw"], r["d_bw"], r["bw_relerr"]])

    # ---- regime-split accuracy summary ----
    L = ["DTLMod vs ADIOS2 — accuracy by regime", "=" * 64, "",
         "Per-step time relative error |(DTLMod - ADIOS2)/ADIOS2|, medians.", ""]
    L.append(f"{'engine':8s} {'role':6s} | {'BW regime (>=1MB)':>18s} | {'latency regime (<1MB)':>22s}")
    L.append("-" * 64)
    for eng in ("BP5", "SST", "DataMan", "MQ"):
        for role in ("writer", "reader"):
            bw = [abs(r["step_relerr"]) for r in rows if r["engine"] == eng and r["role"] == role
                  and r["bytes"] >= BW_REGIME and np.isfinite(r["step_relerr"])]
            lat = [abs(r["step_relerr"]) for r in rows if r["engine"] == eng and r["role"] == role
                   and r["bytes"] < BW_REGIME and np.isfinite(r["step_relerr"])]
            if not bw and not lat:
                continue
            bs = f"{statistics.median(bw)*100:6.1f}% (n={len(bw)})" if bw else "    --"
            ls = f"{statistics.median(lat)*100:6.1f}% (n={len(lat)})" if lat else "    --"
            L.append(f"{eng:8s} {role:6s} | {bs:>18s} | {ls:>22s}")
    L.append("")
    L.append("Latency floor at smallest message (1 KB), writer, 1 rank:")
    for eng in ("BP5", "SST", "DataMan"):
        ak = (eng, "writer", 1, 1024); dk = ak
        if ak in A and dk in D:
            L.append(f"   {eng:8s}: ADIOS2 {A[ak]['med_step_s']*1e3:8.3f} ms   "
                     f"DTLMod {D[dk]['med_step_s']*1e3:8.3f} ms")
    L.append("")
    L.append("Idealized MQ staging (data movement cost), writer, 1 rank:")
    for nb in (1 << 20, 1 << 24, 1 << 26):
        mq = D.get(("MQ", "writer", 1, nb))
        sst = A.get(("SST", "writer", 1, nb))
        if mq and sst:
            L.append(f"   {nb>>20:4d} MB: MQ predicts {mq['med_step_s']*1e6:8.3f} us  "
                     f"(real SST {sst['med_step_s']*1e3:6.1f} ms)")
    summary = "\n".join(L)
    open(f"{ROOT}/results/accuracy_summary.txt", "w").write(summary + "\n")
    print(summary)
    make_plots(A, D)


def make_plots(A, D):
    engines = [("BP5", "tab:blue"), ("SST", "tab:orange"), ("DataMan", "tab:green")]

    # Fig 1: per-step time & bandwidth vs size (writer, 1 rank)
    fig, ax = plt.subplots(1, 2, figsize=(13, 5))
    for eng, c in engines:
        sz = sorted({k[3] for k in A if k[:3] == (eng, "writer", 1)})
        if not sz: continue
        ax[0].plot([s/1e6 for s in sz], [A[(eng,"writer",1,s)]["med_step_s"]*1e3 for s in sz],
                   "o-", color=c, label=f"{eng} ADIOS2")
        dd = [(s, D[(eng,"writer",1,s)]["med_step_s"]) for s in sz if (eng,"writer",1,s) in D]
        ax[0].plot([s/1e6 for s,_ in dd], [t*1e3 for _,t in dd], "s--", color=c, alpha=.6, label=f"{eng} DTLMod")
    ax[0].set_xscale("log"); ax[0].set_yscale("log")
    ax[0].axvline(1.0, color="gray", ls=":", lw=1); ax[0].text(1.05, ax[0].get_ylim()[0]*1.5, "1 MB", fontsize=8, color="gray")
    ax[0].set_xlabel("message size per rank (MB)"); ax[0].set_ylabel("per-step time (ms)")
    ax[0].set_title("Per-step transfer time vs size (writer, 1 rank)")
    ax[0].legend(fontsize=8); ax[0].grid(True, which="both", alpha=.3)
    for eng, c in engines:
        sz = sorted({k[3] for k in A if k[:3] == (eng, "writer", 1)})
        if not sz: continue
        ax[1].plot([s/1e6 for s in sz], [A[(eng,"writer",1,s)]["bw_GBps"] for s in sz], "o-", color=c, label=f"{eng} ADIOS2")
        dd = [(s, D[(eng,"writer",1,s)]["bw_GBps"]) for s in sz if (eng,"writer",1,s) in D]
        ax[1].plot([s/1e6 for s,_ in dd], [b for _,b in dd], "s--", color=c, alpha=.6, label=f"{eng} DTLMod")
    ax[1].set_xscale("log"); ax[1].set_xlabel("message size per rank (MB)"); ax[1].set_ylabel("aggregate bandwidth (GB/s)")
    ax[1].set_title("Write bandwidth vs size (1 rank)"); ax[1].legend(fontsize=8); ax[1].grid(True, which="both", alpha=.3)
    fig.tight_layout(); fig.savefig(f"{FIGDIR}/fig_size_sweep.png", dpi=110); plt.close(fig)

    # Fig 2: BP5 write rank scaling
    fig, ax = plt.subplots(figsize=(7, 5))
    for s, mk in [(16<<20, "o"), (64<<20, "s"), (128<<20, "^")]:
        ra = [r for r in (1,2,4) if ("BP5","writer",r,s) in A]
        ax.plot(ra, [A[("BP5","writer",r,s)]["bw_GBps"] for r in ra], mk+"-", color="tab:blue", label=f"ADIOS2 {s>>20}MB")
        rd = [r for r in (1,2,4) if ("BP5","writer",r,s) in D]
        ax.plot(rd, [D[("BP5","writer",r,s)]["bw_GBps"] for r in rd], mk+"--", color="tab:red", alpha=.6, label=f"DTLMod {s>>20}MB")
    ax.set_xlabel("publisher ranks"); ax.set_ylabel("aggregate write bandwidth (GB/s)")
    ax.set_title("BP5 / File-engine write rank scaling (single node)")
    ax.set_xticks([1,2,4]); ax.set_ylim(bottom=0); ax.legend(fontsize=8); ax.grid(True, alpha=.3)
    fig.tight_layout(); fig.savefig(f"{FIGDIR}/fig_bp_ranks.png", dpi=110); plt.close(fig)

    # Fig 3: relative error vs size (writer, 1 rank), highlighting the two regimes
    fig, ax = plt.subplots(figsize=(8, 5))
    for eng, c in engines:
        sz = sorted({k[3] for k in A if k[:3] == (eng, "writer", 1) and k in D})
        sz = [s for s in sz if (eng,"writer",1,s) in D]
        if not sz: continue
        err = [(D[(eng,"writer",1,s)]["med_step_s"]-A[(eng,"writer",1,s)]["med_step_s"])/A[(eng,"writer",1,s)]["med_step_s"]*100 for s in sz]
        ax.plot([s/1e6 for s in sz], err, "o-", color=c, label=eng)
    ax.axvspan(1e-3, 1.0, color="orange", alpha=.08); ax.axvspan(1.0, 200, color="green", alpha=.08)
    ax.text(3e-3, 80, "latency regime", fontsize=9, color="darkorange")
    ax.text(8, 80, "bandwidth regime", fontsize=9, color="green")
    ax.axhline(0, color="k", lw=.8); ax.set_xscale("log")
    ax.set_xlabel("message size per rank (MB)"); ax.set_ylabel("DTLMod per-step time error (%)")
    ax.set_title("DTLMod prediction error vs message size (writer, 1 rank)")
    ax.set_ylim(-110, 110); ax.legend(fontsize=9); ax.grid(True, which="both", alpha=.3)
    fig.tight_layout(); fig.savefig(f"{FIGDIR}/fig_error_regimes.png", dpi=110); plt.close(fig)
    print(f"\nWrote figures to {FIGDIR}/fig_*.png")


if __name__ == "__main__":
    main()
