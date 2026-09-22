#include "upstream/upstream_manager.hpp"

#include <cassert>
#include <iostream>

UpstreamManager::UpstreamManager(net::IEventLoop& event_loop, const Config& config)
    : event_loop_(event_loop), config_(config) {
    endpoints_.reserve(config_.upstreams.size());
    for (const auto& u : config_.upstreams) {
        endpoints_.push_back(Endpoint{u.host, u.port, net::Endpoint{}});
    }
    idle_pools_.resize(endpoints_.size());
}

void UpstreamManager::start() {
    // 생성자가 아니라 여기서 만드는 이유: GatewayShard는 event loop
    // 스레드가 실제로 돌기 시작하기 전에 UpstreamManager를 만들고 --
    // resolver_/dns_refresh_timer_는 오직 그 스레드에서만 건드려야 함.
    resolver_ = event_loop_.create_resolver();
    dns_refresh_timer_ = event_loop_.create_timer();
    resolve_all();
    schedule_refresh();
}

void UpstreamManager::resolve_all() {
    // 여기선 동기 resolve로 충분함: shard 시작 시 1회, 이후로는
    // dns_refresh_interval_seconds마다 1회뿐 -- hot path 아님.
    for (auto& ep : endpoints_) {
        auto [err, results] = resolver_->resolve(ep.host, ep.port);
        if (!err.ok() || results.empty()) {
            std::cerr << "warning: failed to resolve upstream " << ep.host << ":" << ep.port << ": " << err.message
                       << " (keeping previous address)\n";
            continue;
        }
        ep.resolved = results.front();
    }
}

void UpstreamManager::schedule_refresh() {
    dns_refresh_timer_->expires_after(std::chrono::seconds(config_.dns_refresh_interval_seconds));
    dns_refresh_timer_->async_wait([this](const net::Error& err) {
        if (!err.ok()) {
            return;  // timer가 취소됨 -- shard 종료 중
        }
        resolve_all();
        schedule_refresh();
    });
}

std::size_t UpstreamManager::select_endpoint() {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");
    const std::size_t idx = next_endpoint_;
    next_endpoint_ = (next_endpoint_ + 1) % endpoints_.size();
    return idx;
}

void UpstreamManager::acquire_connection(std::size_t index, net::SocketCallback callback) {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");

    auto& pool = idle_pools_[index];
    // TODO: 임시 디버그 로그 -- keep-alive/retry 도입 중 pool 재사용
    // 여부를 실측 확인하려고 넣어둠. 나중에 제대로 된 로깅 체계(레벨
    // 있는 로거 등)가 생기면 그걸로 교체.
    std::cerr << "[debug] acquire_connection index=" << index << " pool_size=" << pool.size() << "\n";
    if (!pool.empty()) {
        auto socket = std::move(pool.back());
        pool.pop_back();
        callback(net::Error::none(), std::move(socket));
        return;
    }

    connect_fresh(index, std::move(callback));
}

void UpstreamManager::acquire_fresh_connection(std::size_t index, net::SocketCallback callback) {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");
    connect_fresh(index, std::move(callback));
}

void UpstreamManager::connect_fresh(std::size_t index, net::SocketCallback callback) {
    auto socket = event_loop_.create_socket();
    net::ISocket* raw = socket.get();
    const net::Endpoint target = endpoints_[index].resolved;

    // connect 타임아웃: timer와 async_connect가 서로 경쟁하고, 먼저
    // 끝나는 쪽이 상대방의 리소스(소켓/timer)를 정리해줘야 한다. 즉
    // socket/timer/callback 모두 "두 콜백 양쪽에서 접근 가능해야 하는"
    // 진짜 공유 상태다 -- Listener::do_accept()처럼 단순히 한 곳으로만
    // 넘기면 끝나는 상황(그쪽은 MoveOnlyFunction으로 boxing 없이
    // 해결됨)과 달리, 여기선 shared_ptr가 원래 의도된 정확한 도구다.
    auto shared_socket = std::make_shared<std::unique_ptr<net::ISocket>>(std::move(socket));
    auto timer = event_loop_.create_timer();
    auto shared_timer = std::make_shared<std::unique_ptr<net::ITimer>>(std::move(timer));
    auto shared_callback = std::make_shared<net::SocketCallback>(std::move(callback));
    auto done = std::make_shared<bool>(false);

    (*shared_timer)->expires_after(std::chrono::seconds(config_.connect_timeout_seconds));
    (*shared_timer)->async_wait([shared_socket, shared_callback, done](const net::Error& err) {
        if (*done || !err.ok()) {
            return;  // 이미 처리됐거나(connect가 먼저 끝남), timer가 cancel된 것
        }
        *done = true;
        (*shared_socket)->cancel();
        (*shared_callback)(net::Error{1, "upstream connect timed out"}, nullptr);
    });

    raw->async_connect(target, [shared_socket, shared_timer, shared_callback, done](const net::Error& err) {
        if (*done) {
            return;  // 타임아웃이 먼저 발생해서 이미 처리됨
        }
        *done = true;
        (*shared_timer)->cancel();
        if (!err.ok()) {
            (*shared_callback)(err, nullptr);
            return;
        }
        (*shared_callback)(net::Error::none(), std::move(*shared_socket));
    });
}

void UpstreamManager::release_connection(std::size_t index, std::unique_ptr<net::ISocket> socket) {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");

    auto& pool = idle_pools_[index];
    std::cerr << "[debug] release_connection index=" << index << " pool_size_before=" << pool.size() << "\n";
    if (pool.size() >= config_.connection_pool_max_idle_per_endpoint) {
        socket->close();
        return;
    }
    pool.push_back(std::move(socket));
}
