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
        // NOTE: this socket is bound to io_context_ (this acceptor's loop),
        // not necessarily the loop of whatever shard ends up owning the
        // connection. Callers that hand it off across loops must rebind it
        // via IEventLoop::adopt_socket() before using it -- see
        // net/event_loop.hpp and Listener::do_accept().
        cb(net::Error::none(), std::make_unique<BoostSocket>(io_context_, std::move(peer)));
    });
}

void BoostAcceptor::close() {
    ::boost::system::error_code ec;
    acceptor_.close(ec);
}

}  // namespace net::boost_asio
