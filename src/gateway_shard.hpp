#pragma once

#include <boost/asio.hpp>
#include <thread>

#include "buffer_pool.hpp"
#include "config.hpp"
#include "local_metrics.hpp"

// 1 CPU Core = 1 Thread = 1 io_context = 1 GatewayShard.
// Every Connection accepted onto this shard is owned by it for its whole
// lifetime; all I/O for that connection runs on this shard's thread only.
class GatewayShard {
public:
    GatewayShard(std::size_t index, const Config& config,
                 boost::asio::ip::tcp::endpoint upstream_endpoint);

    // Spawns the shard's thread, pins it to cpu_core, and runs io_context.
    void start(int cpu_core);

    // Stops io_context and joins the thread. Safe to call once after start().
    void stop();

    // Entry point for a newly accepted socket. Must only be invoked via
    // asio::post(io_context(), ...) from the Listener thread -- never called
    // directly across threads.
    void dispatch_accept(boost::asio::ip::tcp::socket socket);

    boost::asio::io_context& io_context() { return io_context_; }
    const LocalMetrics& metrics() const { return metrics_; }
    std::size_t index() const { return index_; }

private:
    std::size_t index_;
    const Config& config_;
    boost::asio::ip::tcp::endpoint upstream_endpoint_;

    boost::asio::io_context io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    std::thread thread_;

    BufferPool buffer_pool_;
    LocalMetrics metrics_;
};
