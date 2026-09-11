# perCoreShard 구현 로드맵

`doc/per-core-sharded-architecture.md`의 Phase 1~7 전략과, 이번 대화에서 나온 실무적 이슈(DNS 재조회, connect timeout 등)를 결합해 앞으로의 작업을 정리한다. 각 Phase는 이전 Phase가 끝나야만 시작 가능한 건 아니고, 문서 자체도 "단계별 리팩토링 전략"이라고 명시했듯 **점진적** 진행을 전제로 한다.

---

## 현재 상태 (완료)

### Phase 1 — Event Loop 분리 ✅
`GatewayShard`마다 독립 `io_context` + pinned thread (`src/gateway_shard.hpp/.cpp`, `src/cpu_affinity.hpp`).

### Phase 2 — Connection Ownership ✅
`Listener`가 accept한 소켓을 round-robin으로 정확히 하나의 `GatewayShard`에 `asio::post`로 위임, 그 shard가 `Session` 생명주기 전체를 소유 (`src/listener.hpp`, `src/session.hpp`).

MVP 범위: 순수 TCP byte relay, 단일 고정 upstream, HTTP 파싱 없음. git 첫 커밋(`e4b0ab1`) 완료.

**알려진 갭 (당시 기준, 일부는 Phase 3에서 이미 해소됨):**
- ~~upstream 1개 고정, 커넥션 재사용 없음~~ → Phase 3에서 해결 (다중 upstream + Connection Pool)
- ~~DNS resolve가 부팅 시 1회뿐~~ → Phase 3에서 해결 (주기적 재조회)
- upstream connect에 타임아웃 없음 → 여전히 Phase 4 대기
- `BufferPool`이 실제 pool이 아니라 매번 new/delete → 여전히 Phase 4 대기
- Listener가 단일 스레드 → Phase 7에서 재검토

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

## Phase 4 — Timer / Buffer / Metrics 완전 Local화 (다음 단계 후보)

### TimerManager (신규, shard-local)
```text
GatewayShard::TimerManager
├─ Upstream Connect Timeout   # Session::connect_upstream()에 steady_timer 추가
├─ Connection Idle Timeout    # 일정 시간 무입출력 시 close()
├─ Request Timeout            # HTTP 레이어 도입 후 적용 (Phase 진입 시점에 따라 순서 조정 가능)
└─ Retry Timer                # Phase 3 ConnectionPool과 연동
```
가장 먼저 처리할 항목은 **upstream connect timeout** — 지금 `Session::connect_upstream()`은 timeout이 없어 upstream이 응답 없으면 무한 대기한다 (직전 대화에서 확인된 이슈). `boost::asio::steady_timer` 하나를 `Session`에 추가해서 `async_connect`와 경합시키는 것으로 최소 구현 가능.

### 실제 BufferPool (free-list)
현재 `src/buffer_pool.hpp`는 `acquire()`마다 `new`, `release()`에서 그냥 버림. 이를 shard-local free-list로 바꿔서 buffer 재사용 → allocation/free 빈도 감소 (문서 section 14의 목표).

### MetricsAggregator
현재는 종료 시(`GatewayRuntime::print_metrics_summary`) 1회 합산뿐. 운영 중 관찰이 필요하면:
- 주기적 타이머로 각 shard의 `LocalMetrics`를 읽어 집계 (읽기만 하므로 lock 불필요 — 단, cache-line 경계 때문에 다른 스레드가 읽는 동안 tearing 가능성은 낮지만 완전한 원자성은 없음. 필요하면 개별 필드를 `std::atomic`이 아니라 "근사치로 충분"하다는 전제로 그대로 두거나, snapshot 복사 방식 고려)
- 간단한 `/metrics` HTTP endpoint 또는 stdout periodic dump로 노출

---

## Phase 5 — CPU Affinity 고도화

Phase 1에서 기본 pinning(`pin_thread_to_cpu`, shard i → core i)은 이미 구현됨. 남은 작업:
- `lscpu`/`hwloc` 기반으로 실제 NUMA topology, hyperthread(sibling) 구조를 감지해서 매핑 개선 (지금은 순진하게 0..N-1 순서로 고정)
- `--shards`가 물리 코어 수보다 크게 설정된 경우(hyperthreading 포함 논리 코어 초과 등)에 대한 처리 정책 결정
- 필요 시 특정 코어를 제외(OS/인터럽트 처리용으로 예약)하는 옵션 추가

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

1. **Boost.Beast HTTP relay**: `Session`을 raw byte relay에서 HTTP request/response 파싱 기반으로 교체(또는 병행 — L4/L7 모드 선택 가능하게). `boost::beast::http::async_read`/`async_write` 사용.
2. **RouteSnapshot / 라우팅**: 문서 section 11의 Immutable Snapshot + Atomic Pointer Swap 패턴으로 Route table 도입. Host/Path 기준 매칭.
3. **Policy/Filter 체인**: 요청/응답 변형, 헤더 조작 등.
4. **RuntimeConfigManager**: 설정 hot reload — 새 `RuntimeSnapshot` 생성 후 각 shard에 atomic pointer swap으로 배포, 기존 요청은 기존 snapshot으로 계속 처리.
5. **TLS**: downstream/upstream 각각 별도로 검토 (termination vs passthrough).

**의사결정 필요:** HTTP 도입을 몇 번째 순서로 넣을지 — 예를 들어 Phase 3(Upstream Pool)보다 먼저 Beast부터 넣을 수도 있음. 우선순위는 사용자가 다음에 확인하고 싶은 것에 따라 정하면 됨.

---

## 다음 세션 시작 시 체크할 것

- 위 로드맵 중 어느 Phase/기능부터 진행할지 확인 (grill-me로 결정 트리 재확인 권장)
- Phase 3부터 진행한다면: config 포맷 확장 여부부터 결정
- HTTP부터 진행한다면: Beast 도입이 `Session`을 완전히 대체하는지, 아니면 raw relay 모드와 공존시키는지부터 결정
