# 세션 요약 (2026-09-22 ~ 2026-09-23)

이 문서는 이번 세션에서 진행한 작업 전체를 훑는 요약이다. 각 항목의 상세 설계/경위는 `doc/plan.md`(아키텍처 결정 로그)와 `doc/benchmark-report.md`(성능 실측)에 이미 기록돼 있고, 여기서는 무엇을 왜 했는지와 그 결과만 간결하게 정리한다.

## 커밋 목록 (시간순)

| 커밋 | 내용 |
|---|---|
| `5afd592` | 필터 체인 도입 (헤더+바디 대칭 훅, Policy/Filter 아키텍처) |
| `7204281` | `HttpSession`을 `.hpp`(선언)/`.cpp`(구현)로 분리 |
| `1064bbe` | 벤치마크 도구 + baseline 문서 추가 |
| `9c438e7` | downstream/upstream keep-alive 도입 + stale 커넥션 retry-once |
| `9fd469b` | 로그로 인한 부하 방지 |
| `e72c780` | keep-alive 재측정 중 발견한 TCP_NODELAY 누락 수정 |
| `9436a2f` | Phase 5: P-core/E-core 감지해 shard를 성능 높은 코어부터 배정 |
| `71d5ece` | accept 소켓 재바인딩 불변식을 타입/소유권 검증으로 강제 |
| *(미커밋)* | `UpstreamManager::connect_fresh`의 timer race를 `net::race_with_timeout()`으로 추출 |

---

## 1. Policy/Filter 체인 (`5afd592`)

- `IFilter` 인터페이스로 헤더(`on_request`/`on_response`)와 바디(`on_request_data`/`on_response_data`) 훅을 대칭으로 통합 (Envoy/Proxygen 스타일). 처음엔 `IRequestFilter`/`IResponseFilter`로 분리했다가, 사용자 요청으로 하나로 합침.
- 필터 인스턴스는 shard당 싱글턴 — 세션별 인스턴스(Envoy 방식)와 비교 검토했으나, 필터 간 데이터 공유(인증→rate limit)까지 고려해 **싱글턴 + `FilterContext`** 조합으로 결정 (grill 세션).
- `FilterChain`: 요청은 등록 순, 응답은 등록 역순(onion 모델, Envoy/Netty와 동일).
- 바디 필터: 스트리밍 통과 + 옵트인 버퍼링(`kStopIterationAndBuffer`). watermark는 Envoy의 dual-threshold + read-pause가 아니라 **단일 hard cap + 즉시 거부**(413/502) — "바디 전체가 필요한 필터 + read 일시정지"를 같이 쓰면 데드락이 된다는 걸 실제로 유도해서 확인 후 결정. Envoy의 `envoy.filters.http.buffer`도 같은 이유로 하드 캡을 쓴다는 것도 확인.
- `BodyFilterGate`로 필터 적용/watermark 판단 정책을 `HttpSession`에서 분리 — 이 분리 과정에서 **실제 버그 2건 발견**: 필터가 `end_stream`에서도 계속 버퍼링을 요청하면 원래 `assert()`로 프로세스가 죽었던 것(방어적 강제 종결로 수정), 그리고 그 방어 로직이 watermark 체크보다 먼저 실행돼서 단일 청크짜리 요청이 413을 우회하던 것(순서 수정).
- GTest 55개 → 63개, 실제 upstream e2e(GET/POST/200KB body/413 강제 재현)로 검증.

## 2. `HttpSession` `.hpp`/`.cpp` 분리 (`7204281`)

`UpstreamManager`와 같은 패턴으로 기계적 분리. 462줄 → 127줄(헤더), 나머지는 `.cpp`로. 설계 변경 없음.

## 3. keep-alive 도입 + TCP_NODELAY 버그 (`9c438e7`, `9fd469b`, `e72c780`)

*(다른 세션에서 진행, 이 세션은 그 결과를 이어받아 검증/실측만 수행)*

- Phase 6 baseline 프로파일링에서 accept/connect(요청당 1회 커널 비용)가 최대 병목으로 드러나 착수.
- downstream/upstream 두 hop을 독립적으로 판단하는 표준 프록시 패턴, `UpstreamManager`의 idle pool을 HTTP relay에서 실제 사용하도록 연결.
- **회귀 발견 → 원인 추적**: keep-alive 도입 직후 p50/p75/p90이 41ms 부근에 고정적으로 뭉치는 회귀 발생 — Nagle 알고리즘 + 수신측 delayed ACK의 전형적 시그니처. 원인은 게이트웨이가 아니라 **테스트 업스트림(Python `http.server`)이 TCP_NODELAY를 안 켜고 있던 것** — 수정 후 25,704 req/s, p90 8.65ms로 대폭 개선.

