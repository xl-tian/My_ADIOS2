# Are ADIOS2 engine writes thread-safe?

**Claim under test:** *"ADIOS2 engine writes are not thread-safe."*

**Verdict: VERIFIED (TRUE).** Concurrent `Engine::Put()` calls on a single
engine are not thread-safe. This is confirmed three independent ways:

1. **The project documents it.** `docs/user_guide/source/advice/advice.rst`
   (item 10): *"Thread-safety: treat ADIOS 2 as NOT thread-safe. Either use a
   mutex or only handle I/O from a master thread."* The recently added
   thread-safe path is **read-only** (`GetContext`, BP5 reader); there is no
   counterpart on the write side.

2. **The source has no write-path locking.** `Engine::Put` →
   `BP5Writer::DoPutSync/PutCommon` → `BP5Serializer::Marshal`
   (`source/adios2/toolkit/format/bp5/BP5Serializer.cpp`) mutates a pile of
   shared, unsynchronized state on the engine's single serializer/buffer:
   the output buffer (`CurDataBuffer->AddToVec/Allocate`), the metadata buffer
   and field bitfield (`BP5BitfieldSet`), the writer-record hashtable
   (`LookupWriterRec`/`CreateWriterRec`), the `DefSpanMinMax` vector, and plain
   counters/accumulators (`m_BufferAppendCalls++`, `m_GetMinMaxSecs +=`). None
   of it is guarded by a mutex or made atomic. (The only mutexes in BP5 are in
   the *reader*/`Deserializer` and in the *async write thread* machinery —
   neither protects concurrent user `Put`s.)

3. **It fails empirically** — see the test below.

## The test

`concurrent_put_test.cpp` drives **one** BP5 writer engine from N threads that
all call `Engine::Put()` (Mode::Sync) at once. Each thread owns a disjoint set
of variables, so the correct result is unambiguous: every variable must read
back exactly the deterministic pattern its owner wrote. Variables are *defined*
single-threaded up front, so the only thing under concurrency is the engine
**write** path.

Three modes isolate "concurrency is the problem" from "the operations are
broken":

| mode           | what it does                              | expected            |
|----------------|-------------------------------------------|---------------------|
| `--serial`     | one thread does all Puts                  | PASS, TSan-clean    |
| `--mutex`      | N threads, every Put under one `std::mutex`| PASS, TSan-clean   |
| `--concurrent` | N threads, **no** synchronization          | FAIL / races / crash|

## Results obtained here

Built (library **and** test) with `-fsanitize=thread`, gcc 13.3:

* `--serial`  → **PASS**, no race reports.
* `--mutex`   → **PASS**, no race reports.  ⇒ the Puts are individually fine;
  serializing them fixes everything. The defect is purely the missing internal
  lock.
* `--concurrent` (8 threads) → **NOT safe**, shown three ways:
  * **58 ThreadSanitizer data-race reports** in a single run, every one rooted
    in the `Put` call stack
    (`Engine::Put → BP5Writer::DoPutSync → PutCommon → BP5Serializer::Marshal`),
    racing on the heap block allocated by `IO::MakeEngine<BP5Writer>` — i.e. the
    one shared engine. Distinct sites include `ChunkV::AddToVec` (buffer),
    `BP5Serializer::Marshal/AddSimpleField/CreateWriterRec/RecalcMarshalStorageSize`,
    and `BP5Base::BP5BitfieldSet/Test`. Full list in `evidence_tsan_summary.txt`.
  * **A hard crash:** glibc fortify aborts the process with
    `*** buffer overflow detected ***` — a corrupted shared offset caused a real
    out-of-bounds write (memory unsafety, not just a benign race).
  * **Silent data corruption:** a completed run reported
    `read-back: 7 missing variables (of 8)` — concurrent metadata/bitfield/
    writer-record updates clobbered each other, so 7 of 8 variables never made
    it into the file. Wrong output, no error raised.

## Reproduce

Build a serial BP5 ADIOS2 with ThreadSanitizer (see header of
`build_and_run.sh` for the exact `cmake` flags), then:

```bash
export ADIOS2_BUILD=/path/to/your/build-tsan   # defaults to ../build-tsan
./build_and_run.sh
```

Or compile by hand against a build tree:

```bash
g++ -std=c++17 -g -O1 -fsanitize=thread \
    -I<src>/bindings/CXX -I<src>/source -I<build>/source \
    concurrent_put_test.cpp \
    -L<build>/lib -ladios2_cxx -ladios2_core -Wl,-rpath,<build>/lib -pthread \
    -o concurrent_put_test

LD_LIBRARY_PATH=<build>/lib TSAN_OPTIONS="halt_on_error=0" \
    ./concurrent_put_test --concurrent --threads 8 --iters 20
```

Flags: `--serial | --mutex | --concurrent`, `--threads N`, `--vars N`,
`--elems N`, `--iters N`.

**Note on a clean PASS in `--concurrent`:** data races are nondeterministic, so
an occasional clean functional pass does *not* prove safety — that is exactly
why the ThreadSanitizer build (which reports the race regardless of whether the
corruption happens to surface) is the authoritative check. The controls
(`--serial`, `--mutex`) staying TSan-clean while `--concurrent` reports races is
the definitive signal.

## Takeaway

The claim holds. If you write from multiple threads, give each thread its own
`IO`+`Engine` (separate output), or serialize all `Put`/`BeginStep`/`EndStep`/
`Close` calls on a shared engine behind your own lock — exactly as the ADIOS2
docs advise.
