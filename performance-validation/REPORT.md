# Does DTLMod model ADIOS2 performance accurately? — An empirical study

**Question.** DTLMod advertises *"realistic data transport layer modeling"* and *"high-fidelity
simulation of data transport protocols"* for ADIOS2-style in-situ workflows. This study tests that
claim by measuring real ADIOS2 performance and comparing it, configuration by configuration, against
the equivalent DTLMod simulation.

**Verdict (short version).** DTLMod is an accurate **bandwidth** model and a poor **latency** model.
Once its simulated platform is calibrated, DTLMod reproduces ADIOS2's per-step transfer time to within
**~5–15 %** for bulk transfers (≳1 MB/rank) — the regime that dominates HPC bulk I/O and in-situ data
movement. It does **not** reproduce small-message latency: it has no per-message software-overhead or
polling model, so it misses ADIOS2 **SST's ~44 ms small-message polling floor by four orders of
magnitude**, assumes a constant per-engine bandwidth (missing DataMan's size-dependent rate), and its
staging throughput at scale is only as good as the network topology the user supplies.

---

## 1. What was built

Everything was built and installed from source on a single 4-core node (Ubuntu 24.04, GCC 13.3):

| Component | Version / commit | Role |
|---|---|---|
| SimGrid | v4.1 (`535b880f`) | discrete-event simulation kernel |
| FSMod (file-system-module) | 0.4 (`917210b`) | simulated disks / file systems |
| DTLMod | 0.5 (`be35e5e` + fix) | the data-transport-layer simulator under test |
| ADIOS2 | 2.12.0 (MPI, BP5/SST/DataMan, ZeroMQ) | the real library = ground truth |

Two matched benchmark harnesses drive identical publish/subscribe scenarios and emit identical CSV:

* `adios/adios_bench.cpp` — real ADIOS2 (MPI), writer/reader roles, BP5 / SST / DataMan.
* `dtlmod/dtl_bench.cpp` — the DTLMod simulation, with the SimGrid platform's disk and link rates
  exposed on the command line so they can be **calibrated** to the machine ADIOS2 ran on.

Data model in both: a 1-D `double` array, each rank owning a contiguous block, so an ADIOS2 variable
maps 1:1 onto a DTLMod variable.

### Engine mapping

DTLMod exposes a **File** engine and a **Staging** engine (with `Mailbox` or `MQ` transport). Reading
the source pins down what each one models:

| ADIOS2 engine | DTLMod configuration | Cost model |
|---|---|---|
| **BP5** (files) | File engine + File transport | `FileTransport` → FSMod `write_async`/`read_async`, time = bytes / disk-rate |
| **SST** (network staging) | Staging + **Mailbox** | data sent as a SimGrid `Comm` of N bytes → time = latency + N/bandwidth |
| **DataMan** (ZeroMQ staging) | Staging + **Mailbox** | same affine network cost |
| *(none — idealized)* | Staging + **MQ** | SimGrid `MessageQueue` — **zero simulated time** (see §5) |

A key source-level finding: SimGrid's `MessImpl::start()` goes `READY → RUNNING → finish()` with no
resource action, so the **MQ transport moves data in zero simulated time**. It is a synchronization
primitive, not a transport with a cost.

---

## 2. Methodology

DTLMod is a simulator: it predicts time from a *described* platform. A fair test therefore (a)
measures ADIOS2, (b) calibrates the simulated platform's resource rates to that machine, and (c) checks
whether the calibrated simulation reproduces ADIOS2 **across configurations it was not calibrated on**.

* **Sweep.** message size 1 KB → 128 MB per rank; ranks 1/2/4 (BP5), 1/2 (SST), 1 (DataMan — see §7);
  ≥3 repetitions, medians reported. 285 ADIOS2 runs, 120 DTLMod runs.
* **Calibration model.** the standard affine (Hockney) transfer model `t(n) = α + n/β` fit to the
  1-rank data: `β` = effective bandwidth, `α` = fixed per-message overhead. These are exactly the two
  knobs SimGrid exposes (a link is `latency + size/bandwidth`; an FSMod disk is `size/bandwidth`).
* **Reference-point auto-calibration.** each engine's single resource knob is scaled so DTLMod matches
  ADIOS2 at **one** reference point (64 MB, 1 rank); everything else is a genuine extrapolation test.
  (DTLMod's transfer models are linear in the resource rate, verified, so one scaling pass is exact at
  the reference.) The SimGrid network model is set to the ideal fluid form so the link rate maps 1:1
  onto the calibrated bandwidth.

Measured effective rates on this node (the calibration):

| Engine | effective bandwidth β | small-message floor (1 KB) |
|---|---|---|
| BP5 write | 1.11 GB/s (page cache) | 0.020 ms |
| BP5 read | 6.04 GB/s (page cache) | 0.059 ms |
| SST | 0.94 GB/s | **44 ms (polling)** |
| DataMan | 0.62 GB/s asymptotic (≈1.4 GB/s at 256 KB–1 MB) | 0.009 ms |

---

## 3. Result A — Bandwidth regime: DTLMod is accurate

For bulk transfers (≥1 MB/rank, 1 rank), the calibrated simulation tracks ADIOS2 closely across two
orders of magnitude of message size — i.e. the calibration *extrapolates*:

| Engine | role | median \|error\| (≥1 MB, 1 rank) | max |
|---|---|---|---|
| BP5 | writer | **5.0 %** | 13 % |
| BP5 | reader | **11.5 %** | 33 % |
| SST | writer | **7.9 %** | 23 % |
| SST | reader | **4.8 %** | 21 % |
| DataMan | writer | 23.2 % | 112 % |
| DataMan | reader | 53.9 % | 207 % |

BP5 and SST are reproduced to single/low-double-digit percent. DataMan is worse **because its real
bandwidth is not constant** — it peaks near ~1.4 GB/s at 256 KB–1 MB and falls to ~0.6 GB/s for large
messages (ZeroMQ buffering). DTLMod's single calibrated rate cannot represent a size-dependent
bandwidth, so it over-predicts time at mid sizes and is exact only where it was calibrated (64 MB).

See `analysis/fig_size_sweep.png` (curves) and `analysis/fig_error_regimes.png` (error converging to 0
in the green band).

---

## 4. Result B — Latency regime: DTLMod's blind spot

Below ~256 KB/rank, fixed overheads dominate and the affine model with `α≈0` breaks down. Median
per-step error in the latency regime (<1 MB) is **~90 %** for every engine. The single most dramatic
gap is **SST**:

```
Latency floor at 1 KB (writer, 1 rank):
   BP5     : ADIOS2   0.020 ms   DTLMod 0.001 ms     (DTLMod misses ~20 us of BP buffering overhead)
   SST     : ADIOS2  43.999 ms   DTLMod 0.001 ms     (DTLMod misses SST's polling floor — 40000x off)
   DataMan : ADIOS2   0.009 ms   DTLMod 0.002 ms     (event-driven ZeroMQ; both small)
```

ADIOS2 **SST shows a persistent ~44 ms per-step time for messages ≤16 KB** (confirmed across
repetitions; 16 KB is bimodal — mostly 44 ms with the occasional 0.3 ms step), vanishing by 256 KB.
This is SST's reader-side polling/timeout behaviour, a real and important latency characteristic for
fine-grained coupling. DTLMod's transport has no notion of polling intervals or per-message software
overhead, so it predicts microseconds where reality is tens of milliseconds. **For latency-bound or
fine-grained streaming, DTLMod is not a faithful model.**

---

## 5. Result C — The MQ transport is a zero-cost idealization

The `MQ` staging transport predicts **exactly 0 µs** for data movement at every size:

```
   1 MB : MQ predicts 0.000 us   (real SST  1.0 ms)
  16 MB : MQ predicts 0.000 us   (real SST 14.4 ms)
  64 MB : MQ predicts 0.000 us   (real SST 70.8 ms)
```

This is correct given the SimGrid mechanics (§1) but means **MQ models no real engine** — it is an
infinitely-fast, zero-latency upper bound (useful for "what if the network were free" studies). Only
the **Mailbox** transport carries an actual, modellable transfer cost. Anyone benchmarking
"DataMan-like" or "SST-like" staging in DTLMod must use Mailbox, not MQ.

---

## 6. Result D — Rank scaling

**Writes / BP5 (`fig_bp_ranks.png`).** On one node ADIOS2's aggregate write bandwidth stays roughly
flat (~0.9–1.15 GB/s) from 1 → 4 ranks: the shared I/O path is the bottleneck. DTLMod's
single-shared-disk model predicts a constant 1.11 GB/s — **the right behaviour**, agreeing within
~15 %. DTLMod captures single-node write contention well.

**Staging / SST.** Here topology fidelity dominates. Real SST throughput **scales** with ranks
(×1.6–1.9 aggregate at 2 ranks — the two streams run on different cores), but the minimal
*single-shared-link* platform used here forces all transfers through one link, predicting **×1.0**
(no scaling) and a ~112 % per-step error at 2 ranks. This is a property of the supplied platform, not
a hard DTLMod limit: an independent-link-per-rank topology would predict ×2.0, and reality (×1.7) sits
between the two extremes. **DTLMod's staging accuracy at scale is bounded by how faithfully the
platform graph captures the node's real memory/interconnect parallelism** — a much harder modelling
task than the single shared disk.

---

## 7. Robustness findings (and a bug fixed)

* **DTLMod crash — fixed here.** The multiple-publisher *and* multiple-subscriber File configuration
  (untested upstream — only 1×N and N×1 have tests) reliably segfaulted with a null `engine_` in
  `Engine::add_subscriber`. Root cause: `Stream::open` created the engine under the DTL lock but
  registered the actor *outside* it, so a concurrent close could null `engine_` in the gap. **Fix:**
  make create+register a single critical section (`src/Stream.cpp`). This took 4×4 File from 100 %
  crashes to 0/15, with no unit-test regression (the 3 pre-existing `DTLReductionTest` failures are
  unrelated and fail on stock `main` too).
* **DTLMod deadlock — remaining.** N×N File with N≥3 and large data still deadlocks (the DTL
  connection-manager waits on message activities while a subscriber blocks on a barrier). Write rank
  scaling was therefore measured with the reliable N×1 decomposition (equivalent, since File
  subscribers sleep through the write phase).
* **DataMan is single-writer.** `DataManWriter` throws for `MpiSize > 1`, and DataMan dropped/aborted
  the largest (128 MB) transfers over loopback — so DataMan is inherently 1×1 here.

---

## 8. Conclusion

> **DTLMod models ADIOS2's *bandwidth* accurately, but not its *latency*.**

* **Supported:** for bulk, bandwidth-bound transfers (≳1 MB/rank) — the case in-situ workflows are
  built around — a calibrated DTLMod reproduces ADIOS2 BP5 and SST per-step time within ~5–15 % and
  extrapolates correctly across message size; single-node write contention is captured well. For its
  intended purpose (comparing data-transport configurations at scale), the accuracy claim holds.
* **Not supported / caveats:**
  1. No latency model — small-message overheads and especially **SST's ~44 ms polling floor** are
     missed by orders of magnitude.
  2. Per-engine bandwidth is assumed constant — **DataMan's** size-dependent rate is not captured.
  3. The **MQ** transport is a zero-cost idealization, not a real-engine model.
  4. Staging accuracy at scale is only as good as the user-supplied network topology.
  5. The multi-publisher/multi-subscriber File path was crash-prone (one crash fixed here; an N≥3
     deadlock remains).

DTLMod is a credible tool for *bandwidth/throughput* what-if studies of in-situ workflows once
calibrated to the target hardware. It should not be used to reason about *fine-grained latency* or to
predict absolute times for sub-megabyte messages without an added overhead/latency model.

---

## 9. Reproducing

```bash
# build (see scripts/ for exact flags): SimGrid 4.1 -> FSMod -> DTLMod -> ADIOS2
bash   scripts/sweep_adios.sh      # -> results/adios_raw.csv   (real ADIOS2)
python analysis/calibrate.py       # -> results/calibration.json (fit beta, alpha)
python scripts/sweep_dtl.py        # -> results/dtl_raw.csv      (calibrated DTLMod)
python analysis/compare.py         # -> results/comparison.csv, accuracy_summary.txt, analysis/fig_*.png
```

Raw data, the calibration, the joined comparison, and the three figures are committed alongside this
report.
