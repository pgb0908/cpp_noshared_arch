#pragma once

#include <cstdint>

// Always touched only by the owning shard's thread -- no atomics needed.
// alignas(64) keeps distinct shards' metrics on separate cache lines to
// avoid false sharing once these are aggregated from another thread.
struct alignas(64) LocalMetrics {
    uint64_t connections_accepted = 0;
    uint64_t connections_active = 0;
    uint64_t connections_closed = 0;
    uint64_t bytes_downstream_to_upstream = 0;
    uint64_t bytes_upstream_to_downstream = 0;
    uint64_t upstream_connect_errors = 0;

    void on_accept() noexcept {
        ++connections_accepted;
        ++connections_active;
    }

    void on_close() noexcept {
        ++connections_closed;
        --connections_active;
    }
};
