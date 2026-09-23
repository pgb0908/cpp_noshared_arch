#pragma once

#include <boost/asio.hpp>
#include <atomic>
#include <thread>

#include "net/event_loop.hpp"

namespace net::boost_asio {

class BoostEventLoop : public net::IEventLoop {
public:
    BoostEventLoop();

    void run() override;
    void stop() override;
    void post(net::VoidCallback task) override;
    bool is_current_thread() const noexcept override;

    std::unique_ptr<net::ISocket> create_socket() override;
    std::unique_ptr<net::IAcceptor> create_acceptor(uint16_t port) override;
    std::unique_ptr<net::IResolver> create_resolver() override;
    std::unique_ptr<net::ITimer> create_timer() override;

private:
    // base(IEventLoop)에서 private virtual이지만 오버라이드는 문제없다
    // -- net/event_loop.hpp 주석 참고.
    std::unique_ptr<net::ISocket> do_adopt_socket(std::unique_ptr<net::ISocket> foreign_socket) override;

    ::boost::asio::io_context io_context_;
    ::boost::asio::executor_work_guard<::boost::asio::io_context::executor_type> work_guard_;

    // run() 진입 시점에 기록해서 is_current_thread()를 어떤 스레드에서든
    // 확인할 수 있게 함. std::thread::id에는 run()이 한 번도 실행 안
    // 됐을 때 안전하게 비교할 "미설정" sentinel 값이 없어서, has_run_로
    // 게이팅한다.
    std::atomic<bool> has_run_{false};
    std::thread::id owner_thread_id_{};
};

}  // namespace net::boost_asio
