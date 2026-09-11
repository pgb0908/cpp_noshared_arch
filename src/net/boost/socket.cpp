#include "net/boost/socket.hpp"

#include "net/boost/error.hpp"

namespace net::boost_asio {

BoostSocket::BoostSocket(::boost::asio::io_context& io_context) : socket_(io_context) {}

BoostSocket::BoostSocket(::boost::asio::io_context& io_context, ::boost::asio::ip::tcp::socket existing)
    : socket_(std::move(existing)) {
    (void)io_context;
}

BoostSocket::BoostSocket(::boost::asio::io_context& io_context, int native_fd)
    : socket_(io_context, ::boost::asio::ip::tcp::v4(), native_fd) {}

void BoostSocket::async_connect(const net::Endpoint& endpoint, net::ErrorCallback cb) {
    ::boost::system::error_code parse_ec;
    auto address = ::boost::asio::ip::make_address(endpoint.host, parse_ec);
    if (parse_ec) {
        cb(to_net_error(parse_ec));
        return;
    }
    ::boost::asio::ip::tcp::endpoint ep(address, endpoint.port);
    socket_.async_connect(ep, [cb = std::move(cb)](const ::boost::system::error_code& ec) { cb(to_net_error(ec)); });
}

void BoostSocket::async_read_some(net::MutableBuffer buffer, net::IoCallback cb) {
    socket_.async_read_some(::boost::asio::buffer(buffer.data, buffer.size),
                             [cb = std::move(cb)](const ::boost::system::error_code& ec, std::size_t n) {
                                 cb(to_net_error(ec), n);
                             });
}

void BoostSocket::async_write(net::ConstBuffer buffer, net::IoCallback cb) {
    ::boost::asio::async_write(socket_, ::boost::asio::buffer(buffer.data, buffer.size),
                                [cb = std::move(cb)](const ::boost::system::error_code& ec, std::size_t n) {
                                    cb(to_net_error(ec), n);
                                });
}

void BoostSocket::shutdown() {
    ::boost::system::error_code ec;
    socket_.shutdown(::boost::asio::ip::tcp::socket::shutdown_both, ec);
}

void BoostSocket::close() {
    ::boost::system::error_code ec;
    socket_.close(ec);
}

bool BoostSocket::is_open() const { return socket_.is_open(); }

void BoostSocket::cancel() {
    ::boost::system::error_code ec;
    socket_.cancel(ec);
}

int BoostSocket::release_native_handle() { return socket_.release(); }

}  // namespace net::boost_asio
