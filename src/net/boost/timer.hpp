#pragma once

#include <boost/asio.hpp>

#include "net/timer.hpp"

namespace net::boost_asio {

class BoostTimer : public net::ITimer {
public:
    explicit BoostTimer(::boost::asio::io_context& io_context);

    void expires_after(std::chrono::seconds duration) override;
    void async_wait(net::ErrorCallback cb) override;
    void cancel() override;

private:
    ::boost::asio::steady_timer timer_;
};

}  // namespace net::boost_asio
