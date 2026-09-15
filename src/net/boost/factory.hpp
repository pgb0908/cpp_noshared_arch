#pragma once

#include <memory>

#include "net/event_loop.hpp"

// 이 헤더(와 대응 .cpp)는 net/boost/ 밖에서 composition-root 파일
// (GatewayShard, GatewayRuntime)이 구체적인 Boost 기반 net::IEventLoop를
// 얻기 위해 include해야 하는 유일한 곳이다. 다른 도메인 코드(Session,
// UpstreamManager, Listener)는 net::boost_asio를 전혀 건드리지 않는다.
namespace net::boost_asio {

std::unique_ptr<net::IEventLoop> create_event_loop();

}  // namespace net::boost_asio
