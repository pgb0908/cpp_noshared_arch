# 아키텍처 Deepening 후보 (`/improve-codebase-architecture`, 2026-09-23)

`src/` 전체를 "얕은 모듈(shallow module) vs 깊은 모듈(deep module)" 관점에서 정찰한 결과. 각 항목은 deletion test(이 모듈을 지우면 복잡도가 사라지는지, 아니면 호출부로 다시 나타나는지)를 적용해서 뽑음.

용어는 `/improve-codebase-architecture` 스킬의 정의를 따른다: **모듈**(인터페이스+구현을 가진 것), **인터페이스**(호출자가 알아야 하는 전부: 타입/불변식/에러모드/순서/설정), **깊이**(작은 인터페이스 뒤에 많은 동작 = leverage), **seam**(동작을 갈아끼울 수 있는 지점), **locality**(변경/버그/지식이 한 곳에 모이는 정도).

---

## 1. Metrics 합산 로직이 세 곳에 중복 — `LocalMetrics`가 얕은 모듈

- **파일**: `util/local_metrics.hpp`(정의), `runtime/metrics_aggregator.hpp:38-63`(주기적 합산), `runtime/gateway_runtime.cpp::print_metrics_summary`(종료 시 합산)
- **문제**: 8개 카운터를 세 곳에서 각자 나열해서 더함. 필드 하나 추가하면 세 파일을 동시에 고쳐야 함 — `LocalMetrics`라는 모듈이 "shard들의 지표를 합산한다"는 leverage를 전혀 안 주고, 그 복잡도가 호출부로 그대로 새어나감(shallow).
- **해결 방향**: `LocalMetrics`(또는 별도 `MetricsSnapshot`)에 "여러 개를 합산해서 스냅샷을 만드는" 연산 자체를 소유시켜서, `MetricsAggregator`와 `GatewayRuntime`은 그 결과만 갖다 쓰게.
- **효과**: 필드 추가가 한 곳으로 국소화(locality). 지금 세 곳이 진짜 일치하는지 확인할 테스트가 없는데, 합산 로직 자체를 유닛테스트 가능한 하나의 인터페이스로 만들 수 있음.

---

## 2. "accept된 소켓의 이벤트루프 소유권 이전" 불변식이 인터페이스가 아니라 주석에만 존재 — ✅ 완료

- **파일**: `net/event_loop.hpp`, `net/boost/event_loop.{hpp,cpp}`, `net/boost/socket.{hpp,cpp}`, `net/boost/acceptor.{hpp,cpp}`, `runtime/listener.hpp`, `runtime/gateway_shard.{hpp,cpp}`
- **문제였던 것**: `GatewayShard::dispatch_accept()`의 `assert(is_current_thread())`가 실제로는 원래 버그(재바인딩 없이 소켓이 Listener 스레드에 남는 것)를 못 잡음 — `dispatch_accept()`는 항상 `post(lambda)` 안에서만 불려서 이 assert는 `adopt_socket()` 호출 여부와 무관하게 항상 통과했음.
- **적용한 해결책**:
  1. `net::AdoptedSocket` — `adopt_socket()`을 거친 소켓만 만들 수 있는 타입(생성자 private + `IEventLoop`만 friend). `adopt_socket()`을 non-virtual public(NVI 패턴)으로 바꿔 항상 이 타입으로 감싸 반환하도록 강제, 구현체는 `private virtual do_adopt_socket()`만 오버라이드. `dispatch_accept(AdoptedSocket)`로 시그니처를 바꿔서 adopt 안 된 소켓을 넘기는 코드는 컴파일이 안 되게 함.
  2. `ISocket::is_owned_by_current_thread()` — `BoostSocket`이 자신을 만든 `IEventLoop`를 들고 있다가 모든 I/O 진입점에서 자체 검증 (`release_native_handle()`만 예외 — adopt 절차 자체가 의도적으로 다른 스레드에서 호출).
  3. 부수 효과로 `Listener::do_accept()`가 `post`/`adopt_socket`을 몰라도 되게 `GatewayShard::accept_from()`으로 시퀀스 이관, 안 쓰는 `event_loop()` 접근자 제거.
- **커밋**: `71d5ece` — GTest 67개 + 실제 e2e(GET/동시 20개 요청/POST) 검증 완료. `doc/plan.md`에 상세 기록.

---

## 3. `UpstreamManager::connect_fresh`의 boxing이 프로젝트 원칙(MoveOnlyFunction)과 모순 — ✅ 완료

