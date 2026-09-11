#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "net/acceptor.hpp"
#include "net/event_loop.hpp"
#include "runtime/gateway_shard.hpp"

// A single acceptor running on its own dedicated thread/event loop, kept
// separate from every shard so accept load never competes with a shard's
// relay processing. Round-robins each accepted socket to a shard via
// event_loop().post() -- never touches shard-internal state directly (per
// the architecture's cross-core communication rule).
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
                return;  // acceptor closed intentionally -- don't re-arm
            }

            if (err.ok()) {
                GatewayShard& target_shard = *shards_[next_shard_];
                next_shard_ = (next_shard_ + 1) % shards_.size();

                net::IEventLoop& target_loop = target_shard.event_loop();
                // IEventLoop::post() takes a std::function<void()>, which
                // requires a copy-constructible target even though it will
                // only ever run once here -- a unique_ptr capture alone
                // isn't copyable, so it's boxed in a shared_ptr first.
                auto boxed_sock = std::make_shared<std::unique_ptr<net::ISocket>>(std::move(socket));
                target_loop.post([&target_shard, &target_loop, boxed_sock]() {
                    // The accepted socket is still bound to THIS Listener's
                    // event loop at this point. adopt_socket() rebinds it
                    // to the shard's own loop so all subsequent I/O for
                    // this connection actually runs on the shard's thread
                    // -- not the listener's. See net/event_loop.hpp.
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
    // Only ever touched by this Listener's single accept-loop thread --
    // plain std::size_t, no atomic needed.
    std::size_t next_shard_ = 0;
    bool stopping_ = false;
};
