/*
 * dtl_bench.cpp — DTLMod simulation counterpart of adios_bench.cpp.
 *
 * Builds a SimGrid platform whose resource rates (disk bandwidth, link
 * bandwidth/latency) are passed on the command line so they can be CALIBRATED
 * to the machine on which the real ADIOS2 numbers were collected. It then runs
 * the same publish/subscribe scenario DTLMod is meant to model and prints the
 * SIMULATED write/read time in a CSV format that mirrors the ADIOS2 harness.
 *
 *   engine=File    transport=File              -> models ADIOS2 BP
 *   engine=Staging transport=Mailbox           -> models ADIOS2 SST (network cost)
 *   engine=Staging transport=MQ                -> idealized zero-cost staging
 *
 * Data model is identical to adios_bench: a 1-D global double array of
 * (npub*nelem) elements; publisher i owns [i*nelem, nelem). For the "own"
 * read pattern with nsub==npub, subscriber i reads exactly publisher i's block.
 */

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <simgrid/s4u.hpp>

#include <fsmod/FileSystem.hpp>
#include <fsmod/OneDiskStorage.hpp>

#include "dtlmod/DTL.hpp"
#include "dtlmod/DTLException.hpp"

namespace sg4  = simgrid::s4u;
namespace sgfs = simgrid::fsmod;

XBT_LOG_NEW_DEFAULT_CATEGORY(dtl_bench, "DTLMod benchmark");

struct Config {
    std::string engine    = "File";    // File | Staging
    std::string transport = "File";    // File | Mailbox | MQ
    size_t bytes          = 1 << 20;   // bytes per rank
    int steps             = 11;
    int warmup            = 1;
    int npub              = 1;
    int nsub              = 1;
    std::string read_pattern = "own";  // own | full
    std::string disk_bw   = "2000MBps";
    std::string disk_read_bw  = "";    // overrides disk_bw for reads if set
    std::string disk_write_bw = "";    // overrides disk_bw for writes if set
    std::string disk_lat  = "0us";
    std::string link_bw   = "10000MBps";
    std::string link_lat  = "10us";
    std::string host_speed = "1Gf";
    std::string tag       = "";
};

static Config cfg;
static const size_t DSZ = sizeof(double);

// Shared result slots, written by actors, reduced by main after run().
static std::vector<double> g_pub_time;   // accumulated write step time per pub
static std::vector<double> g_sub_time;   // accumulated read  step time per sub
static double g_pub_open = 0, g_pub_close = 0, g_sub_open = 0, g_sub_close = 0;
static std::mutex g_mtx;
// Set once all publishers have closed+disconnected. The File-engine subscriber
// waits on this so its read phase never overlaps the write phase (a pure read
// measurement, matching ADIOS2's separate writer/reader runs).
static std::atomic<int> g_pubs_done{0};

static size_t nelem() { return std::max<size_t>(1, cfg.bytes / DSZ); }
static size_t gN()    { return static_cast<size_t>(cfg.npub) * nelem(); }

// ----------------------------------------------------------------------------
// Platform construction
// ----------------------------------------------------------------------------

// File platform: a star zone, one host per rank, plus a shared storage server
// holding a single disk mounted as the parallel file system at /pfs/. All
// publishers write to the same device, so they contend for its bandwidth just
// like N ranks writing to one node-local filesystem.
//
// The ADIOS2 ground truth was collected on ONE physical node, so the faithful
// model is a single host with a single shared disk: every rank writes/reads the
// same device and therefore contends for its bandwidth, exactly like N MPI ranks
// hitting one node-local filesystem. No network hop confounds the disk model.
static void build_file_platform() {
    auto* root    = sg4::Engine::get_instance()->get_netzone_root();
    auto* cluster = root->add_netzone_star("cluster");

    // One compute host per rank (matches DTLMod's validated multi-pub tests) plus
    // a storage server holding a single shared disk. Node<->storage links are made
    // effectively free so the single shared disk is the only bottleneck, giving the
    // same N-ranks-contend-for-one-device behaviour as a node-local filesystem.
    std::string rbw = cfg.disk_read_bw.empty()  ? cfg.disk_bw : cfg.disk_read_bw;
    std::string wbw = cfg.disk_write_bw.empty() ? cfg.disk_bw : cfg.disk_write_bw;
    auto* server = cluster->add_host("storage_server", cfg.host_speed);
    auto* disk   = server->add_disk("disk", rbw, wbw);  // (read_bw, write_bw)
    auto* slink  = cluster->add_link("storage_link", "1000GBps")->set_latency("0us");
    cluster->add_route(server, nullptr, {sg4::LinkInRoute(slink)}, false);
    cluster->add_route(nullptr, server, {sg4::LinkInRoute(slink)}, false);

    int nhosts = std::max(cfg.npub, cfg.nsub);
    for (int i = 0; i < nhosts; ++i) {
        std::string h = "node-" + std::to_string(i);
        auto* host    = cluster->add_host(h, cfg.host_speed);
        auto* link    = cluster->add_link(h + "_link", "1000GBps")->set_latency("0us");
        cluster->add_route(host, nullptr, {sg4::LinkInRoute(link)}, false);
        cluster->add_route(nullptr, host, {sg4::LinkInRoute(link)}, false);
    }
    cluster->seal();

    auto storage = sgfs::OneDiskStorage::create("storage", disk);
    auto fs = sgfs::FileSystem::create("my_fs");
    sgfs::FileSystem::register_file_system(cluster, fs);
    fs->mount_partition("/scratch/", storage, "500TB");
}

