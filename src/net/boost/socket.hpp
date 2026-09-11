#pragma once

#include <boost/asio.hpp>

#include "net/socket.hpp"

namespace net::boost_asio {

class BoostSocket : public net::ISocket {
public:
    explicit BoostSocket(::boost::asio::io_context& io_context);
    BoostSocket(::boost::asio::io_context& io_context, ::boost::asio::ip::tcp::socket existing);
    // Constructs bound to io_context, taking ownership of an already-open
    // native handle (POSIX fd) released from a socket on a different
    // io_context -- see net::IEventLoop::adopt_socket().
    BoostSocket(::boost::asio::io_context& io_context, int native_fd);

    void async_connect(const net::Endpoint& endpoint, net::ErrorCallback cb) override;
    void async_read_some(net::MutableBuffer buffer, net::IoCallback cb) override;
    void async_write(net::ConstBuffer buffer, net::IoCallback cb) override;
    void shutdown() override;
    void close() override;
    bool is_open() const override;
    void cancel() override;
    int release_native_handle() override;

private:
    ::boost::asio::ip::tcp::socket socket_;
};

}  // namespace net::boost_asio
