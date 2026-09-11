#include "runtime/gateway_shard.hpp"

#include "session/session.hpp"
#include "util/cpu_affinity.hpp"

GatewayShard::GatewayShard(std::size_t index, const Config& config)
    : index_(index),
      config_(config),
      io_context_(1),
      work_guard_(boost::asio::make_work_guard(io_context_)),
      buffer_pool_(config.buffer_size),
      upstream_manager_(io_context_, config_) {}

void GatewayShard::start(int cpu_core) {
    thread_ = std::thread([this, cpu_core] {
        pin_thread_to_cpu(cpu_core);
        upstream_manager_.start();
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
    auto session = std::make_shared<Session>(std::move(socket), upstream_manager_, buffer_pool_, metrics_);
    session->start();
}
