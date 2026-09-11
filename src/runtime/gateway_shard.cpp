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
    // This is the exact spot where the cross-thread accept handoff bug
    // used to bite: the socket must already be rebound to THIS shard's
    // event loop (by Listener, via adopt_socket()) before it ever gets
    // here. If it isn't, every Session callback for this connection would
    // silently run on the Listener thread instead of this shard's thread --
    // see net/event_loop.hpp's adopt_socket() doc comment for the full story.
    assert(event_loop_->is_current_thread() && "dispatch_accept() called from a non-owning thread");

    auto session =
        std::make_shared<Session>(std::move(socket), *event_loop_, upstream_manager_, buffer_pool_, metrics_);
    session->start();
}
