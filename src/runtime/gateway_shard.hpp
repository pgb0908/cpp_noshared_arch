#pragma once

#include <cstddef>
#include <memory>
#include <thread>

#include "config/config.hpp"
#include "net/event_loop.hpp"
#include "net/socket.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// 1 CPU Core = 1 Thread = 1 event loop = 1 GatewayShard.
// Every Connection accepted onto this shard is owned by it for its whole
// lifetime; all I/O for that connection runs on this shard's thread only.
class GatewayShard {
public:
    GatewayShard(std::size_t index, const Config& config);

    // Spawns the shard's thread, pins it to cpu_core, resolves upstreams,
    // and runs the event loop.
    void start(int cpu_core);

    // Stops the event loop and joins the thread. Safe to call once after start().
    void stop();

    // Entry point for a newly accepted socket. Must only be invoked via
    // event_loop().post(...) from the Listener thread, and the socket must
    // already be adopt_socket()-ed onto this shard's event loop -- never
    // called directly across threads with a foreign-loop socket.
    void dispatch_accept(std::unique_ptr<net::ISocket> socket);

    net::IEventLoop& event_loop() { return *event_loop_; }
    const LocalMetrics& metrics() const { return metrics_; }
    std::size_t index() const { return index_; }

private:
    std::size_t index_;
    const Config& config_;

    std::unique_ptr<net::IEventLoop> event_loop_;
    std::thread thread_;

    BufferPool buffer_pool_;
    LocalMetrics metrics_;
    UpstreamManager upstream_manager_;
};
