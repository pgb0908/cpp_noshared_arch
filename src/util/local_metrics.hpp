#pragma once

#include <atomic>
#include <cstdint>

// write는 소유 shard의 스레드만 하고, read는 MetricsAggregator가 다른
// 스레드에서 주기적으로 한다 -- 그래서 std::atomic + relaxed order를
// 씀 (single-writer/multi-reader라 순서 보장은 필요 없고, 그냥 data
// race를 없애기 위한 용도). mutex 같은 락이 아니라 CPU 원자적 명령어
// 하나라 비용이 거의 없음.
// alignas(64)는 shard별 LocalMetrics가 서로 다른 cache line에 놓이게
// 해서, 여러 shard가 각자 자기 것만 write해도 false sharing이 생기지
// 않도록 함.
struct alignas(64) LocalMetrics {
    std::atomic<uint64_t> connections_accepted{0};
    std::atomic<uint64_t> connections_active{0};
    std::atomic<uint64_t> connections_closed{0};
    std::atomic<uint64_t> bytes_downstream_to_upstream{0};
    std::atomic<uint64_t> bytes_upstream_to_downstream{0};
    std::atomic<uint64_t> upstream_connect_errors{0};
    // keep-alive 도입 이후 connections_accepted와 갈라짐: 커넥션 1개가
    // 여러 요청을 처리할 수 있어서, 이 값/connections_accepted 비율로
    // 커넥션 재사용률을 관찰할 수 있다.
    std::atomic<uint64_t> requests_handled{0};
    std::atomic<uint64_t> upstream_retries{0};

    void on_accept() noexcept {
        connections_accepted.fetch_add(1, std::memory_order_relaxed);
        connections_active.fetch_add(1, std::memory_order_relaxed);
    }

    void on_close() noexcept {
        connections_closed.fetch_add(1, std::memory_order_relaxed);
        connections_active.fetch_sub(1, std::memory_order_relaxed);
    }
};
