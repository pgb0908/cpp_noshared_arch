#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "net/timer.hpp"
#include "runtime/gateway_shard.hpp"

// shard들의 LocalMetrics를 주기적으로 합산해서 stdout에 리포트한다.
// listener_event_loop_ 스레드에서 실행되며, 각 shard의 atomic counter를
// relaxed load로 읽는다 -- shard 스레드의 write와 경합해도 data race가
// 없고(atomic), 값이 리포트 시점과 미세하게 어긋날 수 있는 건 metrics
// 용도로는 근사치면 충분하다는 전제 (문서 section 18).
class MetricsAggregator {
public:
    MetricsAggregator(net::IEventLoop& event_loop, const std::vector<std::unique_ptr<GatewayShard>>& shards,
                       unsigned interval_seconds)
        : event_loop_(event_loop), shards_(shards), interval_seconds_(interval_seconds) {}

    void start() {
        if (interval_seconds_ == 0) {
            return;  // 0이면 주기적 리포트 비활성화
        }
        timer_ = event_loop_.create_timer();
        schedule();
    }

    void stop() {
        if (timer_) {
            timer_->cancel();
        }
    }

private:
    struct Totals {
        uint64_t accepted = 0;
        uint64_t active = 0;
        uint64_t closed = 0;
        uint64_t down_up_bytes = 0;
        uint64_t up_down_bytes = 0;
        uint64_t connect_errors = 0;
        uint64_t requests = 0;
        uint64_t upstream_retries = 0;
    };

    Totals aggregate() const {
        Totals t;
        for (const auto& shard : shards_) {
            const LocalMetrics& m = shard->metrics();
            t.accepted += m.connections_accepted.load(std::memory_order_relaxed);
            t.active += m.connections_active.load(std::memory_order_relaxed);
            t.closed += m.connections_closed.load(std::memory_order_relaxed);
            t.down_up_bytes += m.bytes_downstream_to_upstream.load(std::memory_order_relaxed);
            t.up_down_bytes += m.bytes_upstream_to_downstream.load(std::memory_order_relaxed);
            t.connect_errors += m.upstream_connect_errors.load(std::memory_order_relaxed);
            t.requests += m.requests_handled.load(std::memory_order_relaxed);
            t.upstream_retries += m.upstream_retries.load(std::memory_order_relaxed);
        }
        return t;
    }

    void report_now() const {
        const Totals t = aggregate();
        // 실시간 모니터링용 출력이라, 파일/파이프로 리다이렉트돼도 바로
        // 보이도록 매번 flush한다 (std::endl).
        std::cout << "[metrics] accepted=" << t.accepted << " active=" << t.active << " closed=" << t.closed
                   << " requests=" << t.requests << " down->up=" << t.down_up_bytes
                   << "B up->down=" << t.up_down_bytes << "B connect_errors=" << t.connect_errors
                   << " upstream_retries=" << t.upstream_retries << std::endl;
    }

    void schedule() {
        timer_->expires_after(std::chrono::seconds(interval_seconds_));
        timer_->async_wait([this](const net::Error& err) {
            if (!err.ok()) {
                return;  // cancel됨 -- 종료 중
            }
            report_now();
            schedule();
        });
    }

    net::IEventLoop& event_loop_;
    const std::vector<std::unique_ptr<GatewayShard>>& shards_;
    unsigned interval_seconds_;
    std::unique_ptr<net::ITimer> timer_;
};
