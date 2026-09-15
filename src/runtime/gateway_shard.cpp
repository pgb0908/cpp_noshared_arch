#include "runtime/gateway_shard.hpp"

#include <cassert>

#include "net/boost/factory.hpp"
#include "session/session.hpp"
#include "util/cpu_affinity.hpp"

GatewayShard::GatewayShard(std::size_t index, const Config& config)
    : index_(index),
      config_(config),
      event_loop_(net::boost_asio::create_event_loop()),
      buffer_pool_(config.buffer_size),
      upstream_manager_(*event_loop_, config_) {}

void GatewayShard::start(int cpu_core) {
    thread_ = std::thread([this, cpu_core] {
        pin_thread_to_cpu(cpu_core);
        upstream_manager_.start();
        event_loop_->run();
    });
}

void GatewayShard::stop() {
    event_loop_->stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GatewayShard::dispatch_accept(std::unique_ptr<net::ISocket> socket) {
    // 바로 여기가 예전에 cross-thread accept handoff 버그가 터지던
    // 지점이다: 이 소켓은 여기 도달하기 전에 반드시 이 shard의 event
    // loop에 (Listener가 adopt_socket()으로) 재바인딩돼 있어야 한다.
    // 그렇지 않으면 이 connection의 모든 Session 콜백이 이 shard가
    // 아니라 조용히 Listener 스레드에서 실행되어 버린다 -- 전체 경위는
    // net/event_loop.hpp의 adopt_socket() 주석 참고.
    assert(event_loop_->is_current_thread() && "dispatch_accept() called from a non-owning thread");

    auto session =
        std::make_shared<Session>(std::move(socket), *event_loop_, upstream_manager_, buffer_pool_, metrics_);
    session->start();
}
