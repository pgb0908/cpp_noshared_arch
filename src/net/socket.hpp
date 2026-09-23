#pragma once

#include <memory>

#include "net/types.hpp"

namespace net {

class ISocket {
public:
    virtual ~ISocket() = default;

    virtual void async_connect(const Endpoint& endpoint, ErrorCallback cb) = 0;
    virtual void async_read_some(MutableBuffer buffer, IoCallback cb) = 0;
    // 버퍼 전체를 다 쓴다 (boost::asio::async_write와 동일한 의미,
    // 단발성 async_write_some이 아님).
    virtual void async_write(ConstBuffer buffer, IoCallback cb) = 0;

    virtual void shutdown() = 0;  // 양방향 모두
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual void cancel() = 0;    // pending 중인 async 작업 취소

    // 지금 이 메서드를 부르는 스레드가 이 소켓의 소유 event loop
    // 스레드와 같은지. 구현체는 async_connect/async_read_some/
    // async_write/shutdown/close/cancel 진입 시 이걸로 자체 검증해야
    // 한다 -- 호출하는 쪽마다 assert_on_owning_thread()를 따로 만들어
    // 부를 필요 없이, 소켓 자신이 잘못된 스레드에서 쓰이는 걸 즉시
    // 잡아낸다. (release_native_handle()은 예외 -- adopt_socket()의
    // 재바인딩 절차 자체가 의도적으로 원래 소유 스레드가 아닌 대상
    // shard 스레드에서 옛 소켓을 이걸로 정리하기 때문에, 여기 검증을
    // 걸면 그 절차 자체가 깨진다.)
    virtual bool is_owned_by_current_thread() const = 0;

    // 플랫폼 native 소켓 handle(POSIX fd)을 release해서 반환하고, 이
    // 객체의 소유권을 포기한다. IEventLoop::adopt_socket()이 한 event
    // loop에서 accept된 소켓을 다른 loop로 옮길 때만 사용 -- 자세한 건
    // net/event_loop.hpp 참고. 이후 이 객체의 다른 메서드를 호출하면
    // undefined behavior.
    virtual int release_native_handle() = 0;
};

using SocketCallback = MoveOnlyFunction<void(const Error&, std::unique_ptr<ISocket>)>;

}  // namespace net
