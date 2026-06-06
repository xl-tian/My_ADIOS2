/*
 * concurrent_put_test.cpp
 *
 * Empirical test of the claim:
 *   "ADIOS2 engine writes are not thread-safe."
 *
 * The test drives a SINGLE ADIOS2 writer engine from multiple threads that all
 * call Engine::Put() concurrently. Each thread owns a disjoint set of variables
 * (so the *logically correct* result is unambiguous: every variable must read
 * back exactly the deterministic pattern its owner wrote). The variables share
 * the engine's single internal serializer/buffer (BP5Serializer: CurDataBuffer,
 * the metadata buffer, append counters, the DefSpanMinMax vector, field
 * bitfields, ...), none of which is protected by a lock on the write path.
 *
 * Three modes let us separate "the operations are wrong" from "concurrency is
 * wrong":
 *
 *   --concurrent : N threads call Put() on the shared engine with NO
 *                  synchronization. This is the unsafe case under test.
 *   --mutex      : same N threads, but every Put() is guarded by one shared
 *                  std::mutex. Control: if this passes while --concurrent fails,
 *                  the Put operations themselves are fine and it is precisely
 *                  the lack of internal locking that is the problem.
 *   --serial     : one thread does all Puts. Baseline of correctness.
 *
 * How failure shows up:
 *   * Built normally: with enough threads/iterations the unsynchronized run
 *     produces wrong values on read-back, a reader-side exception (corrupt
 *     metadata), or a crash (heap corruption / segfault).
 *   * Built with -fsanitize=thread: even a 2-thread run prints data-race
 *     reports rooted in BP5Serializer::Marshal / BufferV append, deterministic
 *     proof independent of whether the corruption happens to be observable.
 *
 * Exit code: 0 = all read-back verification passed; non-zero = a mismatch,
 * exception, or detected corruption (i.e. evidence the writes are not safe).
 */

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <adios2.h>

