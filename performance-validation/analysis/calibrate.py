#!/usr/bin/env python3
"""
Calibrate SimGrid platform parameters from the 1-rank ADIOS2 measurements.

For every (engine, role) we fit the classic affine transfer model to the median
per-step time vs message size:

        t(n) = alpha + n / beta          [Hockney model]

  beta  = effective asymptotic bandwidth (bytes/s)
  alpha = fixed per-step latency / software overhead (s)

These are exactly the two knobs SimGrid exposes:
  * a network link has   t = latency + size/bandwidth   -> (alpha, beta)
  * an FSMod disk has     t = size/bandwidth             -> (beta only)

Outputs:
  results/calibration.json   machine-readable parameters
  results/calibration.txt    human-readable summary
"""
import csv, json, statistics, sys
from collections import defaultdict

RAW = "/home/user/dtlmod-verification/results/adios_raw.csv"
OUTJSON = "/home/user/dtlmod-verification/results/calibration.json"
OUTTXT = "/home/user/dtlmod-verification/results/calibration.txt"


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            if r.get("marker") != "RESULT":
                continue
            for k in ("nranks", "bytes_per_rank", "steps"):
                r[k] = int(r[k])
            for k in ("open_s", "step_s", "close_s", "bw_GBps", "min_step_s", "med_step_s"):
                r[k] = float(r[k])
            rows.append(r)
    return rows


def median_per_step(rows, engine, role, nranks):
    """Return {bytes: median_per_step_time} over repetitions."""
    by_size = defaultdict(list)
    for r in rows:
        if r["engine"] == engine and r["role"] == role and r["nranks"] == nranks:
            by_size[r["bytes_per_rank"]].append(r["med_step_s"])
    return {n: statistics.median(v) for n, v in by_size.items()}


def linfit(xs, ys):
    """Least squares y = a + b x. Returns (a, b)."""
    n = len(xs)
    sx = sum(xs); sy = sum(ys)
    sxx = sum(x * x for x in xs); sxy = sum(x * y for x, y in zip(xs, ys))
    denom = n * sxx - sx * sx
    if denom == 0:
        return ys[0], 0.0
    b = (n * sxy - sx * sy) / denom
    a = (sy - b * sx) / n
    return a, b


# Fit bandwidth/latency only over the bandwidth regime. Sub-256KB messages can
# be dominated by engine-specific artifacts (e.g. SST's ~44 ms polling floor)
# that an affine model cannot represent; including them corrupts the fit.
FIT_MIN = 262144


def calibrate_engine(rows, engine, role):
    d = median_per_step(rows, engine, role, 1)
    if not d:
        return None
    all_sizes = sorted(d)
    sizes = [n for n in all_sizes if n >= FIT_MIN] or all_sizes
    ts = [d[n] for n in sizes]
    # Affine fit over the bandwidth regime -> alpha (intercept), 1/beta (slope)
    alpha, inv_beta = linfit([float(n) for n in sizes], ts)
    alpha = max(alpha, 0.0)
    beta = (1.0 / inv_beta) if inv_beta > 0 else float("inf")
    # Empirical small-message latency (the real per-step floor at the smallest
    # size), kept separate from the affine alpha for the latency discussion.
    lat_small = d[all_sizes[0]]
    # Asymptotic bandwidth from the largest message (more robust for the disk model
    # where there is no latency term to subtract): use the two largest sizes to
    # remove the fixed overhead.
    if len(sizes) >= 2:
        n1, n2 = sizes[-2], sizes[-1]
        dt = d[n2] - d[n1]
        beta_asym = (n2 - n1) / dt if dt > 0 else beta
    else:
        beta_asym = beta
    return {
        "engine": engine, "role": role,
        "alpha_s": alpha, "beta_Bps": beta, "beta_asym_Bps": beta_asym,
        "lat_small_s": lat_small, "lat_small_bytes": all_sizes[0],
        "samples": {str(n): d[n] for n in all_sizes},
    }


def fmt_bw(bps):
    return f"{bps/1e9:.3f} GB/s ({bps/1e6:.1f} MB/s)"


def main():
    rows = load(RAW)
    if not rows:
        print("No data in", RAW, file=sys.stderr); sys.exit(1)

    cal = {}
    lines = []
    lines.append("ADIOS2 -> SimGrid calibration (1-rank affine fit  t = alpha + n/beta)\n")
    for engine in ("BP5", "SST", "DataMan"):
        for role in ("writer", "reader"):
            c = calibrate_engine(rows, engine, role)
            if not c:
                continue
            cal[f"{engine}_{role}"] = c
            lines.append(f"{engine:8s} {role:6s}: alpha={c['alpha_s']*1e6:8.1f}us  "
                         f"beta_asym={fmt_bw(c['beta_asym_Bps']):26s}  "
                         f"lat@{c['lat_small_bytes']//1024}KB={c['lat_small_s']*1e3:8.3f}ms")
    # Derive the three platform configs DTLMod will use.
    def g(k, f, d=0.0):
        return cal[k][f] if k in cal else d
    platform = {
        # File engine (BP): FSMod disk has no latency term, use asymptotic BW.
        "disk_write_Bps": g("BP5_writer", "beta_asym_Bps"),
        "disk_read_Bps":  g("BP5_reader", "beta_asym_Bps"),
        # Mailbox staging calibrated to SST (network cost = latency + n/bw).
        "sst_link_Bps":   g("SST_writer", "beta_Bps"),
        "sst_link_lat_s": g("SST_writer", "alpha_s"),
        # Mailbox staging calibrated to DataMan.
        "dm_link_Bps":    g("DataMan_writer", "beta_Bps"),
        "dm_link_lat_s":  g("DataMan_writer", "alpha_s"),
    }
    cal["platform"] = platform
    lines.append("\nDerived DTLMod platform parameters:")
    lines.append(f"  File   disk write : {fmt_bw(platform['disk_write_Bps'])}")
    lines.append(f"  File   disk read  : {fmt_bw(platform['disk_read_Bps'])}")
    lines.append(f"  SST    link bw    : {fmt_bw(platform['sst_link_Bps'])}  lat={platform['sst_link_lat_s']*1e6:.2f} us")
    lines.append(f"  DataMan link bw   : {fmt_bw(platform['dm_link_Bps'])}  lat={platform['dm_link_lat_s']*1e6:.2f} us")

    with open(OUTJSON, "w") as f:
        json.dump(cal, f, indent=2)
    with open(OUTTXT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
