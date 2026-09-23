#pragma once

#include <cstdint>
#include <memory>

#include "net/acceptor.hpp"
#include "net/resolver.hpp"
#include "net/socket.hpp"
#include "net/timer.hpp"
#include "net/types.hpp"

namespace net {

// IEventLoop::adopt_socket()을 거친 소켓이라는 걸 타입으로 증명하는
// 래퍼. 생성자가 private + IEventLoop만 friend라서, adopt_socket() 밖의
// 코드는 이 타입의 인스턴스를 만들 방법이 없다 -- "재바인딩 안 된
// 소켓을 shard에 넘긴다"는 실수를 컴파일 타임에 원천 차단하기 위함
// (doc/plan.md의 "accept된 소켓의 event loop 재바인딩" 절 참고).
class AdoptedSocket {
public:
    std::unique_ptr<ISocket> release() { return std::move(socket_); }

private:
    friend class IEventLoop;
    explicit AdoptedSocket(std::unique_ptr<ISocket> socket) : socket_(std::move(socket)) {}

    std::unique_ptr<ISocket> socket_;
};

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
    virtual void post(VoidCallback task) = 0;

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
    // handle을 release한 뒤 대상 loop에 바인딩된 상태로 재구성.
    //
    // non-virtual: 반환값을 항상 AdoptedSocket으로 감싸는 걸 여기서
    // 강제한다 (Template Method 패턴) -- 구현체(BoostEventLoop 등)는
    // do_adopt_socket()만 오버라이드하면 되고, 그 결과가 AdoptedSocket
    // 없이 그냥 unique_ptr<ISocket>으로 새어나갈 방법이 없다. 도메인
    // 계층(Listener)은 이 메커니즘을 전혀 몰라도 되게 한다. 전체 경위는
    // doc/plan.md 참고.
    AdoptedSocket adopt_socket(std::unique_ptr<ISocket> foreign_socket) {
        return AdoptedSocket(do_adopt_socket(std::move(foreign_socket)));
    }

private:
    // private virtual이어도 하위 클래스(BoostEventLoop 등)는 문제없이
    // 오버라이드할 수 있다 -- C++에서 접근 지정자는 "누가 호출할 수
    // 있는가"만 통제하지 "누가 오버라이드할 수 있는가"는 통제하지
    // 않는다 (Herb Sutter의 NVI/"Virtuality" 관용구). 여기선 이
    // 메서드를 호출하는 코드가 adopt_socket() 하나뿐이라, protected로
    // 열어줄 이유가 없다 -- private으로 좁혀서 하위 클래스가 실수로
    // 이걸 직접 호출해 AdoptedSocket 래핑을 우회하는 경로 자체를
    // 없앤다.
    virtual std::unique_ptr<ISocket> do_adopt_socket(std::unique_ptr<ISocket> foreign_socket) = 0;
};

}  // namespace net