namespace
{

enum class Mode
{
    Concurrent,
    Mutex,
    Serial
};

struct Config
{
    Mode mode = Mode::Concurrent;
    int threads = 8;          // number of writer threads
    int varsPerThread = 4;    // distinct variables each thread owns
    std::size_t elems = 4096; // elements per variable (int32)
    int iterations = 20;      // repeat the whole open/write/close/verify cycle
    std::string fileBase = "thread_safety_test_out";
};

// Deterministic, collision-free pattern so any deviation on read-back is a bug.
inline int32_t patternValue(int threadId, int varIdx, std::size_t i)
{
    // Mix the three coordinates; keep it cheap but distinct across the space.
    const uint64_t k = (static_cast<uint64_t>(threadId) * 92821u + varIdx) * 2654435761u +
                       static_cast<uint64_t>(i);
    return static_cast<int32_t>(k & 0x7fffffff);
}

inline std::string varName(int threadId, int varIdx)
{
    return "t" + std::to_string(threadId) + "_v" + std::to_string(varIdx);
}

// Spin-barrier so every thread reaches its Put loop at (almost) the same instant,
// maximizing contention on the shared engine internals.
class SpinBarrier
{
public:
    explicit SpinBarrier(int n) : m_target(n) {}
    void arrive_and_wait()
    {
        m_count.fetch_add(1, std::memory_order_acq_rel);
        while (m_count.load(std::memory_order_acquire) < m_target)
        {
            std::this_thread::yield();
        }
    }

private:
    const int m_target;
    std::atomic<int> m_count{0};
};

// Returns true if read-back matched the written pattern for every variable.
bool runOnce(adios2::ADIOS &adios, const Config &cfg, int iter, std::string &failReason)
{
    const std::string fileName = cfg.fileBase + "_" + std::to_string(iter) + ".bp";

    // ---- WRITE ----------------------------------------------------------
    {
        adios2::IO io = adios.DeclareIO("Writer_" + std::to_string(iter));
        io.SetEngine("BP5");

        // Define ALL variables up front, single-threaded. The race under test is
        // purely in the engine write path (Put), not in IO::DefineVariable.
        std::vector<std::vector<adios2::Variable<int32_t>>> vars(cfg.threads);
        for (int t = 0; t < cfg.threads; ++t)
        {
            for (int v = 0; v < cfg.varsPerThread; ++v)
            {
                vars[t].push_back(io.DefineVariable<int32_t>(
                    varName(t, v), {cfg.elems}, {0}, {cfg.elems}, adios2::ConstantDims));
            }
        }

        // Pre-fill each thread's source buffers with its pattern.
        std::vector<std::vector<std::vector<int32_t>>> data(cfg.threads);
        for (int t = 0; t < cfg.threads; ++t)
        {
            data[t].resize(cfg.varsPerThread);
            for (int v = 0; v < cfg.varsPerThread; ++v)
            {
                data[t][v].resize(cfg.elems);
                for (std::size_t i = 0; i < cfg.elems; ++i)
                {
                    data[t][v][i] = patternValue(t, v, i);
                }
            }
        }

        adios2::Engine engine = io.Open(fileName, adios2::Mode::Write);
        engine.BeginStep();

        std::mutex putMutex;
        std::atomic<bool> threw{false};
        std::string threadError;
        std::mutex errMutex;

        auto worker = [&](int t, SpinBarrier *barrier) {
            try
            {
                if (barrier)
                {
                    barrier->arrive_and_wait();
                }
                for (int v = 0; v < cfg.varsPerThread; ++v)
                {
                    if (cfg.mode == Mode::Mutex)
                    {
                        std::lock_guard<std::mutex> lk(putMutex);
                        // Sync: do the full marshal/append inside this Put call.
                        engine.Put(vars[t][v], data[t][v].data(), adios2::Mode::Sync);
                    }
                    else
                    {
                        engine.Put(vars[t][v], data[t][v].data(), adios2::Mode::Sync);
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::lock_guard<std::mutex> lk(errMutex);
                if (!threw.exchange(true))
                {
                    threadError = e.what();
                }
            }
        };

        if (cfg.mode == Mode::Serial)
        {
            for (int t = 0; t < cfg.threads; ++t)
            {
                worker(t, nullptr);
            }
        }
        else
        {
            SpinBarrier barrier(cfg.threads);
            std::vector<std::thread> pool;
            pool.reserve(cfg.threads);
            for (int t = 0; t < cfg.threads; ++t)
            {
                pool.emplace_back(worker, t, &barrier);
            }
            for (auto &th : pool)
            {
                th.join();
            }
        }

        engine.EndStep();
        engine.Close();

        if (threw.load())
        {
            failReason = "exception during concurrent Put: " + threadError;
            return false;
        }
    }

    // ---- READ BACK & VERIFY --------------------------------------------
    try
    {
        adios2::IO io = adios.DeclareIO("Reader_" + std::to_string(iter));
        io.SetEngine("BP5");
        adios2::Engine engine = io.Open(fileName, adios2::Mode::ReadRandomAccess);

        std::size_t mismatches = 0;
        std::size_t missing = 0;
        for (int t = 0; t < cfg.threads; ++t)
        {
            for (int v = 0; v < cfg.varsPerThread; ++v)
            {
                auto var = io.InquireVariable<int32_t>(varName(t, v));
                if (!var)
                {
                    ++missing;
                    continue;
                }
                std::vector<int32_t> got;
                engine.Get(var, got, adios2::Mode::Sync);
                if (got.size() != cfg.elems)
                {
                    ++mismatches;
                    continue;
                }
                for (std::size_t i = 0; i < cfg.elems; ++i)
                {
                    if (got[i] != patternValue(t, v, i))
                    {
                        ++mismatches;
                        break;
                    }
                }
            }
        }
        engine.Close();

        if (missing || mismatches)
        {
            failReason = "read-back: " + std::to_string(missing) + " missing variables, " +
                         std::to_string(mismatches) + " corrupted variables (of " +
                         std::to_string(cfg.threads * cfg.varsPerThread) + ")";
            return false;
        }
    }
    catch (const std::exception &e)
    {
        failReason = std::string("exception during read-back (corrupt output): ") + e.what();
        return false;
    }

    return true;
}

Config parseArgs(int argc, char **argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--concurrent")
            cfg.mode = Mode::Concurrent;
        else if (a == "--mutex")
            cfg.mode = Mode::Mutex;
        else if (a == "--serial")
            cfg.mode = Mode::Serial;
        else if (a == "--threads" && i + 1 < argc)
            cfg.threads = std::atoi(argv[++i]);
        else if (a == "--vars" && i + 1 < argc)
            cfg.varsPerThread = std::atoi(argv[++i]);
        else if (a == "--elems" && i + 1 < argc)
            cfg.elems = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--iters" && i + 1 < argc)
            cfg.iterations = std::atoi(argv[++i]);
        else
        {
            std::cerr << "Unknown/!incomplete arg: " << a << "\n";
        }
    }
    return cfg;
}

const char *modeName(Mode m)
{
    switch (m)
    {
    case Mode::Concurrent:
        return "concurrent (NO lock)";
    case Mode::Mutex:
        return "mutex-guarded";
    case Mode::Serial:
        return "serial";
    }
    return "?";
}

} // namespace

int main(int argc, char **argv)
{
    Config cfg = parseArgs(argc, argv);

    std::cout << "=== ADIOS2 engine write thread-safety test ===\n"
              << "mode        : " << modeName(cfg.mode) << "\n"
              << "threads     : " << cfg.threads << "\n"
              << "vars/thread : " << cfg.varsPerThread << "\n"
              << "elems/var   : " << cfg.elems << "\n"
              << "iterations  : " << cfg.iterations << "\n"
              << "engine      : BP5, Put mode = Sync\n"
              << "----------------------------------------------\n";

    adios2::ADIOS adios;

    int failures = 0;
    std::string firstFail;
    for (int iter = 0; iter < cfg.iterations; ++iter)
    {
        std::string reason;
        bool ok = false;
        try
        {
            ok = runOnce(adios, cfg, iter, reason);
        }
        catch (const std::exception &e)
        {
            reason = std::string("top-level exception: ") + e.what();
            ok = false;
        }
        if (!ok)
        {
            if (failures == 0)
                firstFail = reason;
            ++failures;
            std::cout << "iter " << iter << ": FAIL -- " << reason << "\n";
        }
    }

    std::cout << "----------------------------------------------\n";
    if (failures == 0)
    {
        std::cout << "RESULT: PASS (" << cfg.iterations << "/" << cfg.iterations
                  << " iterations verified)\n";
        if (cfg.mode == Mode::Concurrent)
        {
            std::cout << "Note: a clean PASS here does NOT prove safety -- data races\n"
                         "are nondeterministic. Re-run, raise --threads/--iters, and/or\n"
                         "build with -fsanitize=thread for a deterministic verdict.\n";
        }
        return 0;
    }

    std::cout << "RESULT: FAIL -- " << failures << "/" << cfg.iterations
              << " iterations corrupted.\n"
              << "First failure: " << firstFail << "\n"
              << "=> Evidence that ADIOS2 engine writes are NOT thread-safe.\n";
    return 1;
}
