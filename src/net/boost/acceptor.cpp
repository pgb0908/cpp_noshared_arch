#include "net/boost/acceptor.hpp"

#include "net/boost/error.hpp"
#include "net/boost/socket.hpp"

namespace net::boost_asio {

BoostAcceptor::BoostAcceptor(::boost::asio::io_context& io_context, uint16_t port)
    : io_context_(io_context), acceptor_(io_context) {
    ::boost::asio::ip::tcp::endpoint endpoint(::boost::asio::ip::tcp::v4(), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(::boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

void BoostAcceptor::async_accept(net::AcceptCallback cb) {
    acceptor_.async_accept([this, cb = std::move(cb)](const ::boost::system::error_code& ec,
                                                        ::boost::asio::ip::tcp::socket peer) mutable {
        if (ec) {
            cb(to_net_error(ec), nullptr);
            return;
        }
        // 주의: 이 소켓은 io_context_(이 acceptor의 loop)에 바인딩돼
        // 있고, 이 connection을 최종적으로 소유하게 될 shard의 loop와는
        // 별개다. loop 경계를 넘어 이 소켓을 넘기는 쪽에서는 사용하기
        // 전에 반드시 IEventLoop::adopt_socket()으로 재바인딩해야 한다
        // -- net/event_loop.hpp와 Listener::do_accept() 참고.
        cb(net::Error::none(), std::make_unique<BoostSocket>(io_context_, std::move(peer)));
    });
}

void BoostAcceptor::close() {
    ::boost::system::error_code ec;
    acceptor_.close(ec);
}

}  // namespace net::boost_asio
