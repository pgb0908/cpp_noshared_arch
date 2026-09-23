#pragma once

#include <chrono>

#include "net/event_loop.hpp"
#include "net/types.hpp"

namespace net {

// 취소 가능한 비동기 작업 하나를 타임아웃과 경쟁시킨다: 작업이 타임아웃
// 안에 안 끝나면 cancel_op()으로 취소하고 타임아웃 에러로 완료 처리하며,
// 작업이 먼저 끝나면 타이머를 취소하고 그 결과로 완료 처리한다. 둘 중
// 어느 쪽이 이기든 on_done은 정확히 한 번만 불린다.
//
// UpstreamManager::connect_fresh()의 connect-vs-timeout 경쟁을 일반화한
// 것 -- "취소 가능한 두 비동기 작업 중 먼저 끝난 쪽이 이긴다"는 패턴이
// 재사용될 때마다(읽기 타임아웃, DNS 타임아웃 등) 타이머/done 플래그
// shared_ptr 관리를 처음부터 다시 짤 필요가 없게 한다.
//
// start_op: 실제 비동기 작업을 시작하는 함수. 완료되면 받은 콜백을
//     정확히 한 번 호출해야 한다 (start_op 자신은 여기서 동기적으로
//     한 번만 호출됨).
// cancel_op: 타임아웃이 이겼을 때, start_op가 걸어둔 작업을 취소하는
//     함수.
using StartRacingOp = MoveOnlyFunction<void(ErrorCallback on_op_done)>;

void race_with_timeout(IEventLoop& loop, std::chrono::seconds timeout, StartRacingOp start_op, VoidCallback cancel_op,
                       ErrorCallback on_done);

}  // namespace net
