#pragma once

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <vector>

#include "runtime/gateway_shard.hpp"

// A single acceptor running on its own dedicated thread/io_context, kept
// separate from every shard so accept load never competes with a shard's
// relay processing. Round-robins each accepted socket to a shard via
// asio::post -- never touches shard-internal state directly (per the
// architecture's cross-core communication rule).
class Listener {
public:
    Listener(boost::asio::io_context& accept_io_context, unsigned short port,
             std::vector<std::unique_ptr<GatewayShard>>& shards)
        : acceptor_(accept_io_context), shards_(shards) {
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
    }

    void start() { do_accept(); }

    void stop() {
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                GatewayShard& target_shard = *shards_[next_shard_];
                next_shard_ = (next_shard_ + 1) % shards_.size();

                boost::asio::post(target_shard.io_context(), [&target_shard, sock = std::move(socket)]() mutable {
                    target_shard.dispatch_accept(std::move(sock));
                });
            } else if (ec != boost::asio::error::operation_aborted) {
                std::cerr << "accept error: " << ec.message() << "\n";
            }

            if (acceptor_.is_open()) {
                do_accept();
            }
        });
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::unique_ptr<GatewayShard>>& shards_;
    // Only ever touched by this Listener's single accept-loop thread --
    // plain std::size_t, no atomic needed.
    std::size_t next_shard_ = 0;
};
