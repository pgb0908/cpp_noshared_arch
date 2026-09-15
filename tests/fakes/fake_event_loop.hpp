#pragma once

#include <deque>
#include <stdexcept>
#include <vector>

#include "fakes/fake_resolver.hpp"
#include "fakes/fake_socket.hpp"
#include "fakes/fake_timer.hpp"
#include "net/event_loop.hpp"

// net::IEventLoop의 테스트용 가짜 구현체. 실제 스레드/io_context 없이,
// post()된 작업을 큐에 쌓아두고 테스트가 pump()를 호출할 때만 실행한다
// -- 그래서 async 콜백 체인을 한 스텝씩 결정론적으로 검증할 수 있다.
//
// 단일 스레드로만 쓰이므로 is_current_thread()는 항상 true -- 이
// 프로젝트의 실제 cross-thread 소유권 검증(assert)은 net/boost 쪽
// 구현체와 통합 테스트(tools/test_*.py)로 이미 커버됨. Fake는 그
// 검증의 대상이 아니라, 순수 로직(round-robin, pool, timeout 경합 등)
// 검증이 목적이다.
class FakeEventLoop : public net::IEventLoop {
public:
    void run() override { pump(); }
    void stop() override {}

    void post(net::VoidCallback task) override { queue_.push_back(std::move(task)); }

    bool is_current_thread() const noexcept override { return true; }

    std::unique_ptr<net::ISocket> create_socket() override {
        auto sock = std::make_unique<FakeSocket>(*this);
        created_sockets_.push_back(sock.get());
        return sock;
    }

    std::unique_ptr<net::IAcceptor> create_acceptor(uint16_t) override {
        throw std::logic_error("FakeEventLoop::create_acceptor는 단위 테스트에서 지원 안 함");
    }

    std::unique_ptr<net::IResolver> create_resolver() override {
        auto r = std::make_unique<FakeResolver>();
        last_resolver_ = r.get();
        return r;
    }

    std::unique_ptr<net::ITimer> create_timer() override {
        auto t = std::make_unique<FakeTimer>(*this);
        created_timers_.push_back(t.get());
        return t;
    }

    std::unique_ptr<net::ISocket> adopt_socket(std::unique_ptr<net::ISocket> foreign_socket) override {
        return foreign_socket;  // 단일 loop 테스트라 재바인딩 불필요
    }

    // 큐에 쌓인 작업을 전부(그 작업이 새로 post한 것까지 재귀적으로)
    // 실행한다. FakeSocket/FakeTimer의 콜백은 모두 post()를 거치므로,
    // 어떤 async 조작을 한 뒤 이걸 호출해야 콜백이 실제로 실행된다.
    void pump() {
        while (!queue_.empty()) {
            auto task = std::move(queue_.front());
            queue_.pop_front();
            task();
        }
    }

    FakeSocket* last_socket() const { return created_sockets_.empty() ? nullptr : created_sockets_.back(); }
    const std::vector<FakeSocket*>& created_sockets() const { return created_sockets_; }
    const std::vector<FakeTimer*>& created_timers() const { return created_timers_; }
    FakeResolver* last_resolver() const { return last_resolver_; }

private:
    std::deque<net::VoidCallback> queue_;
    std::vector<FakeSocket*> created_sockets_;
    std::vector<FakeTimer*> created_timers_;
    FakeResolver* last_resolver_ = nullptr;
};
