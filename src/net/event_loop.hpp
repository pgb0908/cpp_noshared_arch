#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "net/acceptor.hpp"
#include "net/resolver.hpp"
#include "net/socket.hpp"
#include "net/timer.hpp"

namespace net {

// event loop 하나 = 실행 컨텍스트 하나(예: boost::asio::io_context 하나)이며
// 정확히 스레드 하나에서 돈다. per-core-shard 아키텍처의
// "shard당 io_context 1개" 규칙을 그대로 따름 -- 각 GatewayShard는
// IEventLoop를 정확히 하나만 소유한다. 여기에 바인딩되는 모든 I/O 객체의
// factory 역할도 겸해서, 호출부는 구체적인 라이브러리 타입을 직접 다루지
// 않는다.
class IEventLoop {
public:
    virtual ~IEventLoop() = default;

    virtual void run() = 0;   // stop()까지 호출한 스레드를 blocking
    virtual void stop() = 0;
    virtual void post(std::function<void()> task) = 0;

    // 이 loop의 run()을 현재 실행 중인 스레드에서 호출됐으면 true.
    // run()이 아직 한 번도 호출 안 됐으면 항상 false. shard가 소유한
    // 코드가 실제로 그 shard의 스레드에서 실행되는지 assert()로
    // 검증하기 위한 용도 -- GatewayShard::dispatch_accept()와
    // UpstreamManager가 per-core-shard 아키텍처상 반드시 한 스레드에
    // 머물러야 하는 모든 진입점에서 이걸 assert한다.
    virtual bool is_current_thread() const noexcept = 0;

    virtual std::unique_ptr<ISocket> create_socket() = 0;
    virtual std::unique_ptr<IAcceptor> create_acceptor(uint16_t port) = 0;
    virtual std::unique_ptr<IResolver> create_resolver() = 0;
    virtual std::unique_ptr<ITimer> create_timer() = 0;

    // 다른 event loop에서 만들어진 소켓을 이 loop에 재바인딩해서, 이후
    // 모든 async 작업이 이 loop의 스레드에서 돌도록 만든다.
    //
    // 이 메서드가 존재하는 이유는 이 프로젝트에서 실제로 발견된
    // 버그 때문이다: IAcceptor::async_accept()로 accept된 소켓은
    // ACCEPTOR의 event loop(=Listener의 loop)에 바인딩되지, 그 소켓을
    // 넘겨받는 shard에 바인딩되지 않는다. 이 재바인딩이 없으면 모든
    // Session의 실제 read/write 완료 콜백이 계속 Listener 스레드에서
    // 실행되어 -- 소유 shard의 스레드가 아니라 -- per-core-shard
    // 설계 전체를 조용히 무력화시킨다. 해결책: accept된 소켓의 native
    // handle을 release한 뒤 대상 loop에 바인딩된 상태로 재구성 --
    // 이걸 여기서 처리해서 도메인 계층(Listener)은 이 메커니즘을 전혀
    // 몰라도 되게 한다. 전체 경위는 doc/plan.md 참고.
    virtual std::unique_ptr<ISocket> adopt_socket(std::unique_ptr<ISocket> foreign_socket) = 0;
};

}  // namespace net
