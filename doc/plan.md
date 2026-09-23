# perCoreShard 구현 로드맵

`doc/per-core-sharded-architecture.md`의 Phase 1~7 전략과, 이번 대화에서 나온 실무적 이슈(DNS 재조회, connect timeout 등)를 결합해 앞으로의 작업을 정리한다. 각 Phase는 이전 Phase가 끝나야만 시작 가능한 건 아니고, 문서 자체도 "단계별 리팩토링 전략"이라고 명시했듯 **점진적** 진행을 전제로 한다.

### 한눈에 보는 현재 상태 (2026-09-22 기준)

| 항목 | 상태 |
|---|---|
| Phase 1 (Event Loop 분리) | ✅ 완료 |
| Phase 2 (Connection Ownership) | ✅ 완료 |
| Phase 3 (Upstream Pool Sharding) | ✅ 완료 |
| Phase 4 (Timer/Buffer/Metrics Local화) | ✅ 완료 |
| net/ 추상화 계층 (Boost.Asio 디커플링) | ✅ 완료 |
| MoveOnlyFunction + 단위 테스트 스위트 | ✅ 완료 |
| HTTP 파싱/직렬화 엔진 (llhttp + 자체 시리얼라이저) | ✅ 완료 |
| HTTP 엔진을 실제 relay에 연결 (`HttpSession`) | ✅ 완료 — `Session`(raw relay)을 완전히 대체 |
| **필터 체인 (Policy/Filter)** | ✅ **완료** — 헤더 대칭 인터페이스(`IFilter`) + onion 순서, 바디 필터(스트리밍/버퍼링, watermark) 추가, upstream 연결 전에 거부 가능함을 실측 확인 |
| **Phase 6 baseline 프로파일링** | ✅ **완료** — wrk + perf + FlameGraph, kptr_restrict 해제 후 재측정까지 (`doc/benchmark-report.md`) |
| **keep-alive (downstream + upstream)** | ✅ **완료** — hop 독립 판단, upstream pool 실사용, stale pooled connection retry-once. 아래 상세 |
| GTest 전체 | 63개, 전부 통과 |
| Phase 5 (CPU Affinity 고도화) | ✅ 완료 -- hwloc으로 P-core/E-core 감지, 실측 하드웨어에서 실제 pinning 결과(PSR)까지 검증 |
| Phase 6 (프로파일링 기반 Hot Path 최적화, baseline 이후 개선) | 미착수 |
| Phase 7 (SO_REUSEPORT) | 미착수 |
| 라우팅/정책/RuntimeConfigManager/TLS | 미착수 |

`src/session/http_session.hpp`(`HttpSession`)가 이제 실제 요청 경로다. `Session`(`src/session/session.hpp`, raw byte relay)은 삭제하지 않고 참고용으로 남아있지만 더 이상 `GatewayShard::dispatch_accept()`에서 쓰이지 않는다.

---

## 현재 상태 (완료)

### Phase 1 — Event Loop 분리 ✅
`GatewayShard`마다 독립 event loop + pinned thread (`src/runtime/gateway_shard.hpp/.cpp`, `src/util/cpu_affinity.hpp`). 이후 인프라 리팩토링에서 `net::IEventLoop` 인터페이스 뒤로 옮겨짐 (아래 참고).

### Phase 2 — Connection Ownership ✅
`Listener`가 accept한 소켓을 round-robin으로 정확히 하나의 `GatewayShard`에 위임, 그 shard가 `Session` 생명주기 전체를 소유 (`src/runtime/listener.hpp`, `src/session/session.hpp`).

MVP 범위: 순수 TCP byte relay, 단일 고정 upstream, HTTP 파싱 없음. git 첫 커밋(`e4b0ab1`) 완료.

**알려진 갭 (당시 기준, 전부 이후 Phase에서 해소됨):**
- ~~upstream 1개 고정, 커넥션 재사용 없음~~ → Phase 3에서 해결 (다중 upstream + Connection Pool)
- ~~DNS resolve가 부팅 시 1회뿐~~ → Phase 3에서 해결 (주기적 재조회)
- ~~upstream connect에 타임아웃 없음~~ → Phase 4에서 해결
- ~~`BufferPool`이 실제 pool이 아니라 매번 new/delete~~ → Phase 4에서 해결
- Listener가 단일 스레드 → 여전히 Phase 7 대기 (SO_REUSEPORT)

---

## Phase 3 — Upstream Pool Sharding ✅

`src/upstream_manager.hpp/.cpp`로 구현 완료. `GatewayShard`마다 독립적인 `UpstreamManager`를 소유하며 (shard 간 공유 없음), 다음을 제공한다:

- **다중 upstream + shard-local round-robin**: `select_endpoint()`, 각 shard가 독립적인 `next_endpoint_` 카운터를 가짐
- **Connection Pool**: `acquire_pooled_connection()`/`release_connection()`, endpoint별 idle 소켓 목록, `connection_pool_max_idle_per_endpoint`로 상한
- **DNS 주기적 재조회**: `dns_refresh_timer_`(shard의 `io_context_`에 묶인 `steady_timer`)가 `dns_refresh_interval_seconds`마다 재조회
- **JSON 설정 파일** (`nlohmann-json3-dev` 도입): `--config path.json`, 예시는 `config.example.json` 참고. 기존 CLI 개별 플래그(`--listen-port` 등)는 JSON 필드로 대체됨

구현 중 발견/수정한 버그: `Session::close()`에서 healthy upstream 소켓을 풀에 반납하기 전에 `relay_upstream_to_downstream()`이 걸어둔 pending `async_read_some`을 취소(`upstream_->cancel()`)하지 않으면, 재사용된 소켓에 이전 Session과 새 Session의 read가 동시에 걸려서 응답이 엉뚱한(이미 닫힌) Session으로 가버리는 문제가 있었음. `cancel()`을 풀 반납 직전에 호출해 해결 (`src/session.hpp` 참고).

**여전히 남은 갭 (Phase 4로 이월):**
- 풀에 있는 idle connection이 상대방에 의해 조용히 끊긴 경우(peer FIN) 이를 미리 감지하는 health-check 없음 — 다음 사용 시점에 실패로 드러남, 재시도 로직도 없음
- DNS 최초 resolve 실패 시 재시도 없이 다음 주기까지 대기 (0.0.0.0:0으로 connect 시도하게 됨)

---

