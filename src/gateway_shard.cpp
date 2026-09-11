#include "gateway_shard.hpp"

#include "cpu_affinity.hpp"
#include "session.hpp"

GatewayShard::GatewayShard(std::size_t index, const Config& config,
                            boost::asio::ip::tcp::endpoint upstream_endpoint)
    : index_(index),
      config_(config),
      upstream_endpoint_(std::move(upstream_endpoint)),
      io_context_(1),
      work_guard_(boost::asio::make_work_guard(io_context_)),
      buffer_pool_(config.buffer_size) {}

void GatewayShard::start(int cpu_core) {
    thread_ = std::thread([this, cpu_core] {
        pin_thread_to_cpu(cpu_core);
        io_context_.run();
    });
}

void GatewayShard::stop() {
    work_guard_.reset();
    io_context_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GatewayShard::dispatch_accept(boost::asio::ip::tcp::socket socket) {
    auto session = std::make_shared<Session>(std::move(socket), upstream_endpoint_, buffer_pool_, metrics_);
    session->start();
}
