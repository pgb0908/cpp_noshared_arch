#include "net/boost/socket.hpp"

#include <cassert>

#include "net/boost/error.hpp"

namespace net::boost_asio {

BoostSocket::BoostSocket(net::IEventLoop& owner, ::boost::asio::io_context& io_context)
    : owner_(owner), socket_(io_context) {}

BoostSocket::BoostSocket(net::IEventLoop& owner, ::boost::asio::io_context& io_context,
                          ::boost::asio::ip::tcp::socket existing)
    : owner_(owner), socket_(std::move(existing)) {
    (void)io_context;
    ::boost::system::error_code ec;
    socket_.set_option(::boost::asio::ip::tcp::no_delay(true), ec);
}

BoostSocket::BoostSocket(net::IEventLoop& owner, ::boost::asio::io_context& io_context, int native_fd)
    : owner_(owner), socket_(io_context, ::boost::asio::ip::tcp::v4(), native_fd) {
    ::boost::system::error_code ec;
    socket_.set_option(::boost::asio::ip::tcp::no_delay(true), ec);
}

// TCP_NODELAY(Nagle 비활성화) -- keep-alive 도입 후 wrk 부하테스트에서
// p50 latency가 ~41ms에 고정적으로 뭉치는 걸 발견 (Nagle + 수신측 delayed
// ACK의 전형적인 상호작용 증상, doc/benchmark-report.md 참고). 우리 relay는
// 헤더/바디를 여러 번의 작은 write로 내보내는 구조라 Nagle이 그 사이를
// 묶어서 지연시키는데, 같은 커넥션에서 요청이 계속 오가는 keep-alive에서는
// 이게 매 요청마다 반복돼서 훨씬 크게 드러난다 (요청마다 새 커넥션이던
// 이전엔 상대적으로 덜 보였음). 게이트웨이가 스스로 애플리케이션 레벨
// 버퍼링/프레이밍을 이미 하고 있어서 Nagle이 줄 수 있는 이득(작은 패킷
// 합치기)이 없고 손해만 있음 -- accept된 downstream, connect된 upstream
// 둘 다 켠다.
void BoostSocket::async_connect(const net::Endpoint& endpoint, net::ErrorCallback cb) {
    assert(is_owned_by_current_thread() && "BoostSocket::async_connect() called from a non-owning thread");
    ::boost::system::error_code parse_ec;
    auto address = ::boost::asio::ip::make_address(endpoint.host, parse_ec);
    if (parse_ec) {
        cb(to_net_error(parse_ec));
        return;
    }
    ::boost::asio::ip::tcp::endpoint ep(address, endpoint.port);
    socket_.async_connect(ep, [this, cb = std::move(cb)](const ::boost::system::error_code& ec) {
        if (!ec) {
            ::boost::system::error_code nodelay_ec;
            socket_.set_option(::boost::asio::ip::tcp::no_delay(true), nodelay_ec);
        }
        cb(to_net_error(ec));
    });
}

void BoostSocket::async_read_some(net::MutableBuffer buffer, net::IoCallback cb) {
    assert(is_owned_by_current_thread() && "BoostSocket::async_read_some() called from a non-owning thread");
    socket_.async_read_some(::boost::asio::buffer(buffer.data, buffer.size),
                             [cb = std::move(cb)](const ::boost::system::error_code& ec, std::size_t n) {
                                 cb(to_net_error(ec), n);
                             });
}

void BoostSocket::async_write(net::ConstBuffer buffer, net::IoCallback cb) {
    assert(is_owned_by_current_thread() && "BoostSocket::async_write() called from a non-owning thread");
    ::boost::asio::async_write(socket_, ::boost::asio::buffer(buffer.data, buffer.size),
                                [cb = std::move(cb)](const ::boost::system::error_code& ec, std::size_t n) {
                                    cb(to_net_error(ec), n);
                                });
}

void BoostSocket::shutdown() {
    assert(is_owned_by_current_thread() && "BoostSocket::shutdown() called from a non-owning thread");
    ::boost::system::error_code ec;
    socket_.shutdown(::boost::asio::ip::tcp::socket::shutdown_both, ec);
}

void BoostSocket::close() {
    assert(is_owned_by_current_thread() && "BoostSocket::close() called from a non-owning thread");
    ::boost::system::error_code ec;
    socket_.close(ec);
}

bool BoostSocket::is_open() const { return socket_.is_open(); }

void BoostSocket::cancel() {
    assert(is_owned_by_current_thread() && "BoostSocket::cancel() called from a non-owning thread");
    ::boost::system::error_code ec;
    socket_.cancel(ec);
}

// 소유권 검증 없음: adopt_socket()의 재바인딩 절차 자체가 이 메서드를
// "옛 소켓의 원래 소유 스레드가 아닌, 대상 shard 스레드"에서 의도적으로
// 호출한다 (net/event_loop.hpp의 AdoptedSocket 참고). 여기 검증을 걸면
// 그 절차 자체가 깨진다 -- fd만 뽑아내고 리액터 등록은 안 건드리는
// 안전한 연산이라 스레드 무관하게 허용해도 된다.
int BoostSocket::release_native_handle() { return socket_.release(); }

}  // namespace net::boost_asio
