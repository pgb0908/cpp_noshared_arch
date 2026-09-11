#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "net/event_loop.hpp"
#include "net/resolver.hpp"
#include "net/socket.hpp"
#include "net/timer.hpp"

// Shard-local: owned by exactly one GatewayShard, every method runs on that
// shard's event loop thread only. Each shard resolves and pools upstream
// connections independently -- no cross-shard sharing (shared-nothing per
// doc/per-core-sharded-architecture.md section 10).
class UpstreamManager {
public:
    UpstreamManager(net::IEventLoop& event_loop, const Config& config);

    // Blocking initial DNS resolve for every configured endpoint, then
    // schedules the periodic refresh timer. Call once, before event_loop.run().
    void start();

    // Shard-local round-robin endpoint selection -- plain std::size_t, no
    // atomic, since only this shard's thread ever calls it (doc section 17).
    std::size_t select_endpoint();

    // Delivers a connected socket for this endpoint via callback: either a
    // pooled idle connection (callback invoked synchronously, before this
    // call returns) or a freshly connected one (invoked once connect
    // completes). Callers must treat both paths identically. On failure,
    // the socket pointer passed to the callback is null.
    void acquire_connection(std::size_t index, net::SocketCallback callback);

    // Returns a still-healthy socket to the idle pool (bounded by
    // connection_pool_max_idle_per_endpoint); if the pool for this endpoint
    // is already full, the socket is simply closed and dropped.
    void release_connection(std::size_t index, std::unique_ptr<net::ISocket> socket);

private:
    struct Endpoint {
        std::string host;
        uint16_t port;
        net::Endpoint resolved;
    };

    void resolve_all();
    void schedule_refresh();

    net::IEventLoop& event_loop_;
    const Config& config_;

    std::unique_ptr<net::IResolver> resolver_;
    std::unique_ptr<net::ITimer> dns_refresh_timer_;

    std::vector<Endpoint> endpoints_;
    std::size_t next_endpoint_ = 0;

    std::vector<std::vector<std::unique_ptr<net::ISocket>>> idle_pools_;
};