## 인프라 리팩토링 — net/ 추상화 계층 + Cross-Thread Accept 버그 수정 ✅

Phase 4 진행 전에 두 가지 구조적 작업을 먼저 했다.

### 1. Boost.Asio 결합도 낮추기

`src/net/`에 라이브러리 독립적인 인터페이스(`ISocket`, `IAcceptor`, `IResolver`, `ITimer`, `IEventLoop`)를 두고, `src/net/boost/`에만 Boost.Asio 구현체(`BoostSocket`, `BoostAcceptor`, `BoostResolver`, `BoostTimer`, `BoostEventLoop`)를 둠. `Session`/`UpstreamManager`/`Listener`/`GatewayShard.hpp`는 이제 `<boost/asio.hpp>`를 전혀 include하지 않고 `net/` 인터페이스만 참조한다. 유일한 예외는 `GatewayRuntime::run_until_signal()`의 SIGINT/SIGTERM 처리 — 1회성 control-plane 코드라 추상화 실익이 없어 의도적으로 Boost 직접 사용 유지 (주석으로 명시).

`GatewayShard.cpp`/`GatewayRuntime.cpp`만 `net/boost/factory.hpp`(`create_event_loop()`)를 통해 구체 구현체를 생성 — 이 두 곳이 유일한 "composition root".

### 2. 발견한 버그: Session의 I/O가 실제로는 Listener 스레드에서 실행되고 있었음

Boost.Asio에서 `acceptor.async_accept(handler)`로 받은 소켓은 **acceptor 자신의 io_context(=Listener의 io_context)에 바인딩된 채로 생성**된다. `asio::post`로 shard에 넘겨도 이 바인딩은 안 바뀌어서, `dispatch_accept()` 자체(Session 생성)는 shard 스레드에서 실행되지만 **Session의 실제 `async_read_some`/`async_write` 완료 콜백은 전부 Listener 스레드에서 실행**되고 있었다. Phase 2부터 지금까지 "connection의 I/O가 shard 스레드에서 처리된다"는 핵심 전제가 깨져 있었던 것 — thread-id 비교로 직접 확인함.

**수정**: `IEventLoop::adopt_socket()` 추가 — accept된 소켓의 native handle(fd)을 release해서 대상 shard의 event loop에 새로 바인딩된 소켓으로 재구성. `Listener::do_accept()`가 shard에 post한 뒤 `target_loop.adopt_socket(...)`을 호출하고 나서야 `dispatch_accept()`를 호출하도록 수정 (`src/runtime/listener.hpp`).

### 3. 회귀 방지: assert 기반 스레드 검증

`IEventLoop::is_current_thread()`를 추가해 "지금 이 스레드가 이 event loop를 실행 중인 스레드인가"를 확인할 수 있게 하고, 다음 지점에 `assert()`로 박아넣음:
- `GatewayShard::dispatch_accept()` — 호출 지점 자체 검증
- `UpstreamManager::select_endpoint/acquire_connection/release_connection` — shard-local 상태 접근 지점 검증
- **`Session`의 모든 I/O 콜백** (`connect_upstream`, `relay_downstream_to_upstream`, `relay_upstream_to_downstream`의 read/write 콜백) — 실제 버그가 드러나는 지점, `assert_on_owning_thread()` 헬퍼로 일괄 적용

실제로 `adopt_socket()`을 빼고 빌드해서 `Session::assert_on_owning_thread()`가 정확히 터지는 것까지 확인함. `CMakeLists.txt`의 기본 `CMAKE_BUILD_TYPE`을 `Debug`로 바꿔서 (기존 `RelWithDebInfo`는 `NDEBUG`를 정의해 assert를 무력화하므로) 기본 빌드에서 이 검증이 항상 활성화되도록 함.

**의사결정 필요**: 이 assert들을 나중에 Release 빌드에서도 살려둘지(런타임 비용은 거의 0 — 비교 1번), 아니면 Phase 6 프로파일링 이후 안정성이 확인되면 NDEBUG로 끌지.

---

## Phase 4 — Timer / Buffer / Metrics 완전 Local화 ✅

세 가지를 한 패스로 구현 완료.

### 1. Upstream Connect Timeout
`UpstreamManager::acquire_connection()`의 fresh-connect 경로에 `net::ITimer` 하나를 추가해서 `async_connect`와 경합시킴 (`src/upstream/upstream_manager.cpp`). 먼저 끝나는 쪽이 "승자"가 되고 나머지는 `done` 플래그로 무시됨 — 타임아웃이 이기면 `socket->cancel()`로 pending connect를 끊는다. `Config::connect_timeout_seconds`로 설정 (기본 5초). 블랙홀 IP로 실측 검증 — 설정값과 거의 일치하는 시간에 정확히 타임아웃되고 `connect_errors` 카운터도 증가함을 확인.

TimerManager를 별도 클래스로 만들지, 이렇게 각 기능(UpstreamManager, MetricsAggregator)이 필요할 때 `net::ITimer`를 직접 쓰는 방식으로 갈지는 후자로 결정 — 지금 시점엔 "모든 timer를 한곳에 모으는 것"보다 "각 도메인이 자기 timer를 직접 소유하는 것"이 더 단순하고 `net::IEventLoop`이 이미 timer factory 역할을 하고 있어서 중복 추상화가 될 뻔함. Connection Idle Timeout / Request Timeout은 각각 HTTP 레이어와 idle-connection 정리가 실제로 필요해지는 시점에 붙이기로 미룸.

### 2. 실제 BufferPool (free-list)
`src/util/buffer_pool.hpp`가 shard-local free-list를 갖도록 변경. `acquire()`는 free-list에 있으면 재사용, 없으면 새로 `new`. `release()`는 그냥 버리지 않고 free-list에 반납 (상한 `Config::buffer_pool_max_free`, 기본 256 — 초과분은 버림). `Session::close()`가 두 relay 버퍼를 명시적으로 `buffer_pool_.release()`하도록 수정 (이전엔 Session 소멸과 함께 그냥 버려졌음).

### 3. MetricsAggregator
`src/runtime/metrics_aggregator.hpp` 신설. `GatewayRuntime`이 소유하고 listener의 event loop에서 주기적으로(`Config::metrics_report_interval_seconds`, 기본 10초, 0이면 비활성화) 전체 shard의 `LocalMetrics`를 합산해 `[metrics] ...` 형태로 stdout에 출력. `std::endl`로 매번 flush — 파일/파이프 리다이렉트 시에도 바로 보이게 함 (버퍼링 때문에 5초 넘게 안 보이던 문제를 실측으로 발견하고 수정).

