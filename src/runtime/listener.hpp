#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "net/acceptor.hpp"
#include "net/event_loop.hpp"
#include "runtime/gateway_shard.hpp"

// 전용 스레드/event loop에서 도는 단일 acceptor. 모든 shard와 분리돼
// 있어서 accept 부하가 shard의 relay 처리와 절대 경합하지 않는다.
// accept된 소켓을 event_loop().post()를 통해 shard에 round-robin으로
// 분배한다 -- shard 내부 상태를 절대 직접 건드리지 않음 (아키텍처의
// cross-core 통신 규칙에 따름).
class Listener {
public:
    Listener(net::IEventLoop& accept_event_loop, uint16_t port, std::vector<std::unique_ptr<GatewayShard>>& shards)
        : event_loop_(accept_event_loop), shards_(shards) {
        acceptor_ = event_loop_.create_acceptor(port);
    }

    void start() { do_accept(); }

    void stop() {
        stopping_ = true;
        acceptor_->close();
    }

private:
    void do_accept() {
        acceptor_->async_accept([this](const net::Error& err, std::unique_ptr<net::ISocket> socket) {
            if (stopping_) {
                return;  // acceptor를 의도적으로 닫은 것 -- 재무장하지 않음
            }

            if (err.ok()) {
                GatewayShard& target_shard = *shards_[next_shard_];
                next_shard_ = (next_shard_ + 1) % shards_.size();

                net::IEventLoop& target_loop = target_shard.event_loop();
                // IEventLoop::post()는 std::function<void()>를 받는데,
                // 이건 어차피 한 번만 실행될 콜백이라도 복사 생성 가능한
                // 대상을 요구한다 -- unique_ptr 캡처만으로는 복사가 안
                // 되므로 shared_ptr로 먼저 감싼다.
                auto boxed_sock = std::make_shared<std::unique_ptr<net::ISocket>>(std::move(socket));
                target_loop.post([&target_shard, &target_loop, boxed_sock]() {
                    // 이 시점에 accept된 소켓은 아직 이 Listener의 event
                    // loop에 바인딩돼 있다. adopt_socket()이 이걸 shard
                    // 자신의 loop로 재바인딩해서, 이 connection의 이후
                    // 모든 I/O가 실제로 shard의 스레드에서 실행되게
                    // 만든다 -- listener 스레드가 아니라. net/event_loop.hpp
                    // 참고.
                    auto rebound = target_loop.adopt_socket(std::move(*boxed_sock));
                    target_shard.dispatch_accept(std::move(rebound));
                });
            } else {
                std::cerr << "accept error: " << err.message << "\n";
            }

            do_accept();
        });
    }

    net::IEventLoop& event_loop_;
    std::unique_ptr<net::IAcceptor> acceptor_;
    std::vector<std::unique_ptr<GatewayShard>>& shards_;
    // 이 Listener의 단일 accept-loop 스레드만 건드림 -- 평범한
    // std::size_t, atomic 불필요.
    std::size_t next_shard_ = 0;
    bool stopping_ = false;
};
