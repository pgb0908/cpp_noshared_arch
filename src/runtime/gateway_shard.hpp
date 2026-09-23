#pragma once

#include <cstddef>
#include <memory>
#include <thread>

#include "config/config.hpp"
#include "filter/filter_chain.hpp"
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

    // Listener가 accept한 소켓을 이 shard로 안전하게 넘기는 유일한
    // 진입점. 호출 스레드는 무관 -- 내부적으로 이 shard의 event loop에
    // post한 뒤 adopt_socket()으로 재바인딩하고 나서야 세션을 시작한다.
    // 호출부(Listener)는 post/adopt_socket이라는 개념을 몰라도 된다.
    void accept_from(std::unique_ptr<net::ISocket> foreign_socket);

    const LocalMetrics& metrics() const { return metrics_; }
    std::size_t index() const { return index_; }

private:
    // accept_from()이 재바인딩까지 끝낸 뒤에만 호출한다. net::AdoptedSocket
    // 타입 자체가 "adopt_socket()을 거치지 않은 소켓은 여기 못 들어온다"는
    // 걸 컴파일 타임에 보장한다 -- net/event_loop.hpp 참고.
    void dispatch_accept(net::AdoptedSocket socket);

    std::size_t index_;
    const Config& config_;

    std::unique_ptr<net::IEventLoop> event_loop_;
    std::thread thread_;

    BufferPool buffer_pool_;
    LocalMetrics metrics_;
    UpstreamManager upstream_manager_;
    FilterChain filter_chain_;
};
