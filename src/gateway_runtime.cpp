#include "gateway_runtime.hpp"

#include <iostream>
#include <stdexcept>

GatewayRuntime::GatewayRuntime(Config config) : config_(std::move(config)) {}

void GatewayRuntime::start() {
    // Blocking resolve at startup only -- not on the hot path.
    boost::asio::io_context resolve_io_context;
    boost::asio::ip::tcp::resolver resolver(resolve_io_context);
    boost::system::error_code ec;
    auto results = resolver.resolve(config_.upstream_host, std::to_string(config_.upstream_port), ec);
    if (ec || results.empty()) {
        throw std::runtime_error("failed to resolve upstream " + config_.upstream_host + ":" +
                                  std::to_string(config_.upstream_port) + ": " + ec.message());
    }
    const boost::asio::ip::tcp::endpoint upstream_endpoint = *results.begin();

    shards_.reserve(config_.shard_count);
    for (std::size_t i = 0; i < config_.shard_count; ++i) {
        shards_.push_back(std::make_unique<GatewayShard>(i, config_, upstream_endpoint));
    }
    for (std::size_t i = 0; i < shards_.size(); ++i) {
        shards_[i]->start(static_cast<int>(i));
    }

    listener_ = std::make_unique<Listener>(listener_io_context_, config_.listen_port, shards_);
    listener_->start();
    listener_thread_ = std::thread([this] { listener_io_context_.run(); });

    std::cout << "perCoreShard listening on 0.0.0.0:" << config_.listen_port << " -> "
              << config_.upstream_host << ":" << config_.upstream_port << " with "
              << shards_.size() << " shard(s)\n";
}

void GatewayRuntime::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;

    listener_->stop();
    listener_io_context_.stop();
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    for (auto& shard : shards_) {
        shard->stop();
    }
}

void GatewayRuntime::run_until_signal() {
    boost::asio::io_context signal_io_context;
    boost::asio::signal_set signals(signal_io_context, SIGINT, SIGTERM);
    signals.async_wait([&signal_io_context](const boost::system::error_code&, int) { signal_io_context.stop(); });
    signal_io_context.run();
    stop();
}

void GatewayRuntime::print_metrics_summary() const {
    uint64_t total_accepted = 0;
    uint64_t total_closed = 0;
    uint64_t total_active = 0;
    uint64_t total_down_up_bytes = 0;
    uint64_t total_up_down_bytes = 0;
    uint64_t total_connect_errors = 0;

    std::cout << "\n--- shard metrics ---\n";
    for (const auto& shard : shards_) {
        const LocalMetrics& m = shard->metrics();
        std::cout << "shard " << shard->index() << ": accepted=" << m.connections_accepted
                   << " active=" << m.connections_active << " closed=" << m.connections_closed
                   << " down->up=" << m.bytes_downstream_to_upstream
                   << "B up->down=" << m.bytes_upstream_to_downstream
                   << "B connect_errors=" << m.upstream_connect_errors << "\n";

        total_accepted += m.connections_accepted;
        total_closed += m.connections_closed;
        total_active += m.connections_active;
        total_down_up_bytes += m.bytes_downstream_to_upstream;
        total_up_down_bytes += m.bytes_upstream_to_downstream;
        total_connect_errors += m.upstream_connect_errors;
    }
    std::cout << "total: accepted=" << total_accepted << " active=" << total_active
               << " closed=" << total_closed << " down->up=" << total_down_up_bytes
               << "B up->down=" << total_up_down_bytes << "B connect_errors=" << total_connect_errors
               << "\n";
}
