#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"

// Shard-local: owned by exactly one GatewayShard, every method runs on that
// shard's io_context thread only. Each shard resolves and pools upstream
// connections independently -- no cross-shard sharing (shared-nothing per
// doc/per-core-sharded-architecture.md section 10).
class UpstreamManager {
public:
    UpstreamManager(boost::asio::io_context& io_context, const Config& config);

    // Blocking initial DNS resolve for every configured endpoint, then
    // schedules the periodic refresh timer. Call once, before io_context.run().
    void start();

    // Shard-local round-robin endpoint selection -- plain std::size_t, no
    // atomic, since only this shard's thread ever calls it (doc section 17).
    std::size_t select_endpoint();
    const boost::asio::ip::tcp::endpoint& endpoint_address(std::size_t index) const;

    // Returns a pooled, already-connected socket for this endpoint if one is
    // idle, or nullptr if the caller must open a fresh connection. A pooled
    // connection may have gone stale (peer closed) without us noticing yet --
    // accepted MVP gap, see doc/plan.md Phase 4 (health-check timers).
    std::unique_ptr<boost::asio::ip::tcp::socket> acquire_pooled_connection(std::size_t index);

    // Returns a still-healthy socket to the idle pool (bounded by
    // connection_pool_max_idle_per_endpoint); if the pool for this endpoint
    // is already full, the socket is simply closed and dropped.
    void release_connection(std::size_t index, std::unique_ptr<boost::asio::ip::tcp::socket> socket);

private:
    struct Endpoint {
        std::string host;
        unsigned short port;
        boost::asio::ip::tcp::endpoint resolved;
    };

    void resolve_all();
    void schedule_refresh();

    boost::asio::io_context& io_context_;
    const Config& config_;

    std::vector<Endpoint> endpoints_;
    std::size_t next_endpoint_ = 0;

    boost::asio::steady_timer dns_refresh_timer_;
    std::vector<std::vector<std::unique_ptr<boost::asio::ip::tcp::socket>>> idle_pools_;
};