// Staging platform, single-node faithful: all publishers share one host, all
// subscribers share another, joined by ONE link whose (bandwidth, latency) are
// the calibration knobs. With M producers and N consumers the M*N transfers all
// share that link, modelling the shared memory/loopback bandwidth of a node.
static void build_staging_platform() {
    auto* root = sg4::Engine::get_instance()->get_netzone_root();
    auto* zone = root->add_netzone_full("cluster");  // full zone: allows the direct prod<->cons route
    auto* prod = zone->add_host("prod", cfg.host_speed);
    auto* cons = zone->add_host("cons", cfg.host_speed);
    auto* link = zone->add_link("L", cfg.link_bw)->set_latency(cfg.link_lat);
    zone->add_route(prod, cons, {sg4::LinkInRoute(link)});
    zone->seal();
    root->seal();
}

// ----------------------------------------------------------------------------
// Actor logic
// ----------------------------------------------------------------------------

static dtlmod::Engine::Type engine_type() {
    return cfg.engine == "Staging" ? dtlmod::Engine::Type::Staging
                                   : dtlmod::Engine::Type::File;
}
static dtlmod::Transport::Method transport_method() {
    if (cfg.transport == "Mailbox") return dtlmod::Transport::Method::Mailbox;
    if (cfg.transport == "MQ")      return dtlmod::Transport::Method::MQ;
    return dtlmod::Transport::Method::File;
}

static void publisher(int id) {
    auto dtl    = dtlmod::DTL::connect();
    auto stream = dtl->add_stream("bench");
    stream->set_engine_type(engine_type());
    stream->set_transport_method(transport_method());

    size_t ne = nelem();
    auto var = stream->define_variable("data", {gN()}, {static_cast<size_t>(id) * ne},
                                       {ne}, DSZ);

    std::string path = (cfg.engine == "Staging")
                           ? std::string("bench")
                           : std::string("cluster:my_fs:/scratch/bench");

    double t0   = sg4::Engine::get_clock();
    auto engine = stream->open(path, dtlmod::Stream::Mode::Publish);
    double open_dt = sg4::Engine::get_clock() - t0;

    // Let all participants open before the first transaction.
    sg4::this_actor::sleep_for(0.5);

    double acc = 0;
    for (int s = 0; s < cfg.steps; ++s) {
        double ts = sg4::Engine::get_clock();
        engine->begin_transaction();
        engine->put(var);
        engine->end_transaction();
        double dt = sg4::Engine::get_clock() - ts;
        if (s >= cfg.warmup) acc += dt;
    }

    double tc = sg4::Engine::get_clock();
    engine->close();
    double close_dt = sg4::Engine::get_clock() - tc;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pub_time[id] = acc;
        g_pub_open  = std::max(g_pub_open, open_dt);
        g_pub_close = std::max(g_pub_close, close_dt);
    }
    dtlmod::DTL::disconnect();
    g_pubs_done.fetch_add(1);
}

static void subscriber(int id) {
    auto dtl = dtlmod::DTL::connect();

    // File engine is desynchronized: wait until ALL publishers have closed and
    // disconnected before reading, so the read phase is measured in isolation
    // (mirrors ADIOS2's separate writer/reader runs). Staging must be concurrent,
    // so subscribers join immediately.
    if (cfg.engine == "File")
        while (g_pubs_done.load() < cfg.npub)
            sg4::this_actor::sleep_for(0.1);

    auto stream = dtl->add_stream("bench");
    if (cfg.engine == "File") {
        stream->set_engine_type(engine_type());
        stream->set_transport_method(transport_method());
    }
    std::string path = (cfg.engine == "Staging")
                           ? std::string("bench")
                           : std::string("cluster:my_fs:/scratch/bench");

    double t0   = sg4::Engine::get_clock();
    auto engine = stream->open(path, dtlmod::Stream::Mode::Subscribe);
    double open_dt = sg4::Engine::get_clock() - t0;

    auto var = stream->inquire_variable("data");
    size_t ne = nelem();
    if (cfg.read_pattern == "own" && cfg.nsub == cfg.npub) {
        var->set_selection({static_cast<size_t>(id) * ne}, {ne});
    } else if (cfg.read_pattern == "own") {
        // uneven: split global array across subscribers
        size_t per = gN() / cfg.nsub;
        var->set_selection({static_cast<size_t>(id) * per}, {per});
    } // "full": leave default selection (whole array)

    double acc = 0;
    for (int s = 0; s < cfg.steps; ++s) {
        double ts = sg4::Engine::get_clock();
        engine->begin_transaction();
        engine->get(var);
        engine->end_transaction();
        double dt = sg4::Engine::get_clock() - ts;
        if (s >= cfg.warmup) acc += dt;
    }

    double tc = sg4::Engine::get_clock();
    engine->close();
    double close_dt = sg4::Engine::get_clock() - tc;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_sub_time[id] = acc;
        g_sub_open  = std::max(g_sub_open, open_dt);
        g_sub_close = std::max(g_sub_close, close_dt);
    }
    dtlmod::DTL::disconnect();
}

