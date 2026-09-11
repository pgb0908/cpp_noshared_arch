#include "upstream_manager.hpp"

#include <iostream>

UpstreamManager::UpstreamManager(boost::asio::io_context& io_context, const Config& config)
    : io_context_(io_context), config_(config), dns_refresh_timer_(io_context) {
    endpoints_.reserve(config_.upstreams.size());
    for (const auto& u : config_.upstreams) {
        endpoints_.push_back(Endpoint{u.host, u.port, boost::asio::ip::tcp::endpoint{}});
    }
    idle_pools_.resize(endpoints_.size());
}

void UpstreamManager::start() {
    resolve_all();
    schedule_refresh();
}

void UpstreamManager::resolve_all() {
    // Synchronous resolve is fine here: this runs once at shard startup and
    // then only once per dns_refresh_interval_seconds -- not the hot path.
    boost::asio::ip::tcp::resolver resolver(io_context_);
    for (auto& ep : endpoints_) {
        boost::system::error_code ec;
        auto results = resolver.resolve(ep.host, std::to_string(ep.port), ec);
        if (ec || results.empty()) {
            std::cerr << "warning: failed to resolve upstream " << ep.host << ":" << ep.port << ": "
                       << ec.message() << " (keeping previous address)\n";
            continue;
        }
        ep.resolved = *results.begin();
    }
}

void UpstreamManager::schedule_refresh() {
    dns_refresh_timer_.expires_after(std::chrono::seconds(config_.dns_refresh_interval_seconds));
    dns_refresh_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            return;  // timer cancelled -- shard is shutting down
        }
        resolve_all();
        schedule_refresh();
    });
}

std::size_t UpstreamManager::select_endpoint() {
    const std::size_t idx = next_endpoint_;
    next_endpoint_ = (next_endpoint_ + 1) % endpoints_.size();
    return idx;
}

const boost::asio::ip::tcp::endpoint& UpstreamManager::endpoint_address(std::size_t index) const {
    return endpoints_[index].resolved;
}

std::unique_ptr<boost::asio::ip::tcp::socket> UpstreamManager::acquire_pooled_connection(std::size_t index) {
    auto& pool = idle_pools_[index];
    if (pool.empty()) {
        return nullptr;
    }
    auto socket = std::move(pool.back());
    pool.pop_back();
    return socket;
}

void UpstreamManager::release_connection(std::size_t index, std::unique_ptr<boost::asio::ip::tcp::socket> socket) {
    auto& pool = idle_pools_[index];
    if (pool.size() >= config_.connection_pool_max_idle_per_endpoint) {
        boost::system::error_code ec;
        socket->close(ec);
        return;
    }
    pool.push_back(std::move(socket));
}
