#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "runtime/gateway_shard.hpp"
#include "runtime/listener.hpp"

// Owns the shard pool and the single accept listener. Each GatewayShard
// resolves and manages its own upstreams independently (see UpstreamManager).
class GatewayRuntime {
public:
    explicit GatewayRuntime(Config config);

    void start();
    void stop();

    // Blocks the calling thread until SIGINT/SIGTERM, then calls stop().
    void run_until_signal();

    void print_metrics_summary() const;

private:
    Config config_;
    std::vector<std::unique_ptr<GatewayShard>> shards_;

    boost::asio::io_context listener_io_context_;
    std::unique_ptr<Listener> listener_;
    std::thread listener_thread_;

    bool stopped_ = false;
};
