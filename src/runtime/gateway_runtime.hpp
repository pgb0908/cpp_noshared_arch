#pragma once

#include <memory>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "net/event_loop.hpp"
#include "runtime/gateway_shard.hpp"
#include "runtime/listener.hpp"
#include "runtime/metrics_aggregator.hpp"

// shard 풀과 단일 accept listener를 소유한다. 각 GatewayShard는 자신의
// upstream을 독립적으로 resolve하고 관리한다 (UpstreamManager 참고).
class GatewayRuntime {
public:
    explicit GatewayRuntime(Config config);

    void start();
    void stop();

    // SIGINT/SIGTERM이 올 때까지 호출한 스레드를 blocking한 뒤 stop() 호출.
    void run_until_signal();

    void print_metrics_summary() const;

private:
    Config config_;
    std::vector<std::unique_ptr<GatewayShard>> shards_;

    std::unique_ptr<net::IEventLoop> listener_event_loop_;
    std::unique_ptr<Listener> listener_;
    std::thread listener_thread_;
    std::unique_ptr<MetricsAggregator> metrics_aggregator_;

    bool stopped_ = false;
};
