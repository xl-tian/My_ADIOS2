#!/usr/bin/env python3
"""
Run the DTLMod simulation sweep -> results/dtl_raw.csv (same schema as ADIOS2).

Calibration strategy: REFERENCE-POINT auto-calibration.
  Each engine has ONE free resource knob (disk bandwidth, or link bandwidth).
  We pick it so DTLMod reproduces the ADIOS2 bandwidth at a single reference
  configuration (64 MB, 1 rank); then we run the whole size x rank matrix and
  test how well that single calibration EXTRAPOLATES. DTLMod's transfer models
  are linear in the resource rate, so one scaling pass is exact at the reference.
  This absorbs any constant model factors (FSMod / SimGrid network) and isolates
  the real question: does DTLMod capture the SCALING with size and rank count?

  Link latency is set to the measured affine intercept (bandwidth-regime alpha),
  which is ~0; DTLMod therefore does NOT attempt to reproduce SST's small-message
  polling floor -- that divergence is itself a reported result.

Tags map each DTLMod run to the ADIOS2 engine it models: BP5, SST, DataMan, MQ.
"""
import json, subprocess, os, sys

ROOT = "/home/user/dtlmod-verification"
BIN = f"{ROOT}/dtlmod/dtl_bench"
CAL = f"{ROOT}/results/calibration.json"
OUT = f"{ROOT}/results/dtl_raw.csv"

SIZES = [1024, 16384, 262144, 1048576, 4194304, 16777216, 67108864, 134217728]
REF = 67108864            # reference message size for calibration (64 MB)
STEPS, WARM = 12, 2
ENV = dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib:" + os.environ.get("LD_LIBRARY_PATH", ""))
HEADER = ("marker,tag,engine,role,nranks,bytes_per_rank,steps,open_s,step_s,"
          "close_s,total_bytes,bw_GBps,min_step_s,med_step_s")


def bps(x):
    return f"{int(max(1.0, x))}Bps"


def secs(x):
    return f"{max(0.0, x):.9f}s"


def invoke(args, retries=8):
    # DTLMod has an intermittent race with many concurrent actors (segfaults a
    # fraction of multi-rank runs). The simulation is deterministic in its timing,
    # so a successful run gives valid numbers; just retry until one succeeds.
    last = ""
    for _ in range(retries):
        p = subprocess.run([BIN] + args, capture_output=True, text=True, env=ENV, timeout=180)
        rows = [ln for ln in p.stdout.splitlines() if ln.startswith("RESULT,")]
        if rows:
            return rows
        last = p.stderr[-200:]
    print(f"  WARN no output after {retries} tries: {' '.join(args[-8:])}\n{last}", file=sys.stderr)
    return []


def field(row, idx):
    return float(row.split(",")[idx])


BW = 11  # index of bw_GBps in a RESULT line


def file_args(size, ranks, dwrite, dread, nsub=None):
    if nsub is None:
        nsub = ranks
    return ["--engine", "File", "--transport", "File", "--bytes", str(size),
            "--steps", str(STEPS), "--warmup", str(WARM), "--npub", str(ranks),
            "--nsub", str(nsub), "--read_pattern", "own",
            "--disk_write_bw", bps(dwrite), "--disk_read_bw", bps(dread),
            "--link_bw", "1000GBps", "--link_lat", "0us"]


def staging_args(size, ranks, transport, lbw, llat):
    return ["--engine", "Staging", "--transport", transport, "--bytes", str(size),
            "--steps", str(STEPS), "--warmup", str(WARM), "--npub", str(ranks),
            "--nsub", str(ranks), "--read_pattern", "own",
            "--link_bw", bps(lbw), "--link_lat", secs(llat)]


def calibrate_scalar(probe_args_fn, target_bw_GBps, role_idx, init):
    """Return resource rate (B/s) s.t. DTLMod bandwidth == target at the reference.
    role_idx selects writer(0)/reader(1) RESULT row from the probe output."""
    rows = invoke(probe_args_fn(init))
    if len(rows) <= role_idx:
        return init
    bw_dtl = field(rows[role_idx], BW)             # GB/s produced with `init`
    if bw_dtl <= 0:
        return init
    return init * (target_bw_GBps / bw_dtl)        # linear model -> exact scaling


