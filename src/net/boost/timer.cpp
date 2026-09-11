#include "net/boost/timer.hpp"

#include "net/boost/error.hpp"

namespace net::boost_asio {

BoostTimer::BoostTimer(::boost::asio::io_context& io_context) : timer_(io_context) {}

void BoostTimer::expires_after(std::chrono::seconds duration) { timer_.expires_after(duration); }

void BoostTimer::async_wait(net::ErrorCallback cb) {
    timer_.async_wait([cb = std::move(cb)](const ::boost::system::error_code& ec) { cb(to_net_error(ec)); });
}

void BoostTimer::cancel() { timer_.cancel(); }

}  // namespace net::boost_asio
