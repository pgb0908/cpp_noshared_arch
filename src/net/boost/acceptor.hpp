#pragma once

#include <boost/asio.hpp>
#include <cstdint>

#include "net/acceptor.hpp"

namespace net::boost_asio {

class BoostAcceptor : public net::IAcceptor {
public:
    BoostAcceptor(::boost::asio::io_context& io_context, uint16_t port);

    void async_accept(net::AcceptCallback cb) override;
    void close() override;

private:
    ::boost::asio::io_context& io_context_;
    ::boost::asio::ip::tcp::acceptor acceptor_;
};

}  // namespace net::boost_asio