## 4. 성능 실측 (`doc/benchmark-report.md`)

- 이 세션에서 이어서 진행한 부분: 현재 코드 기준 재측정, 그리고 **"단순 프록시인데 느리다"는 인상을 검증**.
- 실측 환경(이 세션이 명령을 실행하는 샌드박스)의 신뢰도 문제를 여러 겹으로 발견: `perf_event_paranoid`/`kptr_restrict` 권한, `nproc`가 같은 세션 안에서 8→20으로 흔들리는 현상(cgroup 쿼터가 동적으로 걸렸다 풀리는 것으로 추정) — **베어메탈인데도 이런 흔들림이 있어서, 이 세션이 잰 절대 수치는 참고용일 뿐 확정적 결론의 근거로 쓸 수 없다**고 정리.
- **테스트 업스트림(Python) 자체가 병목이었다는 것도 실측으로 증명**: 게이트웨이 없이 Python 업스트림에 직접 wrk를 쐈을 때 p99가 이미 497ms — 게이트웨이가 개입하지 않는 구간에서도 이 정도 지연이 나온다는 뜻.
- **실제 nginx로 백엔드를 바꾼 뒤 재측정**: 160,448 req/s(nginx 직접) vs 149,817 req/s(게이트웨이 경유) — 처리량 -6.6%, p99는 2.11ms→8.12ms. 이게 게이트웨이의 진짜 오버헤드에 가까운 수치. Envoy/Proxygen도 벤치마크 백엔드로 컴파일된 경량 서버(또는 nginx)를 쓰는 게 표준 관례라는 것도 확인.
- 종료 시 메트릭에서 shard 간 요청 처리량이 3배 넘게 차이나는 불균형도 발견 — 원인은 미확정(당시엔 "가상화 노이즈"로 추정했으나, 이후 베어메탈로 정정되면서 재검토 필요 상태로 남음).

## 5. Phase 5 — CPU Affinity 고도화 (`9436a2f`)

- 개발 머신(Intel Core Ultra 7 265K)이 **P-core 8개(cpu0-7) + E-core 12개(cpu8-19)** 하이브리드 CPU라는 걸 발견 (`lscpu -e`의 `MAXMHZ` 컬럼으로 확인). NUMA는 단일 노드라 해당 없음 — doc이 원래 상정한 멀티소켓 시나리오와 다름.
- `src/util/cpu_topology.{hpp,cpp}`: `hwloc`의 `cpukinds` API로 코어 종류 감지, 성능 높은 순으로 정렬된 `ShardCpuPlan` 생성. 동종 CPU에서는 기존 0..N-1 매핑으로 자동 폴백.
- `shard_count`가 P-core 개수를 넘으면 E-core로 자연스럽게 이어서 배정 + 경고 로그(서비스 중단 안 함).
- **솔직한 재평가**: 이 머신은 P-core가 이미 cpu0-7로 순서대로 정렬돼 있어서, 새 코드가 이 하드웨어에서 실제로 만들어내는 배정 결과는 예전 순진한 매핑과 **완전히 동일함**을 사용자가 지적 — 실질 효과는 (1) 초과 시 경고 로그, (2) P/E가 뒤섞여 번호매겨진 다른 머신에서의 이론적 정확성(이 세션에서 검증 불가)뿐이라고 정정.
- 실측: `shard_count=12`로 실행 → shard 8~11 경고 확인 + `ps -L -o tid,psr`로 실제 커널이 보고하는 실행 코어가 의도한 배정과 정확히 일치함을 확인.
- GTest 4개 추가(하드웨어 비의존적 불변조건만 검증).

## 6. 아키텍처 Deepening (`/improve-codebase-architecture`)

`src/` 전체를 얕은 모듈(shallow module) 관점에서 정찰(Explore 서브에이전트) → 후보 6개 도출 → 2개 완료. 전체 목록은 `report/architecture-deepening-candidates.md` 참고.

### 완료: accept 소켓 재바인딩 불변식 강제 (`71d5ece`)

