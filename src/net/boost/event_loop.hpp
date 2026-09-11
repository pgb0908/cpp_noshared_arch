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
    void post(std::function<void()> task) override;
    bool is_current_thread() const noexcept override;

    std::unique_ptr<net::ISocket> create_socket() override;
    std::unique_ptr<net::IAcceptor> create_acceptor(uint16_t port) override;
    std::unique_ptr<net::IResolver> create_resolver() override;
    std::unique_ptr<net::ITimer> create_timer() override;
    std::unique_ptr<net::ISocket> adopt_socket(std::unique_ptr<net::ISocket> foreign_socket) override;

private:
    ::boost::asio::io_context io_context_;
    ::boost::asio::executor_work_guard<::boost::asio::io_context::executor_type> work_guard_;

    // Recorded on entry to run() so is_current_thread() can be checked from
    // any thread. std::thread::id has no "unset" sentinel that's safe to
    // compare before run() has ever executed, so we gate on has_run_.
    std::atomic<bool> has_run_{false};
    std::thread::id owner_thread_id_{};
};

}  // namespace net::boost_asio
