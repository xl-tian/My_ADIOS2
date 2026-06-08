/*
 * adios_bench.cpp — Empirical performance benchmark for ADIOS2 engines.
 *
 * A single configurable binary that plays the "writer" or "reader" role and
 * exercises one of the BP5 / SST / DataMan engines. It measures the wall-clock
 * cost of the Open / Put / EndStep / Get / Close phases and reports a single
 * CSV summary line (aggregated across MPI ranks with MAX) so the results can be
 * compared directly against the equivalent DTLMod simulation.
 *
 * Data model (kept deliberately simple so it maps 1:1 onto a DTLMod Variable):
 *   - A 1-D global array of `double`.
 *   - Each of the `nranks` ranks owns a contiguous block of `nelem` doubles.
 *   - global shape = {nranks * nelem}, this rank's selection = {rank*nelem, nelem}.
 *
 * Usage:
 *   adios_bench --engine BP5 --role writer --bytes 16777216 --steps 11 \
 *               --name /path/to/out.bp --warmup 1 [--port 12306] [--tag t]
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <adios2.h>
#include <mpi.h>

namespace {

struct Config {
    std::string engine = "BP5";
    std::string role   = "writer"; // writer | reader
    std::string name   = "bench";
    size_t bytes       = 1 << 20;  // bytes per rank
    int steps          = 11;
    int warmup         = 1;
    std::string ip     = "127.0.0.1";
    std::string port   = "12306";
    std::string tag    = "";       // free-form label echoed into the CSV
    int verbose        = 0;
};

Config parse(int argc, char **argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--engine") c.engine = next();
        else if (a == "--role") c.role = next();
        else if (a == "--name") c.name = next();
        else if (a == "--bytes") c.bytes = std::stoull(next());
        else if (a == "--steps") c.steps = std::stoi(next());
        else if (a == "--warmup") c.warmup = std::stoi(next());
        else if (a == "--ip") c.ip = next();
        else if (a == "--port") c.port = next();
        else if (a == "--tag") c.tag = next();
        else if (a == "--verbose") c.verbose = std::stoi(next());
        else { std::cerr << "Unknown arg: " << a << "\n"; }
    }
    return c;
}

double now() { return MPI_Wtime(); }

// Configure engine-specific parameters that matter for a fair, low-overhead
// streaming/file benchmark.
void configure_io(adios2::IO &io, const Config &c) {
    io.SetEngine(c.engine);
    if (c.engine == "DataMan") {
        io.SetParameters({{"IPAddress", c.ip},
                          {"Port", c.port},
                          {"Transport", "tcp"},
                          {"Verbose", "0"}});
    } else if (c.engine == "SST") {
        // RendezvousReaderCount=1: writer Open blocks until the single reader
        // connects (the natural in-situ coupling). FFS marshaling, blocking
        // queue with depth 1 so the reader paces the writer.
        io.SetParameters({{"RendezvousReaderCount", "1"},
                          {"QueueLimit", "1"},
                          {"QueueFullPolicy", "Block"},
                          {"DataTransport", "WAN"}});
    }
    // BP5: defaults (per-rank subfiles, EndStep flush). Nothing to set.
}

} // namespace

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, nranks = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    Config c = parse(argc, argv);

    const size_t nelem = std::max<size_t>(1, c.bytes / sizeof(double));
    const size_t local_bytes = nelem * sizeof(double);
    const size_t gN = static_cast<size_t>(nranks) * nelem;
    const size_t start = static_cast<size_t>(rank) * nelem;

    std::vector<double> data(nelem);
    for (size_t i = 0; i < nelem; ++i) data[i] = static_cast<double>(start + i);

    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::IO io = adios.DeclareIO("BenchIO");
    configure_io(io, c);

    // Per-phase accumulators (this rank).
    double t_open = 0, t_step = 0, t_close = 0;
    std::vector<double> per_step;
    per_step.reserve(c.steps);

    const bool writing = (c.role == "writer");
    const adios2::Mode mode = writing ? adios2::Mode::Write : adios2::Mode::Read;

    double t0 = now();
    adios2::Engine engine = io.Open(c.name, mode, MPI_COMM_WORLD);
    t_open = now() - t0;

    adios2::Variable<double> var;
    if (writing) {
        var = io.DefineVariable<double>("data", {gN}, {start}, {nelem});
    }

    int produced_or_consumed_steps = 0;

    for (int s = 0; s < c.steps; ++s) {
        double ts = now();
        if (writing) {
            engine.BeginStep();
            engine.Put(var, data.data(), adios2::Mode::Sync);
            engine.EndStep();
        } else {
            adios2::StepStatus st = engine.BeginStep();
            if (st != adios2::StepStatus::OK) break;
            var = io.InquireVariable<double>("data");
            if (var) {
                var.SetSelection({{start}, {nelem}});
                engine.Get(var, data.data(), adios2::Mode::Sync);
            }
            engine.EndStep();
        }
        double dt = now() - ts;
        if (s >= c.warmup) { t_step += dt; per_step.push_back(dt); }
        ++produced_or_consumed_steps;
    }

    double tc = now();
    engine.Close();
    t_close = now() - tc;

    // Aggregate across ranks: the slowest rank dictates the wall time.
    int counted = std::max(0, produced_or_consumed_steps - c.warmup);
    double agg_open, agg_step, agg_close;
    MPI_Reduce(&t_open,  &agg_open,  1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&t_step,  &agg_step,  1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&t_close, &agg_close, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    // Min/median per-step latency across this rank's timed steps (rank 0 only is
    // fine for reporting; latency regime uses 1 rank anyway).
    double min_step = 0, med_step = 0;
    if (!per_step.empty()) {
        std::vector<double> v = per_step;
        std::sort(v.begin(), v.end());
        min_step = v.front();
        med_step = v[v.size() / 2];
    }

    if (rank == 0) {
        const double total_bytes =
            static_cast<double>(local_bytes) * nranks * counted;
        const double bw_gbps =
            (agg_step > 0) ? total_bytes / agg_step / 1e9 : 0.0;
        // CSV: tag,engine,role,nranks,bytes_per_rank,steps_counted,
        //      open_s,step_s,close_s,total_bytes,bw_GBps,min_step_s,med_step_s
        std::cout << "RESULT," << c.tag << "," << c.engine << "," << c.role
                  << "," << nranks << "," << local_bytes << "," << counted
                  << "," << agg_open << "," << agg_step << "," << agg_close
                  << "," << static_cast<uint64_t>(total_bytes) << "," << bw_gbps
                  << "," << min_step << "," << med_step << "\n";
    }

    MPI_Finalize();
    return 0;
}
