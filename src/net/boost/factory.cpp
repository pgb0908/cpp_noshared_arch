#include "net/boost/factory.hpp"

#include "net/boost/event_loop.hpp"

namespace net::boost_asio {

std::unique_ptr<net::IEventLoop> create_event_loop() { return std::make_unique<BoostEventLoop>(); }

}  // namespace net::boost_asio