def main():
    cal = json.load(open(CAL))
    out = [HEADER]

    def g(key, fld):
        return cal[key][fld]

    # ---------- BP5 / File: calibrate disk write & read separately ----------
    tgt_w = g("BP5_writer", "beta_asym_Bps") / 1e9
    tgt_r = g("BP5_reader", "beta_asym_Bps") / 1e9
    dwrite = calibrate_scalar(lambda v: file_args(REF, 1, v, g("BP5_reader", "beta_asym_Bps")),
                              tgt_w, 0, g("BP5_writer", "beta_asym_Bps"))
    dread = calibrate_scalar(lambda v: file_args(REF, 1, dwrite, v),
                             tgt_r, 1, g("BP5_reader", "beta_asym_Bps"))
    print(f"BP5 calib: disk_write={dwrite/1e9:.3f} GB/s  disk_read={dread/1e9:.3f} GB/s")
    # DTLMod's File engine deadlocks for N x N with N>=3 (connection-manager /
    # barrier bug, documented). Subscribers sleep through the File write phase, so
    # an N-publisher x 1-subscriber run yields the same WRITE timing as N x N -- we
    # use it to get write-scaling at ranks {1,2,4}. Reads need matching N x N data
    # layout, so reader scaling is limited to the reliable ranks {1,2}.
    for size in SIZES:
        for ranks in (1, 2, 4):                    # write scaling via N x 1
            for r in invoke(file_args(size, ranks, dwrite, dread, nsub=1)):
                f = r.split(",")
                if f[3] == "writer":
                    f[1] = "BP5"; out.append(",".join(f))
        for ranks in (1, 2):                        # read scaling via N x N
            for r in invoke(file_args(size, ranks, dwrite, dread)):
                f = r.split(",")
                if f[3] == "reader":
                    f[1] = "BP5"; out.append(",".join(f))

    # ---------- SST / Mailbox ----------
    tgt = g("SST_writer", "beta_asym_Bps") / 1e9
    lbw = calibrate_scalar(lambda v: staging_args(REF, 1, "Mailbox", v, g("SST_writer", "alpha_s")),
                           tgt, 0, g("SST_writer", "beta_asym_Bps"))
    llat = g("SST_writer", "alpha_s")
    print(f"SST calib: link_bw={lbw/1e9:.3f} GB/s  link_lat={llat*1e6:.2f} us")
    for size in SIZES:
        for ranks in (1, 2):
            for r in invoke(staging_args(size, ranks, "Mailbox", lbw, llat)):
                f = r.split(","); f[1] = "SST"; out.append(",".join(f))

    # ---------- DataMan / Mailbox (rank 1) ----------
    tgt = g("DataMan_writer", "beta_asym_Bps") / 1e9
    lbw = calibrate_scalar(lambda v: staging_args(REF, 1, "Mailbox", v, 0.0),
                           tgt, 0, g("DataMan_writer", "beta_asym_Bps"))
    print(f"DataMan calib: link_bw={lbw/1e9:.3f} GB/s")
    for size in SIZES:
        for r in invoke(staging_args(size, 1, "Mailbox", lbw, 0.0)):
            f = r.split(","); f[1] = "DataMan"; out.append(",".join(f))

    # ---------- MQ idealized zero-cost staging (link params irrelevant) ----------
    for size in SIZES:
        for ranks in (1, 2):
            for r in invoke(staging_args(size, ranks, "MQ", lbw, 0.0)):
                f = r.split(","); f[1] = "MQ"; out.append(",".join(f))

    open(OUT, "w").write("\n".join(out) + "\n")
    print(f"Wrote {len(out)-1} rows to {OUT}")


if __name__ == "__main__":
    main()