**cross-thread read 안전성**: `LocalMetrics`의 모든 필드를 `std::atomic<uint64_t>` + `memory_order_relaxed`로 변경 (`src/util/local_metrics.hpp`). Single-writer(소유 shard)/occasional-reader(aggregator) 패턴이라 락도, seq_cst 순서 보장도 필요 없음 — 이전에 나눴던 "복사본은 왜 스레드 경합에 자유로운가" 대화에서 나온 두 선택지(atomic relaxed vs message passing) 중 전자를 택함. 종료 시 1회 요약(`print_metrics_summary`)도 동일하게 `.load(relaxed)`로 읽도록 수정.

---

## 인프라 리팩토링 2 — MoveOnlyFunction + 단위 테스트 스위트 ✅

### MoveOnlyFunction 도입
`net/` 인터페이스는 가상함수라 콜백 타입을 `std::function`으로 고정해야 했는데, `std::function`은 담기는 대상이 복사 가능해야 해서 `unique_ptr` 캡처(예: accept된 소켓을 shard로 넘길 때)마다 `shared_ptr<unique_ptr<T>>`로 감싸는 boxing이 필요했다. `src/util/move_only_function.hpp`에 `std::function`과 동일한 타입 소거 구조에 복사만 delete한 `MoveOnlyFunction<Signature>`를 직접 구현 (C++23 `std::move_only_function`을 C++20에서 대체, Seastar/Chromium 등의 자체 move-only 콜백과 동일한 해법). `net::VoidCallback/ErrorCallback/IoCallback/SocketCallback/AcceptCallback`을 전부 이걸로 교체.

이 변경으로 `Listener::do_accept()`의 boxing은 완전히 제거됨 (단일 목적지로 이동하는 단순 케이스였음). 반면 `UpstreamManager::acquire_connection()`의 boxing은 유지 — 거긴 connect와 timeout이라는 **두 개의 독립된 비동기 작업이 같은 socket/timer/callback에 동시 접근**해야 하는 진짜 공유 상태라, `shared_ptr`가 원래 맞는 도구였던 케이스임 (자세한 구분은 대화 로그 참고).

### 단위 테스트 스위트 (GoogleTest)
`libgtest-dev` 설치, `tests/` 디렉토리 신설. `net::IEventLoop/ISocket/IResolver/ITimer`의 가짜 구현체(`tests/fakes/`)를 만들어서, 실제 소켓/스레드/시간 경과 없이 `post()`된 작업을 큐에 쌓고 테스트가 `pump()`로 한 스텝씩 결정론적으로 실행시키는 방식으로 async 로직을 검증한다. 이게 net/ 추상화 계층을 처음 만들 때 의도했던 "테스트 가능성"이 실제로 증명된 지점.

커버리지:
- `BufferPool`: free-list 재사용, LIFO 순서, `max_free` 상한
- `LocalMetrics`: 카운터 정확성, `alignas(64)` 검증
- `Config`: JSON 파싱 성공 경로 (단, `Config::from_file`이 에러 시 `std::exit(1)`을 직접 호출해서 에러 경로는 테스트 프로세스를 죽이므로 검증 불가 — `main.cpp`가 catch하는 예외 방식으로 리팩토링하면 해결 가능, 지금은 미룸)
- `UpstreamManager`: round-robin, pool 재사용/상한, **connect timeout 경쟁 조건**(timeout이 이기는 경우/connect가 이기는 경우/뒤늦은 완료가 무시되는 경우 전부)
- `Session`: 양방향 relay, downstream 종료 시 upstream 풀 반납, upstream 종료 시 미반납, connect 실패 처리, 버퍼 반납까지

`cmake --build` 시 GTest 없으면 조용히 테스트 타겟을 건너뛰고 본체만 빌드 (`find_package(GTest QUIET)`).

---

## Phase 5 — CPU Affinity 고도화 ✅ 완료

**계기**: Phase 6 벤치마크 도중 실제 개발 머신(Intel Core Ultra 7 265K)이 **P-core 8개(cpu0-7, ~4.9~5.0GHz) + E-core 12개(cpu8-19, ~4.6GHz, 4개씩 L2 공유)**로 구성된 이기종(hybrid) CPU라는 걸 발견 -- `lscpu -e`의 `MAXMHZ`/`L2` 컬럼으로 확인. NUMA는 이 머신이 단일 소켓/단일 노드라 해당 사항 없음(`lscpu`의 `NUMA node(s): 1`) -- doc이 원래 상정한 멀티소켓 서버 시나리오와 달라서, 이번 Phase의 실질적인 과제는 "NUMA"가 아니라 "이기종 코어 성능 비대칭"이었음.

**설계 확정 사항**:
- P-core/E-core 감지는 `hwloc`의 `cpukinds` API로 (Intel 하이브리드뿐 아니라 ARM big.LITTLE 등도 포괄하는 표준 API). `efficiency` 값이 높을수록 고성능 코어(P-core) -- `hwloc/cpukinds.h` 문서에 명시된 대로 kind_index가 클수록 고성능이라 인덱스를 내림차순으로 순회하면 됨
- shard 수가 P-core 개수를 넘으면 **E-core로 자연스럽게 이어서 배정하고 경고 로그만 출력** (서비스 중단 안 함) -- 상한을 걸어 거부하는 대안도 검토했으나, 가용성을 우선하기로 함
- 특정 코어를 OS/인터럽트 처리용으로 예약하는 기능은 지금 안 만듦 -- 실측으로 필요성이 확인되면 그때 추가 (YAGNI)

**구조**: `src/util/cpu_topology.{hpp,cpp}` -- `build_shard_cpu_plan()`이 `ShardCpuPlan{cpus, top_tier_count}`를 반환. `cpus`는 성능 높은 코어부터 정렬된 논리 CPU 번호, `top_tier_count`는 그중 최고 성능 그룹(P-core)의 개수. hwloc이 이기종 정보를 못 찾으면(동종 CPU) 그냥 0..N-1을 돌려줘서 Phase 1의 기존 동작과 동일하게 폴백. `GatewayRuntime::start()`가 이 plan대로 `shard i → plan.cpus[i]`로 pinning하고, `i >= top_tier_count`면 `[cpu_affinity] warning: ...` 로그를 찍음.

