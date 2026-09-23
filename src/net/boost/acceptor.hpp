#pragma once

#include <boost/asio.hpp>
#include <cstdint>

#include "net/acceptor.hpp"
#include "net/event_loop.hpp"

namespace net::boost_asio {

class BoostAcceptor : public net::IAcceptor {
public:
    // owner: 이 acceptor가 만들어내는 소켓들이 (adopt_socket()으로
    // 재바인딩되기 전까지) 실제로 속한 event loop -- 항상 이 acceptor를
    // 만드는 IEventLoop 구현체 자신(*this).
    BoostAcceptor(net::IEventLoop& owner, ::boost::asio::io_context& io_context, uint16_t port);

    void async_accept(net::AcceptCallback cb) override;
    void close() override;

private:
    net::IEventLoop& owner_;
    ::boost::asio::io_context& io_context_;
    ::boost::asio::ip::tcp::acceptor acceptor_;
};

}  // namespace net::boost_asio
