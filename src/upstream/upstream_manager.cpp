#include "upstream/upstream_manager.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

#include "net/race_with_timeout.hpp"

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
    // BENCH-TEMP: 프로파일링 중 임시로 꺼둠 -- 측정 끝나면 원복.
    // std::cerr << "[debug] acquire_connection index=" << index << " pool_size=" << pool.size() << "\n";
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
    // shared_socket만 여기 남는 이유: start_op(연결 시도)와 cancel_op(타임아웃
    // 시 취소) 둘 다 같은 소켓에 접근해야 해서 이 두 콜백 사이의 공유는
    // 피할 수 없다. 반면 타이머/done 플래그의 shared_ptr 관리는
    // race_with_timeout() 안으로 완전히 숨었다 -- connect-vs-timeout
    // 경쟁이라는 개념 자체가 net/race_with_timeout.hpp의 재사용 가능한
    // 모듈로 뽑혀나갔기 때문 (예전엔 이 함수 안에 shared_ptr 4개로
    // 풀어헤쳐 있었음, 자세한 경위는 doc/plan.md 참고).
    auto shared_socket = std::make_shared<std::unique_ptr<net::ISocket>>(event_loop_.create_socket());
    const net::Endpoint target = endpoints_[index].resolved;

    // race_with_timeout() 호출부에 람다 3개를 그대로 인라인하면 인자
    // 목록 안에서 서로 다른 콜백의 들여쓰기가 겹쳐 보여 어디가 어디
    // 콜백인지 추적하기 어렵다 -- 이름 있는 변수로 먼저 뽑아서 "연결을
    // 시작하는 법 / 취소하는 법 / 결과를 전달하는 법"을 위에서 아래로
    // 순서대로 읽히게 한다.
    auto start_connecting = [shared_socket, target](net::ErrorCallback on_connect_done) {
        (*shared_socket)->async_connect(target, std::move(on_connect_done));
    };

    auto cancel_connecting = [shared_socket] { (*shared_socket)->cancel(); };

    auto deliver_result = [shared_socket, callback = std::move(callback)](const net::Error& err) mutable {
        // err는 race_with_timeout()이 넘겨준 그대로 -- 타임아웃이
        // 이겼으면 그쪽 메시지("operation timed out"), 아니면
        // async_connect가 실제로 실패한 메시지. 여기서 다시 감싸지
        // 않고 그대로 전달한다.
        if (!err.ok()) {
            callback(err, nullptr);
            return;
        }
        callback(net::Error::none(), std::move(*shared_socket));
    };

    net::race_with_timeout(event_loop_, std::chrono::seconds(config_.connect_timeout_seconds),
                            std::move(start_connecting), std::move(cancel_connecting), std::move(deliver_result));
}

void UpstreamManager::release_connection(std::size_t index, std::unique_ptr<net::ISocket> socket) {
    assert(event_loop_.is_current_thread() && "UpstreamManager touched from a non-owning thread");

    auto& pool = idle_pools_[index];
    // BENCH-TEMP: 프로파일링 중 임시로 꺼둠 -- 측정 끝나면 원복.
    // std::cerr << "[debug] release_connection index=" << index << " pool_size_before=" << pool.size() << "\n";
    if (pool.size() >= config_.connection_pool_max_idle_per_endpoint) {
        socket->close();
        return;
    }
    pool.push_back(std::move(socket));
}