**빌드**: hwloc(BSD 라이선스)이 새 외부 의존성으로 추가됨. CMake config 패키지가 없어서 `pkg-config`로 찾음(`PkgConfig::HWLOC` imported target). Ubuntu는 `libhwloc-dev`만 있으면 됨(CLI 도구 `lstopo`는 별도 패키지 `hwloc-nox`라 빌드엔 불필요).

**검증**: `shard_count=12`(P-core 8개 초과)로 실행해 (1) shard 8~11에서 정확히 경고 로그가 찍히는 것 (2) `ps -L -o tid,psr`로 실제 커널이 보고하는 실행 코어(PSR)가 shard 0~11 → cpu 0~11로 정확히 일치하는 것까지 실측 확인. GTest `tests/cpu_topology_test.cpp` 4개 추가(하드웨어 의존적이라 "P-core가 정확히 몇 개"같은 값 자체가 아니라 "결과가 비어있지 않음/중복 없음/0 이상/top_tier_count가 범위 내" 같은 불변조건만 검증).

---

## Phase 6 — Hot Path Lock 제거 (프로파일링 기반)

이 Phase는 코드를 먼저 새로 짜는 게 아니라 **측정 → 병목 확인 → 제거** 순서로 진행한다.

1. 벤치마크 도구 준비: `wrk` 또는 `ab`로 부하 생성 (HTTP 레이어 도입 후 의미 있음; 그 전엔 raw TCP echo 벤치도 가능)
2. `perf record` / `perf report`, FlameGraph로 hot path의 malloc/free, lock, syscall 비중 확인
3. `bpftrace`로 futex 호출 여부 확인 (mutex contention이 남아있는지)
4. Phase 3/4에서 도입한 ConnectionPool, BufferPool이 실제로 allocation을 줄였는지 `heaptrack`/`valgrind massif`로 검증
5. 문서 section 22의 지표(Throughput/Latency/CPU/Cache/Synchronization/Memory)를 Phase 전후로 비교 기록

---

## Phase 7 — Listener 최적화 (SO_REUSEPORT)

현재 구조(단일 `Listener` 전용 스레드 + `asio::post` round-robin)를 Envoy 스타일로 전환:

```text
GatewayShard #0 ── 자체 listen socket (SO_REUSEPORT)
GatewayShard #1 ── 자체 listen socket (SO_REUSEPORT)
GatewayShard #N ── 자체 listen socket (SO_REUSEPORT)
```

- 커널이 각 shard의 listen socket에 직접 accept를 분산 → 중앙 `Listener` 스레드 및 `asio::post` 홉 자체가 사라짐
- `Listener` 클래스를 `GatewayShard` 내부로 흡수하거나, `acceptor_.set_option(reuse_port)` 옵션을 켠 per-shard acceptor로 교체
- 기존 단일 Listener 모드는 설정 플래그로 남겨두고(`--reuse-port` on/off) 비교 벤치마크로 실제 이득 확인 후 기본값 전환 결정

이 Phase는 아키텍처적으로 가장 마지막에 하는 이유: 지금 구조로도 정확성 검증은 끝났고, single listener의 accept 처리량이 실제 병목인지 Phase 6 프로파일링으로 확인한 뒤 손대는 게 순서에 맞음(성급한 최적화 방지).

---

## 별도 축 — HTTP / Gateway 기능 로드맵

위 Phase 1~7은 "아키텍처적 강건함" 축이고, 이것과 별개로 "기능" 축이 있다 (최초 대화에서 "결국 API Gateway까지 단계적으로"라고 확인됨). 아키텍처 Phase와 인터리빙해서 진행 가능.

1. **HTTP relay (파싱 엔진 + Session 연동)** ✅ 완료 — 아래 상세
2. **Policy/Filter 체인** ✅ 완료 — 아래 상세 (원래 순서상 라우팅 다음이었는데, 사용자가 "필터를 거친 구조화된 데이터를 upstream에 보내고 싶다"고 먼저 요청해서 순서를 당김)
3. **RouteSnapshot / 라우팅**: 문서 section 11의 Immutable Snapshot + Atomic Pointer Swap 패턴으로 Route table 도입. Host/Path 기준 매칭. 필터 체인이 이미 있으니, 라우팅 결과에 따라 필터를 다르게 적용하는 것도 고려 가능.
4. **RuntimeConfigManager**: 설정 hot reload — 새 `RuntimeSnapshot` 생성 후 각 shard에 atomic pointer swap으로 배포, 기존 요청은 기존 snapshot으로 계속 처리. 필터 체인을 지금처럼 코드에서 고정하지 않고 config로 구성하고 싶어지면 이것과 함께 재검토.
5. **TLS**: downstream/upstream 각각 별도로 검토 (termination vs passthrough).

---

### HTTP relay — 파싱/직렬화 엔진 + Session 연동 ✅ 완료

**범위 결정 경위**: 처음엔 Boost.Beast를 파싱 엔진으로만 쓰는 방안을 검토했으나(스트림 개념 없이 `http::parser<buffer_body>`만 순수 상태기계로 사용), 사용자가 "Beast의 사고방식에 지배당하고 싶지 않다, 제품만의 개성 있는 구조를 원한다"고 명시적으로 요청 — Beast를 완전히 배제하는 방향으로 전환. 대안으로 hand-roll 파서와 기존 라이브러리 사용을 저울질하다, **llhttp**(Node.js가 현재 쓰는 HTTP 파서, MIT, 의존성 없는 C 라이브러리)로 확정. apt로 안 되어서 `third_party/llhttp/`에 release 브랜치의 pre-generated 소스(api.c/http.c/llhttp.c + llhttp.h)를 직접 vendoring (Node.js 빌드 툴체인 불필요).

