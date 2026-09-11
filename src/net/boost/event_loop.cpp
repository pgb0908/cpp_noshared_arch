#include "net/boost/event_loop.hpp"

#include "net/boost/acceptor.hpp"
#include "net/boost/resolver.hpp"
#include "net/boost/socket.hpp"
#include "net/boost/timer.hpp"

namespace net::boost_asio {

BoostEventLoop::BoostEventLoop() : io_context_(1), work_guard_(::boost::asio::make_work_guard(io_context_)) {}

void BoostEventLoop::run() {
    owner_thread_id_ = std::this_thread::get_id();
    has_run_.store(true, std::memory_order_release);
    io_context_.run();
}

void BoostEventLoop::stop() {
    work_guard_.reset();
    io_context_.stop();
}

void BoostEventLoop::post(std::function<void()> task) { ::boost::asio::post(io_context_, std::move(task)); }

bool BoostEventLoop::is_current_thread() const noexcept {
    return has_run_.load(std::memory_order_acquire) && std::this_thread::get_id() == owner_thread_id_;
}

std::unique_ptr<net::ISocket> BoostEventLoop::create_socket() { return std::make_unique<BoostSocket>(io_context_); }

std::unique_ptr<net::IAcceptor> BoostEventLoop::create_acceptor(uint16_t port) {
    return std::make_unique<BoostAcceptor>(io_context_, port);
}

std::unique_ptr<net::IResolver> BoostEventLoop::create_resolver() {
    return std::make_unique<BoostResolver>(io_context_);
}

std::unique_ptr<net::ITimer> BoostEventLoop::create_timer() { return std::make_unique<BoostTimer>(io_context_); }

std::unique_ptr<net::ISocket> BoostEventLoop::adopt_socket(std::unique_ptr<net::ISocket> foreign_socket) {
    const int fd = foreign_socket->release_native_handle();
    return std::make_unique<BoostSocket>(io_context_, fd);
}

}  // namespace net::boost_asio
