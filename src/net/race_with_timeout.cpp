#include "net/race_with_timeout.hpp"

#include <memory>

namespace net {

void race_with_timeout(IEventLoop& loop, std::chrono::seconds timeout, StartRacingOp start_op, VoidCallback cancel_op,
                       ErrorCallback on_done) {
    // timer/on_done/cancel_op/done 넷 다 "타이머 콜백과 작업 완료 콜백
    // 양쪽에서 접근 가능해야 하는" 진짜 공유 상태다 -- 어느 쪽이 이길지
    // 실행 전엔 알 수 없고, 이긴 쪽이 진 쪽의 리소스를 정리해줘야 한다.
    // MoveOnlyFunction은 "한 곳으로만 옮기면 끝나는" 상황엔 boxing 없이
    // 쓸 수 있지만, 이렇게 두 콜백이 같은 상태를 공유해야 하는 경우엔
    // shared_ptr이 원래 맞는 도구다.
    auto timer = std::make_shared<std::unique_ptr<ITimer>>(loop.create_timer());
    auto shared_on_done = std::make_shared<ErrorCallback>(std::move(on_done));
    auto shared_cancel = std::make_shared<VoidCallback>(std::move(cancel_op));
    auto done = std::make_shared<bool>(false);

    (*timer)->expires_after(timeout);
    (*timer)->async_wait([timer, shared_on_done, shared_cancel, done](const Error& err) {
        if (*done || !err.ok()) {
            return;  // 이미 처리됐거나(작업이 먼저 끝남), timer가 cancel된 것
        }
        *done = true;
        (*shared_cancel)();
        (*shared_on_done)(Error{1, "operation timed out"});
    });

    start_op([timer, shared_on_done, done](const Error& err) {
        if (*done) {
            return;  // 타임아웃이 먼저 발생해서 이미 처리됨
        }
        *done = true;
        (*timer)->cancel();
        (*shared_on_done)(err);
    });
}

}  // namespace net
