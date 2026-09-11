#pragma once

#include <memory>

#include "net/event_loop.hpp"

// This header (and its .cpp) is the ONLY place outside net/boost/ that a
// composition-root file (GatewayShard, GatewayRuntime) needs to include to
// obtain a concrete, Boost-backed net::IEventLoop. No other domain code
// (Session, UpstreamManager, Listener) touches net::boost_asio at all.
namespace net::boost_asio {

std::unique_ptr<net::IEventLoop> create_event_loop();

}  // namespace net::boost_asio
