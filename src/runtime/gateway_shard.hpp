#pragma once

#include <cstddef>
#include <memory>
#include <thread>

#include "config/config.hpp"
#include "net/event_loop.hpp"
#include "net/socket.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// 1 CPU Core = 1 Thread = 1 event loop = 1 GatewayShard.
// 이 shard에 accept된 모든 Connection은 생명주기 전체를 이 shard가
// 소유한다; 그 connection의 모든 I/O는 이 shard의 스레드에서만 실행된다.
class GatewayShard {
public:
    GatewayShard(std::size_t index, const Config& config);

    // shard의 스레드를 spawn하고 cpu_core에 pinning한 뒤, upstream을
    // resolve하고 event loop를 실행한다.
    void start(int cpu_core);

    // event loop를 멈추고 스레드를 join한다. start() 이후 한 번 호출하는
    // 건 안전함.
    void stop();

    // 새로 accept된 소켓의 진입점. 반드시 Listener 스레드에서
    // event_loop().post(...)를 통해서만 호출돼야 하고, 그 소켓은 이미 이
    // shard의 event loop에 adopt_socket()으로 재바인딩된 상태여야 한다
    // -- 다른 loop에 바인딩된 소켓을 스레드 경계 넘어 직접 호출하면 안 됨.
    void dispatch_accept(std::unique_ptr<net::ISocket> socket);

    net::IEventLoop& event_loop() { return *event_loop_; }
    const LocalMetrics& metrics() const { return metrics_; }
    std::size_t index() const { return index_; }

private:
    std::size_t index_;
    const Config& config_;

    std::unique_ptr<net::IEventLoop> event_loop_;
    std::thread thread_;

    BufferPool buffer_pool_;
    LocalMetrics metrics_;
    UpstreamManager upstream_manager_;
};
