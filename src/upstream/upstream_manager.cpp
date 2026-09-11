#include "upstream/upstream_manager.hpp"

#include <cassert>
#include <iostream>

UpstreamManager::UpstreamManager(net::IEventLoop& event_loop, const Config& config)
    : event_loop_(event_loop), config_(config) {
    endpoints_.reserve(config_.upstreams.size());
    for (const auto& u : config_.upstreams) {
        endpoints_.push_back(Endpoint{u.host, u.port, net::Endpoint{}});
    }
    idle_pools_.resize(endpoints_.size());
}

void UpstreamManager::start() {
    // Created here (not in the constructor) because GatewayShard builds
    // UpstreamManager before its event loop's thread starts running --
    // resolver_/dns_refresh_timer_ are only ever touched from that thread.
    resolver_ = event_loop_.create_resolver();
    dns_refresh_timer_ = event_loop_.create_timer();
    resolve_all();
    schedule_refresh();
}

void UpstreamManager::resolve_all() {
    // Synchronous resolve is fine here: this runs once at shard startup and
    // then only once per dns_refresh_interval_seconds -- not the hot path.
    for (auto& ep : endpoints_) {
        auto [err, results] = resolver_->resolve(ep.host, ep.port);
        if (!err.ok() || results.empty()) {
            std::cerr << "warning: failed to resolve upstream " << ep.host << ":" << ep.port << ": " << err.message
                       << " (keeping previous address)\n";
            continue;
        }
        ep.resolved = results.front();
    }
}

void UpstreamManager::schedule_refresh() {
    dns_refresh_timer_->expires_after(std::chrono::seconds(config_.dns_refresh_interval_seconds));
    dns_refresh_timer_->async_wait([this](const net::Error& err) {
        if (!err.ok()) {
            return;  // timer cancelled -- shard is shutting down
        }
        resolve_all();
        schedule_refresh();
    });
}

std::size_t UpstreamManager::select_endpoint() {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");
    const std::size_t idx = next_endpoint_;
    next_endpoint_ = (next_endpoint_ + 1) % endpoints_.size();
    return idx;
}

void UpstreamManager::acquire_connection(std::size_t index, net::SocketCallback callback) {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");

    auto& pool = idle_pools_[index];
    if (!pool.empty()) {
        auto socket = std::move(pool.back());
        pool.pop_back();
        callback(net::Error::none(), std::move(socket));
        return;
    }

    auto socket = event_loop_.create_socket();
    net::ISocket* raw = socket.get();
    const net::Endpoint target = endpoints_[index].resolved;
    // net::ErrorCallback is a std::function, which requires a
    // copy-constructible target -- a unique_ptr capture alone isn't
    // copyable, even though this callback only ever runs once, so it's
    // boxed in a shared_ptr first (same pattern as Listener::do_accept()).
    auto boxed_socket = std::make_shared<std::unique_ptr<net::ISocket>>(std::move(socket));
    raw->async_connect(target, [boxed_socket, callback = std::move(callback)](const net::Error& err) {
        if (!err.ok()) {
            callback(err, nullptr);
            return;
        }
        callback(net::Error::none(), std::move(*boxed_socket));
    });
}

void UpstreamManager::release_connection(std::size_t index, std::unique_ptr<net::ISocket> socket) {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");

    auto& pool = idle_pools_[index];
    if (pool.size() >= config_.connection_pool_max_idle_per_endpoint) {
        socket->close();
        return;
    }
    pool.push_back(std::move(socket));
}