**구조** (`llhttp/`가 `http/`와 형제 디렉토리로 있으면 헷갈린다는 피드백으로, 구현체 두 개(파싱=llhttp, 직렬화=native) 다 `net/http/` 아래로 통일):
```text
src/net/http/             # 라이브러리 독립적 인터페이스 + 값 타입
├─ types.hpp               # RequestHead, ResponseHead, Header
├─ parser.hpp               # IRequestParser, IResponseParser
├─ serializer.hpp           # IRequestSerializer, IResponseSerializer
│
├─ llhttp/                  # 파싱 구현체 (third_party/llhttp/ 사용)
│  ├─ request_parser.{hpp,cpp}
│  ├─ response_parser.{hpp,cpp}
│  └─ factory.{hpp,cpp}      # net::http::llhttp_backend 네임스페이스
│
└─ native/                  # 직렬화 구현체 (외부 의존성 없음 --
   │                         #  llhttp는 파싱만 하고 직렬화는 안 해줌, 그리고
   │                         #  "내가 emit하는 바이트는 내가 다 통제"라 직접
   │                         #  짜는 게 파싱보다 오히려 안전/단순함)
   ├─ request_serializer.{hpp,cpp}
   ├─ response_serializer.{hpp,cpp}
   ├─ detail.hpp              # chunked framing 등 공용 헬퍼
   └─ factory.{hpp,cpp}
```

파서와 시리얼라이저 모두 **I/O와 완전히 분리된 순수 상태 기계** — `net::ISocket`으로 읽은 raw 바이트를 `feed()`에 먹이고, 내보낼 바이트는 `pull()`로 뽑아내는 구조라 (net/event_loop.hpp 때와 마찬가지로) 스트림 개념 없이 조립 가능. Content-Length와 chunked transfer encoding 둘 다 지원.

**실사용 중 발견한 심각한 버그 (수정 완료)**: 최초 구현은 `on_body` 콜백이 body를 고정 크기(16KB) scratch buffer에 담다가 꽉 차면 `HPE_PAUSED`를 리턴해서 llhttp를 일시정지시키려 했다. 그런데 **llhttp의 `on_body`는 `HPE_PAUSED`를 지원하지 않는다** (llhttp.h 주석: `on_body`는 `0, -1, HPE_USER`만 가능 — `on_message_begin`/`on_headers_complete`/`on_message_complete`/`on_chunk_header`만 `HPE_PAUSED` 지원). 그래서 리턴값이 그냥 무시되고 파싱이 계속 진행되어, **body가 64KB(소켓 read 버퍼 크기)를 넘는 요청/응답에서 relay가 조용히 멈춰버리는 버그**가 있었다 (curl이 영원히 응답을 못 받고 hang). `tools/`로 하는 수동 테스트로는 안 잡히고 실제 큰 body(150KB)를 relay해보다가 발견 — 작은 페이로드만 테스트했다면 놓쳤을 버그. 수정: scratch를 고정 크기 배열 대신 `std::string`으로 바꿔서 pause 자체가 필요 없게 만듦 (실제 메모리 사용량은 caller가 `feed()`에 넘기는 raw 청크 크기로 자연히 bound됨). 재발 방지로 200KB body 파싱 테스트를 회귀 테스트로 추가.

**`HttpSession` 연동** (`src/session/http_session.hpp`): raw byte relay `Session`을 완전히 대체 (`Session` 자체는 참고용으로 코드는 남겨뒀지만 더 이상 안 쓰임). 설계 요점:
- HTTP는 파이프라이닝 미지원 전제 하에 반이중이라, raw relay처럼 양방향 동시 릴레이가 아니라 **"요청 relay 완료 → 응답 relay 시작"을 순차 진행**하는 구조로 단순화됨
- **keep-alive 없음** (이번 MVP 범위, 사용자 확정): downstream/upstream 둘 다 요청/응답 1회만 처리하고 close. `UpstreamManager`의 connection pool은 그래서 HTTP relay에서 아예 안 씀 (`release_connection()` 호출 없음) — "pool에서 꺼낸 연결이 진짜 살아있는지 검증"이라는 어려운 문제를 자연히 피함
- `provide_body({nullptr,0}, true)`를 body 없는 메시지에도 반드시 호출해야 하는 시리얼라이저 계약(문서화해뒀던 것)을 정확히 지킴

**검증**: 실제 Python HTTP 서버(GET JSON 응답, POST body echo)를 upstream으로 세워 curl로 End-to-End 확인 — 일반 GET, 404, 동시 다중 요청(shard round-robin 분산 확인), POST body relay, 그리고 위 버그를 실제로 재현/수정 확인한 150KB·200KB POST body.

---

### Policy/Filter 체인 ✅ 완료

**계기**: 사용자가 "파싱 후 구조화된 데이터가 필터 체인을 거쳐 upstream으로 가면 좋겠다"고 직접 요청 — API Gateway의 핵심 기능이라 로드맵 순서(라우팅 다음)를 당김.

**설계 확정 사항** (grill-me로 확인, 헤더 단계):
- 필터는 헤더/target 수정 + 요청 거부(short-circuit) 둘 다 가능
- 요청/응답 메서드가 한 인터페이스(`IFilter`)에 대칭으로 존재 (Envoy/Proxygen처럼) -- 처음엔 `IRequestFilter`/`IResponseFilter`로 분리했다가, 사용자가 "downstream->upstream/upstream->downstream 메서드가 대칭이었으면 좋겠다"고 요청해서 통합
- 필터 인스턴스는 세션별이 아니라 **shard당 싱글턴** (Envoy는 스트림별 인스턴스이지만, 필터 간 데이터 공유(인증->rate limit)까지 고려하면 싱글턴+`FilterContext` 조합이 더 단순하다고 판단 -- grill로 확인)
- 응답은 등록 **역순**으로 실행 (onion 모델, Envoy/Netty와 동일)
- 지금 단계는 코드에서 직접 필터 리스트 구성 (config 기반 동적 구성은 RuntimeConfigManager 생기면 재검토)

**구조**:
```text
src/filter/
├─ filter.hpp                 # IFilter -- on_request/on_response(헤더) + on_request_data/on_response_data(바디) 대칭
├─ filter_context.hpp         # FilterContext, ContextKey<T> -- 세션 스코프 상태 (필터가 싱글턴이라 필요)
├─ filter_direct_response.hpp # DirectResponse{head, body} -- 아래 두 Result 타입의 공통 페이로드
├─ filter_header_result.hpp   # FilterHeaderAction/FilterHeaderResult (헤더 단계 전용)
├─ filter_data_result.hpp     # FilterDataAction/FilterDataResult (바디 단계 전용, kStopIterationAndBuffer 포함)
├─ filter_chain.hpp           # FilterChain -- 등록 순/역순 실행, apply_data() 헬퍼로 바디 순회 중복 제거
├─ via_header_filter.hpp      # 예시 필터 (RFC 7230 Via 헤더 추가)
└─ default_filters.{hpp,cpp}  # build_default_filter_chain() -- 지금은 Via 필터만 등록
```

