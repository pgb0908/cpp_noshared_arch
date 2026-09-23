#pragma once

#include <boost/asio.hpp>

#include "net/event_loop.hpp"
#include "net/socket.hpp"

namespace net::boost_asio {

class BoostSocket : public net::ISocket {
public:
    // owner: 이 소켓이 실제로 속한 event loop -- is_owned_by_current_thread()가
    // 여기다 위임한다. 호출부는 항상 이 소켓을 생성하는 IEventLoop
    // 구현체 자신(*this)을 넘긴다.
    BoostSocket(net::IEventLoop& owner, ::boost::asio::io_context& io_context);
    BoostSocket(net::IEventLoop& owner, ::boost::asio::io_context& io_context,
                ::boost::asio::ip::tcp::socket existing);
    // io_context에 바인딩된 상태로 생성하되, 다른 io_context의 소켓에서
    // release된(이미 열려있는) native handle(POSIX fd)의 소유권을
    // 넘겨받는다 -- net::IEventLoop::adopt_socket() 참고.
    BoostSocket(net::IEventLoop& owner, ::boost::asio::io_context& io_context, int native_fd);

    void async_connect(const net::Endpoint& endpoint, net::ErrorCallback cb) override;
    void async_read_some(net::MutableBuffer buffer, net::IoCallback cb) override;
    void async_write(net::ConstBuffer buffer, net::IoCallback cb) override;
    void shutdown() override;
    void close() override;
    bool is_open() const override;
    void cancel() override;
    int release_native_handle() override;
    bool is_owned_by_current_thread() const override { return owner_.is_current_thread(); }

private:
    net::IEventLoop& owner_;
    ::boost::asio::ip::tcp::socket socket_;
};

}  // namespace net::boost_asio
