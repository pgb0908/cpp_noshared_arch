#include "runtime/gateway_shard.hpp"

#include <cassert>

#include "filter/default_filters.hpp"
#include "net/boost/factory.hpp"
#include "session/http_session.hpp"
#include "util/cpu_affinity.hpp"

GatewayShard::GatewayShard(std::size_t index, const Config& config)
    : index_(index),
      config_(config),
      event_loop_(net::boost_asio::create_event_loop()),
      buffer_pool_(config.buffer_size, config.buffer_pool_max_free),
      upstream_manager_(*event_loop_, config_),
      filter_chain_(build_default_filter_chain()) {}

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

void GatewayShard::accept_from(std::unique_ptr<net::ISocket> foreign_socket) {
    event_loop_->post([this, socket = std::move(foreign_socket)]() mutable {
        // 이 시점에 socket은 아직 Listener의 event loop에 바인딩돼
        // 있다. adopt_socket()이 이걸 이 shard 자신의 loop로 재바인딩해서,
        // 이후 이 connection의 모든 I/O가 실제로 이 shard의 스레드에서
        // 실행되게 만든다 -- net/event_loop.hpp 참고.
        dispatch_accept(event_loop_->adopt_socket(std::move(socket)));
    });
}

void GatewayShard::dispatch_accept(net::AdoptedSocket socket) {
    assert(event_loop_->is_current_thread() && "dispatch_accept() called from a non-owning thread");

    auto session =
        std::make_shared<HttpSession>(socket.release(), *event_loop_, upstream_manager_, buffer_pool_, filter_chain_,
                                       config_.body_buffer_high_watermark_bytes, metrics_);
    session->start();
}