`GatewayShard`가 `FilterChain`을 소유(shard마다 독립 인스턴스, shared-nothing 원칙 유지)하고 `HttpSession`에 참조로 넘긴다.

**`HttpSession`의 핵심 재구성**: 필터가 요청을 거부하면 **upstream에 연결할 필요조차 없어야** 해서 (인증 실패한 요청 때문에 백엔드 커넥션을 열 이유가 없음), 기존에 "먼저 upstream 연결 → 그다음 요청 relay"였던 순서를 "요청 헤더 파싱 → 필터 실행 → (통과 시) 그제서야 upstream 연결"로 뒤집었다. 요청 필터가 `kRespondDirectly`를 반환하면 `upstream_`은 끝까지 null인 채로 세션이 종료된다. 응답 필터는 이미 upstream이 연결된 뒤라 구조가 더 단순 (응답 헤더 파싱 후 필터만 끼워넣음).

**검증(헤더 단계)**: 실제 Python 업스트림으로 (1) 요청/응답 양쪽에 `Via: 1.1 perCoreShard` 헤더가 실제로 붙는 것을 curl -v로 확인 (2) 임시로 "전부 거부" 필터를 넣고 401이 즉시 반환되면서 **upstream의 accept 카운트가 전혀 늘지 않는 것**(= 진짜로 연결을 안 함)을 실측 확인 후 제거.

---

#### 바디 필터 (스트리밍 + 버퍼링) ✅ 완료

**계기**: "Envoy/Proxygen처럼 바디 접근 + 버퍼링 인터페이스도 있어야 하지 않냐"는 질문에서 시작. Envoy의 실제 모델(`decodeData(Buffer&, bool end_stream)`, `StopIterationAndBuffer`로 필터가 버퍼링을 요청하면 프레임워크가 대신 누적)을 그대로 채택하기로 grill로 확인.

**설계 확정 사항**:
- `IFilter::on_request_data(std::string& data, bool end_stream, FilterContext&)` / `on_response_data(...)` 대칭 추가. `FilterDataResult`는 `kContinue`/`kRespondDirectly`/`kStopIterationAndBuffer` 3가지 (헤더용 `FilterHeaderResult`와 타입을 분리 -- Envoy도 `FilterHeadersStatus`/`FilterDataStatus`를 나눔)
- 필터가 `kStopIterationAndBuffer`를 반환하면 다음 청크가 올 때마다 누적 버퍼로 그 필터를 다시 호출 (Envoy와 동일한 호출 패턴). 버퍼 자체는 필터 인스턴스가 싱글턴이라 필터 멤버에 못 두므로, `HttpSession`이 방향별 `FilterChain::DataIterationState`(막힌 필터 인덱스 + 누적 버퍼)를 들고 있음
- **watermark는 Envoy의 high/low 이중 구조를 안 씀** -- grill 중 발견: 우리 구조(HTTP/1.1, keep-alive 없음, 커넥션당 요청 1개)엔 이중 watermark가 의미를 가질 조건(필터의 부분 방출 API, HTTP/2 멀티플렉싱)이 둘 다 없고, 오히려 "바디 전체가 필요한 필터 + read 일시정지"를 같이 쓰면 데드락이 됨(필터가 풀리는 조건 자체가 "끝까지 다 받아야"인데 read를 멈추면 영원히 못 받음). Envoy의 `envoy.filters.http.buffer`도 이 카테고리는 watermark pause/resume이 아니라 `max_request_bytes` 하드 캡 + 즉시 에러로 처리한다는 걸 확인하고 동일하게 결정: `Config::body_buffer_high_watermark_bytes`(기본 1MB) 하나만 두고 초과 시 즉시 413(요청)/502(응답) 거부, read pause 없음

**실측 중 발견한 버그(수정 완료)**: 필터가 `end_stream` 청크에서도 `kStopIterationAndBuffer`를 반환하면(계약 위반) 처음엔 `assert()`로 프로세스 전체가 죽었음 -- 필터 작성자의 실수 하나로 서버 전체가 죽으면 안 되므로 방어적으로 누적분을 그대로 흘려보내고 강제 종결하도록 수정. 그다음엔 이 방어 로직이 watermark 체크보다 먼저 실행돼서 **단일 청크로 끝나는 요청은 413을 완전히 우회**하는 문제를 발견(이전 호출들이 체크를 통과해왔다는 전제가 이번이 처음이자 마지막 호출인 경우 성립하지 않음) -- watermark 체크를 항상 먼저 하도록 순서 수정.

**검증**: GTest `FilterChainData` 4개(스트리밍 통과 중 필터가 data mutate, 버퍼링 중 뒤 필터로 안 넘어감, 데이터 필터 거부, 응답 onion 순서) 추가. 실제 Python 업스트림으로 GET(바디 없음)/소량 POST/200KB POST(다중 4KB 드레인) e2e 확인, 임시 "항상 버퍼링" 필터 + watermark 16바이트로 낮춰서 413 실제 발생까지 실측 확인 후 제거.

GTest 총 55개 전부 통과.

**아직 안 한 것**: `HttpSession` 자체의 GTest 커버리지 없음 (net::ISocket Fake + 실제 llhttp 파서를 엮어야 해서 upstream_manager_test.cpp/session_test.cpp보다 훨씬 큰 작업 -- 지금은 실측 curl 테스트로만 검증됨). 실제 정책성 필터(인증, rate limit 등)와 상태 있는 스트리밍 변환 필터(gzip 등)는 예시로만 논의됐고 아직 프로덕션에 없음 -- 후자를 실제로 추가하게 되면 `FilterContext`가 핫 패스(매 청크) 접근에 적합한지(현재 `std::any` 기반) 재검토 필요.

---

#### `BodyFilterGate` 추출 ✅ 완료

**계기**: `http_session.hpp`가 512줄까지 늘어나면서 "소켓 I/O 오케스트레이션"과 "필터 적용 정책(watermark 판단)"이 한 클래스에 섞여 있다는 지적. 특히 후자는 위 두 버그(assert 크래시, watermark 우회)가 실제로 났던 부분인데도 `HttpSession`에 끼워져 있어서 소켓/upstream 없이는 GTest로 직접 검증할 수 없었음.