// ----------------------------------------------------------------------------

static Config parse(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--engine") c.engine = next();
        else if (a == "--transport") c.transport = next();
        else if (a == "--bytes") c.bytes = std::stoull(next());
        else if (a == "--steps") c.steps = std::stoi(next());
        else if (a == "--warmup") c.warmup = std::stoi(next());
        else if (a == "--npub") c.npub = std::stoi(next());
        else if (a == "--nsub") c.nsub = std::stoi(next());
        else if (a == "--read_pattern") c.read_pattern = next();
        else if (a == "--disk_bw") c.disk_bw = next();
        else if (a == "--disk_read_bw") c.disk_read_bw = next();
        else if (a == "--disk_write_bw") c.disk_write_bw = next();
        else if (a == "--link_bw") c.link_bw = next();
        else if (a == "--link_lat") c.link_lat = next();
        else if (a == "--host_speed") c.host_speed = next();
        else if (a == "--tag") c.tag = next();
    }
    return c;
}

int main(int argc, char** argv) {
    auto* e = new sg4::Engine(&argc, argv);
    // Use the ideal fluid network model (time = latency + size/bandwidth), the
    // same affine form we calibrate against, so the link rate maps 1:1 to the
    // measured staging bandwidth without SimGrid's TCP correction factors.
    sg4::Engine::set_config("network/bandwidth-factor:1.0");
    sg4::Engine::set_config("network/latency-factor:1.0");
    sg4::Engine::set_config("network/weight-S:0");
    cfg = parse(argc, argv);

    g_pub_time.assign(cfg.npub, 0.0);
    g_sub_time.assign(cfg.nsub, 0.0);

    if (cfg.engine == "Staging") build_staging_platform();
    else                         build_file_platform();

    dtlmod::DTL::create();

    // Placement. Staging: all publishers on "prod", all subscribers on "cons".
    // File: rank i on its own "node-i" host (all sharing the single disk), the
    // topology DTLMod's multi-publisher tests exercise.
    for (int i = 0; i < cfg.npub; ++i) {
        std::string host = (cfg.engine == "Staging") ? "prod" : "node-" + std::to_string(i);
        sg4::Host::by_name(host)->add_actor("pub" + std::to_string(i), [i]() { publisher(i); });
    }
    for (int i = 0; i < cfg.nsub; ++i) {
        std::string host = (cfg.engine == "Staging") ? "cons" : "node-" + std::to_string(i);
        sg4::Host::by_name(host)->add_actor("sub" + std::to_string(i), [i]() { subscriber(i); });
    }

    e->run();

    // Reduce: slowest rank dictates the phase time (MAX), matching the ADIOS2
    // MPI_MAX aggregation.
    double write_s = 0, read_s = 0;
    for (double v : g_pub_time) write_s = std::max(write_s, v);
    for (double v : g_sub_time) read_s = std::max(read_s, v);

    int counted = std::max(0, cfg.steps - cfg.warmup);
    double total_bytes_w = static_cast<double>(nelem() * DSZ) * cfg.npub * counted;
    // read total: "own" reads npub blocks total; "full" reads whole array nsub times
    double read_block = (cfg.read_pattern == "full")
                            ? static_cast<double>(gN() * DSZ) * cfg.nsub
                            : static_cast<double>(nelem() * DSZ) * cfg.npub;
    double total_bytes_r = read_block * counted;

    double bw_w = (write_s > 0) ? total_bytes_w / write_s / 1e9 : 0.0;
    double bw_r = (read_s  > 0) ? total_bytes_r / read_s  / 1e9 : 0.0;

    // Two CSV rows mirroring adios_bench RESULT lines (writer + reader).
    printf("RESULT,%s,%s,writer,%d,%zu,%d,%g,%g,%g,%.0f,%g,%g,%g\n",
           cfg.tag.c_str(), cfg.engine.c_str(), cfg.npub, nelem() * DSZ, counted,
           g_pub_open, write_s, g_pub_close, total_bytes_w, bw_w,
           write_s / std::max(1, counted), write_s / std::max(1, counted));
    printf("RESULT,%s,%s,reader,%d,%zu,%d,%g,%g,%g,%.0f,%g,%g,%g\n",
           cfg.tag.c_str(), cfg.engine.c_str(), cfg.nsub, nelem() * DSZ, counted,
           g_sub_open, read_s, g_sub_close, total_bytes_r, bw_r,
           read_s / std::max(1, counted), read_s / std::max(1, counted));

    return 0;
}
