#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "net/event_loop.hpp"
#include "net/resolver.hpp"
#include "net/socket.hpp"
#include "net/timer.hpp"

// Shard-local: 정확히 하나의 GatewayShard가 소유하며, 모든 메서드는 그
// shard의 event loop 스레드에서만 실행된다. 각 shard는 upstream 연결을
// 독립적으로 resolve하고 풀링한다 -- shard 간 공유 없음 (shared-nothing,
// doc/per-core-sharded-architecture.md section 10).
class UpstreamManager {
public:
    UpstreamManager(net::IEventLoop& event_loop, const Config& config);

    // 설정된 모든 endpoint에 대해 blocking DNS resolve를 1회 수행한 뒤,
    // 주기적 refresh timer를 예약한다. event_loop.run() 전에 한 번만 호출.
    void start();

    // Shard-local round-robin endpoint 선택 -- 이 shard의 스레드만
    // 호출하므로 평범한 std::size_t, atomic 불필요 (문서 section 17).
    std::size_t select_endpoint();

    // 이 endpoint에 대해 연결된 소켓을 콜백으로 전달한다: 풀에 있던
    // idle connection이면(이 호출이 반환되기 전에 콜백이 동기적으로
    // 실행됨) 그걸 쓰고, 아니면 새로 connect한 뒤(connect 완료 시점에
    // 비동기로 콜백 실행) 그걸 쓴다. 호출부는 두 경로를 동일하게
    // 처리해야 한다. 실패 시 콜백에 전달되는 소켓 포인터는 null.
    void acquire_connection(std::size_t index, net::SocketCallback callback);

    // acquire_connection()과 동일하지만 pool을 절대 보지 않고 항상 새로
    // connect한다. HttpSession이 keep-alive 재사용 커넥션의 첫 write가
    // 실패했을 때("풀의 idle 커넥션이 조용히 끊겨 있었다") 1회 재시도
    // 경로로 쓴다 -- 그 재시도가 또 죽어있는 pool 커넥션을 집으면
    // 의미가 없으므로 반드시 fresh connect여야 한다.
    void acquire_fresh_connection(std::size_t index, net::SocketCallback callback);

    // 아직 정상인 소켓을 idle pool에 반납한다 (상한:
    // connection_pool_max_idle_per_endpoint). 이 endpoint의 풀이 이미
    // 가득 찼으면 그냥 소켓을 닫고 버린다.
    void release_connection(std::size_t index, std::unique_ptr<net::ISocket> socket);

private:
    struct Endpoint {
        std::string host;
        uint16_t port;
        net::Endpoint resolved;
    };

    void resolve_all();
    void schedule_refresh();
    void connect_fresh(std::size_t index, net::SocketCallback callback);

    net::IEventLoop& event_loop_;
    const Config& config_;

    std::unique_ptr<net::IResolver> resolver_;
    std::unique_ptr<net::ITimer> dns_refresh_timer_;

    std::vector<Endpoint> endpoints_;
    std::size_t next_endpoint_ = 0;

    std::vector<std::vector<std::unique_ptr<net::ISocket>>> idle_pools_;
};