`process_request_data_chunk()`/`process_response_data_chunk()`가 하던 "필터에 청크를 통과시키고 watermark 넘으면 413/502 판단"을 `src/filter/body_filter_gate.hpp`의 `BodyFilterGate`로 뽑아냄. `HttpSession`은 이제 `BodyFilterGate::Result`(kForward/kStillBuffering/kReject)만 받아서 시리얼라이저/`respond_directly()`에 연결하는 얇은 어댑터 역할만 함. `FilterChain::DataIterationState`(요청/응답 각각)도 `HttpSession` 대신 `BodyFilterGate`가 소유.

**검증**: 위에서 발견한 두 버그(assert 크래시, watermark 순서)를 `tests/body_filter_gate_test.cpp`에 회귀 테스트로 고정 (소켓/파서 없이 순수 로직만). GTest 총 63개 전부 통과, 리팩토링 후 실제 upstream으로 GET/POST e2e 재확인.

---

#### `HttpSession` `.hpp`/`.cpp` 분리 ✅ 완료

`UpstreamManager`와 같은 패턴(`.hpp`엔 클래스 선언 + 멤버만, `.cpp`엔 모든 메서드 구현)으로 기계적으로 분리. 설계 변경 없음 -- 순수하게 코드 위치만 옮김. `http_session.hpp`가 462줄 → 127줄로 줄어 클래스 인터페이스가 한눈에 들어옴, 구현(366줄)은 `http_session.cpp`로. `CMakeLists.txt`의 `perCoreShard` 타겟에 소스 추가 (GTest는 `HttpSession`을 직접 include하는 테스트가 없어서 `perCoreShard_tests`엔 불필요).

GTest 63개 전부 통과 + 실제 upstream GET/POST e2e 재확인.

---

## Phase 6 baseline 프로파일링 ✅ 완료

`wrk` 부하 + `perf record`/FlameGraph로 첫 실측(`doc/benchmark-report.md` 참고). `kernel.kptr_restrict=0`으로 낮춰 커널 스택까지 전부 심볼 해석한 뒤 재측정까지 완료. 결론: 뮤텍스 경합은 두 차례 측정 모두 미미(1~2%대)했고, `accept`(downstream, 40% 누적)/`epoll_wait`+`epoll_ctl`(36%) 같은 **"커넥션당 1회 발생하는 커널 objects 할당/syscall 비용"**이 지배적 — keep-alive 부재(매 요청마다 새 accept+connect)가 직접 원인으로 지목됨. `bpftrace`는 락 경합이 이미 낮게 나와서 한계효용이 낮다고 판단해 보류. 이 결과가 바로 아래 keep-alive 작업의 착수 근거가 됨.

---

## keep-alive (downstream + upstream) ✅ 완료

**계기**: Phase 6 baseline 프로파일링에서 accept/connect(요청당 1회 발생하는 커널 비용)가 최대 병목으로 확인됨 — `doc/benchmark-report.md` §9. 도입 전 grill-me로 확인한 결정 사항:
- downstream(클라이언트-게이트웨이) + upstream(게이트웨이-백엔드) **둘 다** 도입 (upstream만/downstream만이 아니라)
- 두 hop을 **독립적으로 판단**(hop-by-hop decouple, 표준 프록시 패턴) — upstream이 응답에서 close를 요구해도 downstream은 계속 keep-alive 가능(다음 요청은 새 upstream 커넥션으로), 그 반대도 마찬가지
- 죽어있는 pooled 커넥션에 대해 **retry-once** (fresh connect로 딱 1회 재시도)

**핵심 구현**:
- `net::http::IRequestParser`/`IResponseParser`에 `should_keep_alive()` 추가 (llhttp `llhttp_should_keep_alive()` 위임). **실측 중 llhttp 자체의 함정을 발견**: llhttp는 `on_message_complete` 콜백 직후(`llhttp__after_message_complete`) `parser->flags`를 0으로 리셋해버려서, 메시지가 끝난 뒤(우리가 pool 반납 여부를 판단하는 시점)에 이 함수를 호출하면 Connection/Content-Length 관련 플래그가 전부 지워진 상태라 keep-alive 응답인데도 항상 `false`가 나옴. `on_message_complete` 콜백 "안"(flags가 아직 유효한 마지막 시점)에서 값을 캐시해두고, `message_done()` 전엔 라이브로/후엔 캐시로 분기하도록 수정 (`src/net/http/llhttp/{request,response}_parser.{hpp,cpp}`)
- `net::http::set_header()`/`has_header()`(`src/net/http/types.hpp`)로 Connection 헤더를 hop마다 독립적으로 덮어씀 -- 클라이언트/upstream이 뭘 보냈든 게이트웨이가 직접 결정
- downstream keep-alive 최종 판단은 "클라이언트가 원했는지" + "**응답이 명확한 프레이밍(Content-Length/chunked)을 가졌는지**"를 곱한 값(`downstream_keep_alive_effective_`) -- close-delimited 응답에서 keep-alive라고 하면 클라이언트에 거짓 약속이 됨
- `HttpSession::reset_for_next_request()`: keep-alive 사이클마다 파서/시리얼라이저를 새로 만들고(reset() 메서드를 따로 안 만들고 기존 factory 재호출 패턴 재사용) `BodyFilterGate::reset()`/`FilterContext` 재생성까지 포함해 요청별 상태를 전부 새로 만듦
- `UpstreamManager::acquire_fresh_connection()` 신설 (pool을 안 보고 항상 fresh connect) -- retry 전용
- **retry-once의 실제 실패 지점이 예상과 달랐음** (SIGKILL로 직접 재현해서 발견): 죽어있는 pooled 커넥션에 대한 첫 **write는 로컬 send buffer에 조용히 성공**하고(에러 없음), 그 직후 응답을 기다리는 **첫 read에서 즉시 실패**로 드러남. 처음엔 write 실패만 재시도 대상으로 짰다가 이 실측으로 재시도가 전혀 안 걸리는 걸 발견 -- `read_response_chunk()`의 첫 read 실패(응답 바이트 0개 상태)도 재시도 대상에 포함하도록 확장. 이 시점엔 이미 시리얼라이저가 다 드레인돼서 요청을 재구성할 수 없으므로, **요청 전체가 write 1번으로 끝난 경우**(바디 없음 등 흔한 케이스)에 한해 그 바이트를 스냅샷(`upstream_whole_request_snapshot_`)해뒀다가 재전송. 스트리밍 중인 큰 바디는 스냅샷 없이(메모리 무제한 증가 방지) 재시도 스킵
- `LocalMetrics::requests_handled`/`upstream_retries` 추가 (기존 `connections_accepted`와 분리 -- 커넥션 1개가 여러 요청을 처리하므로 재사용률 관찰 가능). `requests_handled`는 **요청 헤더가 실제로 파싱된 시점**에만 증가시켜야 함 -- 처음엔 `start()`/`reset_for_next_request()`에서 미리 셌다가, keep-alive 커넥션이 다음 요청 없이 그냥 끊기는 경우까지 요청으로 잘못 잡히는 버그가 있었음(실측 중 발견 후 이동)
- 임시 디버그 로그(`std::cerr << "[debug] ..."`, `acquire_connection`/`release_connection`/`close()` 세 곳)를 의도적으로 남겨둠 -- 사용자 요청으로, 나중에 제대로 된 로깅 체계로 교체 예정

