/*
 * separate_engine_test.cpp
 *
 * Follow-up question: "Does it work if each thread uses its OWN IO + Engine and
 * writes to its OWN separate stream/file?"
 *
 * Each thread owns a distinct IO, a distinct BP5 engine, and a distinct output
 * file -- so there is no shared serializer/buffer between threads (that was the
 * problem demonstrated by concurrent_put_test.cpp). The remaining question is
 * whether the *surrounding* machinery is safe. Two sub-modes:
 *
 *   --create-in-thread : each thread calls adios.DeclareIO(), io.Open(),
 *                        the write loop, and engine.Close() itself, all
 *                        concurrently, sharing ONE adios2::ADIOS factory.
 *                        Tests the create/teardown path through shared ADIOS.
 *
 *   --create-upfront   : the main thread declares every IO, defines variables,
 *                        and opens every engine (serially). Threads then ONLY
 *                        run BeginStep/Put/EndStep on their own engine
 *                        concurrently; Close is done serially afterward.
 *                        Tests just the steady-state per-engine writing.
 *
 * Build/run exactly like concurrent_put_test (same TSan library). Watch both
 * the verification result AND whether ThreadSanitizer prints any races.
 */

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <adios2.h>

namespace
{

int g_threads = 8;
int g_varsPerThread = 4;
std::size_t g_elems = 4096;
bool g_createInThread = true; // default to the harder case

int32_t patternValue(int threadId, int varIdx, std::size_t i)
{
    const uint64_t k = (static_cast<uint64_t>(threadId) * 92821u + varIdx) * 2654435761u +
                       static_cast<uint64_t>(i);
    return static_cast<int32_t>(k & 0x7fffffff);
}
std::string varName(int v) { return "v" + std::to_string(v); }
std::string fileName(int t) { return "separate_stream_t" + std::to_string(t) + ".bp"; }

class SpinBarrier
{
public:
    explicit SpinBarrier(int n) : m_target(n) {}
    void arrive_and_wait()
    {
        m_count.fetch_add(1, std::memory_order_acq_rel);
        while (m_count.load(std::memory_order_acquire) < m_target)
            std::this_thread::yield();
    }

private:
    const int m_target;
    std::atomic<int> m_count{0};
};

// Build the per-thread source data once.
std::vector<std::vector<int32_t>> makeData(int t)
{
    std::vector<std::vector<int32_t>> d(g_varsPerThread);
    for (int v = 0; v < g_varsPerThread; ++v)
    {
        d[v].resize(g_elems);
        for (std::size_t i = 0; i < g_elems; ++i)
            d[v][i] = patternValue(t, v, i);
    }
    return d;
}

bool verify()
{
    adios2::ADIOS adios;
    std::size_t bad = 0;
    for (int t = 0; t < g_threads; ++t)
    {
        try
        {
            adios2::IO io = adios.DeclareIO("rd" + std::to_string(t));
            io.SetEngine("BP5");
            adios2::Engine e = io.Open(fileName(t), adios2::Mode::ReadRandomAccess);
            for (int v = 0; v < g_varsPerThread; ++v)
            {
                auto var = io.InquireVariable<int32_t>(varName(v));
                if (!var)
                {
                    ++bad;
                    continue;
                }
                std::vector<int32_t> got;
                e.Get(var, got, adios2::Mode::Sync);
                if (got.size() != g_elems)
                {
                    ++bad;
                    continue;
                }
                for (std::size_t i = 0; i < g_elems; ++i)
                    if (got[i] != patternValue(t, v, i))
                    {
                        ++bad;
                        break;
                    }
            }
            e.Close();
        }
        catch (const std::exception &ex)
        {
            std::cout << "  read-back exception on stream " << t << ": " << ex.what() << "\n";
            ++bad;
        }
    }
    if (bad)
    {
        std::cout << "RESULT: FAIL -- " << bad << " bad variables across separate streams\n";
        return false;
    }
    std::cout << "RESULT: PASS -- every separate stream verified\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--create-in-thread")
            g_createInThread = true;
        else if (a == "--create-upfront")
            g_createInThread = false;
        else if (a == "--threads" && i + 1 < argc)
            g_threads = std::atoi(argv[++i]);
        else if (a == "--vars" && i + 1 < argc)
            g_varsPerThread = std::atoi(argv[++i]);
        else if (a == "--elems" && i + 1 < argc)
            g_elems = static_cast<std::size_t>(std::atoll(argv[++i]));
    }

    std::cout << "=== separate IO+Engine+stream per thread ===\n"
              << "sub-mode    : " << (g_createInThread ? "create-in-thread (concurrent "
                                                          "DeclareIO/Open/Close via shared ADIOS)"
                                                        : "create-upfront (only the write loop is "
                                                          "concurrent)")
              << "\n"
              << "threads     : " << g_threads << "\n"
              << "vars/thread : " << g_varsPerThread << "\n"
              << "elems/var   : " << g_elems << "\n"
              << "----------------------------------------------\n";

    // ONE shared ADIOS factory, as in typical multithreaded usage.
    adios2::ADIOS adios;
    std::atomic<bool> threw{false};
    std::string err;
    std::mutex errMutex;

    if (g_createInThread)
    {
        // Each thread does the whole lifecycle itself, concurrently.
        SpinBarrier barrier(g_threads);
        std::vector<std::thread> pool;
        for (int t = 0; t < g_threads; ++t)
        {
            pool.emplace_back([&, t] {
                try
                {
                    auto data = makeData(t);
                    barrier.arrive_and_wait();
                    adios2::IO io = adios.DeclareIO("wr" + std::to_string(t));
                    io.SetEngine("BP5");
                    std::vector<adios2::Variable<int32_t>> vars;
                    for (int v = 0; v < g_varsPerThread; ++v)
                        vars.push_back(io.DefineVariable<int32_t>(varName(v), {g_elems}, {0},
                                                                  {g_elems}, adios2::ConstantDims));
                    adios2::Engine e = io.Open(fileName(t), adios2::Mode::Write);
                    e.BeginStep();
                    for (int v = 0; v < g_varsPerThread; ++v)
                        e.Put(vars[v], data[v].data(), adios2::Mode::Sync);
                    e.EndStep();
                    e.Close();
                }
                catch (const std::exception &ex)
                {
                    std::lock_guard<std::mutex> lk(errMutex);
                    if (!threw.exchange(true))
                        err = ex.what();
                }
            });
        }
        for (auto &th : pool)
            th.join();
    }
    else
    {
        // Create & open everything serially in the main thread.
        std::vector<adios2::IO> ios;
        std::vector<adios2::Engine> engines;
        std::vector<std::vector<adios2::Variable<int32_t>>> vars(g_threads);
        std::vector<std::vector<std::vector<int32_t>>> data(g_threads);
        for (int t = 0; t < g_threads; ++t)
        {
            adios2::IO io = adios.DeclareIO("wr" + std::to_string(t));
            io.SetEngine("BP5");
            for (int v = 0; v < g_varsPerThread; ++v)
                vars[t].push_back(io.DefineVariable<int32_t>(varName(v), {g_elems}, {0}, {g_elems},
                                                             adios2::ConstantDims));
            data[t] = makeData(t);
            ios.push_back(io);
            engines.push_back(io.Open(fileName(t), adios2::Mode::Write));
        }
        // Only the write loop is concurrent.
        SpinBarrier barrier(g_threads);
        std::vector<std::thread> pool;
        for (int t = 0; t < g_threads; ++t)
        {
            pool.emplace_back([&, t] {
                try
                {
                    barrier.arrive_and_wait();
                    engines[t].BeginStep();
                    for (int v = 0; v < g_varsPerThread; ++v)
                        engines[t].Put(vars[t][v], data[t][v].data(), adios2::Mode::Sync);
                    engines[t].EndStep();
                }
                catch (const std::exception &ex)
                {
                    std::lock_guard<std::mutex> lk(errMutex);
                    if (!threw.exchange(true))
                        err = ex.what();
                }
            });
        }
        for (auto &th : pool)
            th.join();
        // Close serially.
        for (int t = 0; t < g_threads; ++t)
            engines[t].Close();
    }

    if (threw.load())
    {
        std::cout << "RESULT: FAIL -- exception during concurrent run: " << err << "\n";
        return 1;
    }

    return verify() ? 0 : 1;
}
