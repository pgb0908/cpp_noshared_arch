#pragma once

#include <boost/asio.hpp>

#include "net/socket.hpp"

namespace net::boost_asio {

class BoostSocket : public net::ISocket {
public:
    explicit BoostSocket(::boost::asio::io_context& io_context);
    BoostSocket(::boost::asio::io_context& io_context, ::boost::asio::ip::tcp::socket existing);
    // io_context에 바인딩된 상태로 생성하되, 다른 io_context의 소켓에서
    // release된(이미 열려있는) native handle(POSIX fd)의 소유권을
    // 넘겨받는다 -- net::IEventLoop::adopt_socket() 참고.
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