- **파일**: `upstream/upstream_manager.cpp:81-120` → `net/race_with_timeout.{hpp,cpp}`
- **문제였던 것**: "boxing 없애자"가 이 프로젝트의 명시적 설계 원칙인데, timer-vs-connect race를 다루는 이 함수만 `shared_ptr<unique_ptr<ISocket>>`, `shared_ptr<unique_ptr<ITimer>>`, `shared_ptr<SocketCallback>`, `shared_ptr<bool>` 4개를 만들어 두 콜백에 캡처시킴. 이 취소/경쟁 패턴이 재사용 가능한 하나의 개념으로 뽑혀 있지 않고 함수 안에 얕게 풀어헤쳐 있었음.
- **적용한 해결책**: `net::race_with_timeout(loop, timeout, start_op, cancel_op, on_done)`으로 "취소 가능한 비동기 작업 vs 타임아웃" 패턴 자체를 추출. `shared_ptr` 자체가 틀린 선택은 아니었음(두 콜백이 진짜 같은 상태를 공유해야 하는 상황)을 재확인했고, 다만 그 관리(타이머+`done` 플래그)를 재사용 가능한 모듈 안으로 숨김. `connect_fresh()`에 남는 `shared_ptr`은 소켓 하나뿐(4개→1개, 소켓은 start_op/cancel_op 둘 다 접근해야 해서 공유가 불가피).
- **효과**: `tests/race_with_timeout_test.cpp`로 이 race 로직 자체를 `UpstreamManager` 없이 독립 테스트 가능해짐 (동시 완료, 뒤늦은 완료 무시 등 4개 케이스). 기존 `upstream_manager_test.cpp`의 타이밍 경쟁 테스트는 회귀 없이 그대로 통과, 블랙홀 IP 실측으로 connect timeout이 설정값에 정확히 맞춰 동작함도 재확인. `doc/plan.md`의 Phase 4(Upstream Connect Timeout) 절에 상세 기록.

---

## 4. `Config::from_file`이 파싱+기본값+검증+에러출력+프로세스종료를 한 함수에서 처리

- **파일**: `config/config.hpp:33-99`
- **문제**: `std::exit(1)`을 라이브러리 코드 한가운데서 직접 호출해서, "잘못된 설정이면 에러"라는 걸 유닛테스트로 검증하려면 테스트 프로세스 자체가 죽음(`doc/plan.md`에 이미 알려진 갭으로 기록됨). 8개 필드가 거의 동일한 3줄 boilerplate로 반복.
- **해결 방향**: 파싱+검증은 결과 타입(성공/실패)을 돌려주는 순수 함수로, `exit`는 `main.cpp` 경계로 이동.
- **효과**: 에러 경로까지 정상적인 유닛테스트로 커버 가능해짐.

---

## 5. `UpstreamManager`↔`Session`/`HttpSession`의 "반납 전 cancel() 필수" 불변식이 API에 안 드러남

- **파일**: `upstream_manager.hpp`(`release_connection` 시그니처), `session/session.hpp:135-155`(계약이 호출부 주석에만 존재)
- **문제**: "pool에 반납하기 전 반드시 `cancel()`을 먼저 불러야 한다"는 지식을 호출자가 알아야 하는데, `release_connection()`은 아무 소켓이나 받아줘서 이 계약을 스스로 강제 못 함 — 잘못 반납하면 재사용된 소켓에 이전 세션과 새 세션의 read가 동시에 걸리는 크로스토크 버그로 이어짐(과거 실제로 겪음).
- **해결 방향**: `release_connection()`이 "이미 정리된 소켓"만 타입으로 받도록 하거나, 그 정리 자체를 내부에서 수행.
- **효과**: 이 버그 클래스를 인터페이스 레벨에서 원천 차단.

---

## 6. `session/session.hpp`(legacy raw TCP relay)가 안 쓰이는데 여전히 테스트됨

- **파일**: `session/session.hpp`, `tests/session_test.cpp`
- **문제**: `HttpSession`이 이미 대체했는데 파일명만 봐선 뭐가 실제로 쓰이는지 구분 안 됨 — deletion test 적용 시 "지워도 복잡도가 안 사라짐"(이미 아무도 안 씀)이라 순수 탐색 마찰.
- **해결 방향**: 삭제(또는 명확히 `legacy/`로 격리).
- **효과**: 리스크 거의 없는 정리, 코드베이스 탐색 비용만 줄어듦.