**검증**: GTest 63개 전부 통과. curl/python으로 (1) 같은 다운스트림 커넥션에서 여러 요청 처리(`Connection: keep-alive` 응답 헤더 확인) (2) 클라이언트가 `Connection: close`를 명시하면 정상 종료 (3) 4 shard 동시 다중 요청 (4) **SIGKILL로 pooled 커넥션을 실제로 죽인 뒤 retry-once가 정확히 1회 발동(`upstream_retries` 메트릭)하고 클라이언트가 정상 200을 받는 것까지 직접 재현 확인**.

`tools/bench_upstream.py`를 HTTP/1.0(강제 close) → HTTP/1.1(keep-alive 기본값)로 변경 -- upstream keep-alive 경로를 테스트하려면 필요.

**아직 안 한 것**:
- Connection idle timeout(다운스트림이 다음 요청을 영원히 안 보내는 경우) / upstream pool 커넥션의 idle timeout -- 여전히 미착수 (retry-once가 "이미 죽은 커넥션"은 잡아주지만, "살아있지만 응답을 마냥 기다리는" 상황은 못 막음)
- 파이프라이닝은 여전히 미지원 (요청 처리 도중 도착한 다음 요청의 leftover 바이트는 버려짐) -- 순차 keep-alive만 지원
- HEAD 요청에 대한 응답 파싱 특수 처리 없음 (llhttp에 `llhttp_finish`/skip-body 힌트를 안 줌) -- keep-alive와 무관하게 이미 있던 pre-existing 갭, 이번에 코드 읽다가 확인됨

**재측정하며 발견한 별도 버그 — `TCP_NODELAY` 누락** (`doc/benchmark-report.md` §10): keep-alive 도입 직후 wrk로 재측정하니 p50/p75/p90이 ~41ms에 고정적으로 뭉치는 회귀가 나왔음 -- Nagle + delayed ACK의 전형적 시그니처(Linux 기본 delayed ACK가 40ms). 이전엔 매 요청 후 `close()`가 Nagle이 미뤄둔 마지막 write를 강제 flush해줘서 숨어 있었을 뿐, keep-alive로 연결이 계속 열리자 드러남. `src/net/boost/socket.cpp`(accept된 downstream + connect된 upstream 둘 다)와 `tools/bench_upstream.py`(Python `http.server`도 기본으로 안 켜져 있었음) 양쪽에 `TCP_NODELAY` 추가로 해결 — 처리량 14,783(§6 다른 환경 기준) → 25,704 req/s, p90 810ms → 8.65ms. **일반 교훈**: keep-alive를 새로 켜는 프로젝트에서 흔히 같이 따라오는 함정이라 다음에 비슷한 걸 만들 때 처음부터 체크리스트에 넣을 것.

---

## 다음 세션 시작 시 체크할 것

HTTP 엔진 연동 + 필터 체인 + keep-alive까지 끝나서, 실제로 쓸만한 API Gateway에 가까워졌다. 다음 후보들:

- **실제 정책 필터 추가** — 지금은 예시(Via 헤더)뿐. 인증(JWT 검증 등), rate limiting, CORS 같은 실제 필터를 `src/filter/`에 추가. `IRequestFilter`/`IResponseFilter` 인터페이스는 이미 있으니 새 필터 클래스 + `build_default_filter_chain()`에 등록만 하면 됨
- **라우팅 (RouteSnapshot)** — 지금은 upstream 1개(혹은 여러 개 round-robin)로만 보내고 Host/Path 기반 분기가 없음. 필터 체인과 결합하면 "이 경로엔 이 필터만" 같은 라우트별 정책도 가능해짐
- **connect 실패 시 502 응답** — 지금 `HttpSession::connect_upstream()`이 실패하면 그냥 downstream을 close해버림 (정상적인 HTTP 에러 응답 없음). `respond_directly()`를 이미 필터 short-circuit용으로 만들어뒀으니, 같은 메커니즘을 connect 실패 시에도 재사용해서 502 Bad Gateway를 돌려주는 게 자연스러운 다음 스텝
- **Connection idle timeout** — keep-alive 도입으로 새로 생긴 리스크: 다운스트림이 커넥션만 열어두고 다음 요청을 영원히 안 보내면 세션이 계속 살아있음. downstream/upstream pool 양쪽 다 아직 없음
- **`HttpSession` 자체의 GTest 커버리지** — 지금은 실측 curl/python 테스트로만 검증됨. Fake 소켓 + 실제 llhttp 파서를 엮은 통합 테스트를 추가하면 keep-alive/retry 로직 회귀 방지에 특히 도움
- **디버그 로그를 제대로 된 로깅 체계로 교체** — keep-alive/retry 도입 중 임시로 넣어둔 `std::cerr << "[debug] ..."` 3곳(`UpstreamManager::acquire_connection/release_connection`, `HttpSession::close()`)을 레벨 있는 로거로 교체
- **Phase 5/7** (CPU Affinity 고도화 / SO_REUSEPORT) — Phase 6 baseline은 나왔으니, keep-alive 도입 효과를 재측정(§9 대비 accept/connect 비중이 실제로 줄었는지)한 뒤에 진행하는 게 순서에 맞음

진행할 방향은 grill-me 스타일로 결정 트리를 다시 확인한 뒤 착수할 것 (이 세션 전체에서 일관되게 써온 패턴).