- **재발견한 문제**: `GatewayShard::dispatch_accept()`의 `assert(is_current_thread())`가 원래 버그(소켓이 재바인딩 안 된 채 Listener 스레드에 남는 것)를 실제로는 못 잡고 있었음 — `dispatch_accept()`가 항상 `post(lambda)` 안에서만 불려서 이 assert는 늘 통과.
- **적용한 해결책**:
  1. `net::AdoptedSocket` — `adopt_socket()`을 거친 소켓만 만들 수 있는 타입(NVI 패턴). `GatewayShard::dispatch_accept(AdoptedSocket)`로 시그니처를 바꿔서 adopt 안 된 소켓을 넘기는 코드는 **컴파일이 안 되게** 함.
  2. `ISocket::is_owned_by_current_thread()` — 소켓 자신이 소유 `IEventLoop`를 알고 모든 I/O 진입점에서 자체 검증하는 일반적 안전망 (컴파일 타임 방어와 별개로, 예상 못 한 다른 오용도 런타임에 잡음).
  3. `do_adopt_socket()`은 `protected`가 아니라 `private`로 — C++에서 접근 지정자는 호출 가능 여부만 통제하고 오버라이드 가능 여부는 통제하지 않는다는 점(Herb Sutter, NVI)을 활용해 더 좁게 제한.
- 부수 효과: `Listener`가 `post`/`adopt_socket`을 몰라도 되게 `GatewayShard::accept_from()`으로 이관, 안 쓰는 `event_loop()` 접근자 제거.
- 검증: GTest 67개 + 실제 e2e(GET/동시 20개 요청/POST).

### 완료: `UpstreamManager::connect_fresh`의 boxing 추출 (미커밋)

- connect-vs-timeout 경쟁 로직(소켓/타이머/콜백/`done` 플래그를 `shared_ptr<unique_ptr<T>>` 4개로 관리하던 부분)이 이름 없이 함수 안에 풀어헤쳐 있던 것을 `net::race_with_timeout()`(`src/net/race_with_timeout.{hpp,cpp}`)으로 추출.
- `shared_ptr` 자체는 원래도 정당한 선택이었음(두 콜백이 진짜 상태를 공유해야 함) — 다만 그 관리를 재사용 가능한 모듈로 숨김. `connect_fresh()`엔 소켓 공유용 `shared_ptr` 1개만 남음(4개→1개).
- 가독성 후속 수정: `race_with_timeout()` 호출부에 람다 3개를 인라인하면 인자 목록 안에서 들여쓰기가 겹쳐 보여 추적이 어렵다는 지적 → `start_connecting`/`cancel_connecting`/`deliver_result`라는 이름 있는 변수로 먼저 뽑아서 위에서 아래로 읽히게 수정.
- 검증: `tests/race_with_timeout_test.cpp` 4개 신규(이 race 로직을 `UpstreamManager` 없이 독립 테스트) + 기존 `upstream_manager_test.cpp` 회귀 없음 + 블랙홀 IP로 connect timeout이 설정값(2초)에 정확히 맞춰 발생하는 것 재확인.
- GTest 67개 → 71개.

### 남은 후보 (미착수)

1. Metrics 합산 로직이 `LocalMetrics`/`MetricsAggregator`/`GatewayRuntime` 세 곳에 중복
2. `Config::from_file`이 파싱+검증+`std::exit()`을 한 함수에서 처리 (에러 경로 테스트 불가)
3. `UpstreamManager`↔`Session`의 "반납 전 cancel() 필수" 계약이 API에 안 드러남
4. `session/session.hpp`(legacy raw TCP relay)가 안 쓰이는데 여전히 테스트됨 — 삭제 후보

---

## 이번 세션에서 얻은 교훈 (일반화 가능한 것)

- **assert가 있다고 안전한 게 아니다** — `dispatch_accept()`의 assert처럼, "무엇을 검증하는지"와 "실제로 무엇이 참일 때만 통과하는지"가 다를 수 있다. 타입 시스템으로 강제할 수 있으면 그게 항상 더 강한 보장.
- **`shared_ptr<unique_ptr<T>>` boxing 자체는 나쁜 게 아니다** — 두 콜백이 진짜 상태를 공유해야 하는 상황(취소 가능한 경쟁)에서는 정당한 도구. 문제는 boxing 여부가 아니라 그 패턴이 이름 붙어서 재사용 가능한지.
- **벤치마크 결과는 측정 환경 자체를 의심하지 않으면 오독하기 쉽다** — 같은 세션 안에서 `nproc`가 8→20으로 바뀐 것, 테스트 업스트림이 병목이었던 것 둘 다 "게이트웨이가 이상하다"고 성급히 결론 내릴 뻔한 지점이었음.
- **"완료"라고 부른 작업도 실제 효과를 재확인해야 한다** — Phase 5가 이 하드웨어에서 예전 코드와 동일한 결과를 낸다는 걸 뒤늦게 인정한 것처럼, 실측 없이 "논리적으로 맞다"만으로 효과를 과대평가하지 않아야 함.
