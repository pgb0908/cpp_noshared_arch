#include "runtime/gateway_runtime.hpp"

// 참고: 이 코드는 이 프로젝트의 Boost 디커플링에서 의도적으로 둔 유일한
// 예외다. SIGINT/SIGTERM 처리는 1회성, control-plane 전용 프로세스
// 생명주기 처리일 뿐 -- request hot path와 무관 -- 이고, Boost.Asio를
// 대체할 어떤 현실적인 라이브러리를 쓰더라도 OS signal 처리는 어차피
// 따로 필요하므로 추상화해봐야 실질적인 교체 가능성을 얻지 못한다.
// 전체 설명은 doc/plan.md 참고.
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>

#include "net/boost/factory.hpp"
#include "util/cpu_topology.hpp"

GatewayRuntime::GatewayRuntime(Config config) : config_(std::move(config)) {}

void GatewayRuntime::start() {
    shards_.reserve(config_.shard_count);
    for (std::size_t i = 0; i < config_.shard_count; ++i) {
        shards_.push_back(std::make_unique<GatewayShard>(i, config_));
    }

    // P-core/E-core처럼 성능이 다른 코어가 섞인 CPU에서 shard를 "성능 높은
    // 코어부터" 채운다 (Phase 5, doc/plan.md 참고). 동종 CPU에서는
    // build_shard_cpu_plan()이 그냥 0..N-1을 돌려줘서 이전과 동일하게 동작.
    const ShardCpuPlan cpu_plan = build_shard_cpu_plan();
    for (std::size_t i = 0; i < shards_.size(); ++i) {
        if (i < cpu_plan.cpus.size()) {
            if (i >= cpu_plan.top_tier_count) {
                std::cerr << "[cpu_affinity] warning: shard " << i << " pinned to cpu" << cpu_plan.cpus[i]
                          << ", a lower-tier core (P-core budget of " << cpu_plan.top_tier_count
                          << " exceeded) -- expect lower per-shard throughput\n";
            }
            shards_[i]->start(cpu_plan.cpus[i]);
        } else {
            // 감지된 논리 CPU 수보다 shard가 많음 -- 예전처럼 그냥 인덱스를
            // 그대로 넘긴다 (pin_thread_to_cpu()가 실패하면 자체적으로
            // 에러를 로깅하고 unpinned로 계속 진행함).
            shards_[i]->start(static_cast<int>(i));
        }
    }

    listener_event_loop_ = net::boost_asio::create_event_loop();
    listener_ = std::make_unique<Listener>(*listener_event_loop_, config_.listen_port, shards_);
    metrics_aggregator_ =
        std::make_unique<MetricsAggregator>(*listener_event_loop_, shards_, config_.metrics_report_interval_seconds);
    listener_->start();
    listener_thread_ = std::thread([this] {
        // MetricsAggregator의 timer도 UpstreamManager의 DNS refresh timer와
        // 같은 이유로 run() 호출 전, 이 스레드 안에서 start()한다.
        metrics_aggregator_->start();
        listener_event_loop_->run();
    });

    std::cout << "perCoreShard listening on 0.0.0.0:" << config_.listen_port << " -> "
              << config_.upstreams.size() << " upstream endpoint(s), " << shards_.size() << " shard(s)"
              << std::endl;
}

void GatewayRuntime::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;

    listener_->stop();
    metrics_aggregator_->stop();
    listener_event_loop_->stop();
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
    uint64_t total_requests = 0;
    uint64_t total_upstream_retries = 0;

    std::cout << "\n--- shard metrics ---\n";
    for (const auto& shard : shards_) {
        const LocalMetrics& m = shard->metrics();
        const uint64_t accepted = m.connections_accepted.load(std::memory_order_relaxed);
        const uint64_t active = m.connections_active.load(std::memory_order_relaxed);
        const uint64_t closed = m.connections_closed.load(std::memory_order_relaxed);
        const uint64_t down_up = m.bytes_downstream_to_upstream.load(std::memory_order_relaxed);
        const uint64_t up_down = m.bytes_upstream_to_downstream.load(std::memory_order_relaxed);
        const uint64_t connect_errors = m.upstream_connect_errors.load(std::memory_order_relaxed);
        const uint64_t requests = m.requests_handled.load(std::memory_order_relaxed);
        const uint64_t upstream_retries = m.upstream_retries.load(std::memory_order_relaxed);

        std::cout << "shard " << shard->index() << ": accepted=" << accepted << " active=" << active
                   << " closed=" << closed << " requests=" << requests << " down->up=" << down_up
                   << "B up->down=" << up_down << "B connect_errors=" << connect_errors
                   << " upstream_retries=" << upstream_retries << "\n";

        total_accepted += accepted;
        total_closed += closed;
        total_active += active;
        total_down_up_bytes += down_up;
        total_up_down_bytes += up_down;
        total_connect_errors += connect_errors;
        total_requests += requests;
        total_upstream_retries += upstream_retries;
    }
    std::cout << "total: accepted=" << total_accepted << " active=" << total_active
               << " closed=" << total_closed << " requests=" << total_requests
               << " down->up=" << total_down_up_bytes << "B up->down=" << total_up_down_bytes
               << "B connect_errors=" << total_connect_errors << " upstream_retries=" << total_upstream_retries
               << "\n";
}
