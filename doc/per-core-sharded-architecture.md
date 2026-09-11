# Per-Core Sharded Architecture

## 1. 목적

본 문서는 C++ 기반 API Gateway의 런타임 구조를 단순화하고 성능을 높이기 위한 **Per-Core Sharded Architecture**의 설계 원칙과 구현 방향을 정의한다.

기존의 일반적인 Multi-Event-Loop 구조는 여러 Event Loop가 요청을 병렬 처리하는 방식이지만, 실제 구현에서는 다음과 같은 문제로 성능 이점이 감소할 수 있다.

- Event Loop 간 공유 자원 접근
- Global Connection Pool
- Global Buffer Pool
- Cross-Thread Task 전달
- Mutex / Atomic 사용 증가
- CPU Core 간 Thread Migration
- Cache Miss 및 Cache Coherency 비용 증가
- 복잡한 Worker Pool 연계

Per-Core Sharded Architecture는 단순히 Event Loop를 여러 개 두는 것이 아니라, **CPU Core 하나를 하나의 독립적인 Gateway Runtime Shard로 취급**하는 것을 핵심으로 한다.

---

# 2. 상위 철학

## 2.1 CPU Core를 독립적인 실행 단위로 본다

기본 개념은 다음과 같다.

```text
1 CPU Core
    =
1 OS Thread
    =
1 Boost.Asio io_context
    =
1 Gateway Shard
```

각 Shard는 자신에게 할당된 요청과 Connection을 독립적으로 처리한다.

```text
CPU Core 0
└─ GatewayShard #0
   ├─ io_context
   ├─ Downstream Connections
   ├─ Upstream Connections
   ├─ Upstream Pool
   ├─ Timer
   ├─ Buffer Pool
   └─ Local Metrics

CPU Core 1
└─ GatewayShard #1
   └─ ...

CPU Core 2
└─ GatewayShard #2
   └─ ...
```

핵심은 **같은 요청 처리 과정이 가능한 한 같은 CPU Core에서 끝나도록 유지하는 것**이다.

---

## 2.2 Shared-Nothing을 기본값으로 한다

성능에 민감한 Hot Path에서는 가능한 한 공유 상태를 만들지 않는다.

피해야 할 구조:

```text
Shard 0 ─┐
Shard 1 ─┼── Global Connection Pool
Shard 2 ─┤
Shard 3 ─┘
             │
             ▼
          Mutex
```

권장 구조:

```text
Shard 0 → Local Connection Pool
Shard 1 → Local Connection Pool
Shard 2 → Local Connection Pool
Shard 3 → Local Connection Pool
```

동일한 원칙을 다음 리소스에 적용한다.

- Upstream Connection Pool
- Buffer Pool
- Timer
- Runtime Cache
- Temporary Object Pool
- Request Statistics
- Rate Limit Local Counter
- DNS Cache
- Connection Registry

목표는 다음과 같다.

```text
Hot Path
  ├─ Lock 최소화
  ├─ Atomic 최소화
  ├─ Cross-Core 이동 최소화
  ├─ Heap Allocation 최소화
  └─ Cache Locality 최대화
```

---

## 2.3 Request / Connection Ownership을 명확히 한다

하나의 Connection은 특정 Shard가 소유한다.

```text
Connection A → Shard 0
Connection B → Shard 1
Connection C → Shard 2
```

Connection에 속한 Request도 기본적으로 동일한 Shard에서 처리한다.

```text
Client Request
      │
      ▼
   Shard #2
      │
      ├─ Read
      ├─ HTTP Parse
      ├─ Route Lookup
      ├─ Filter
      ├─ Endpoint Select
      ├─ Upstream Write
      ├─ Upstream Read
      ├─ Response Filter
      └─ Client Write
```

다른 Shard가 해당 Connection 또는 Request 상태를 직접 수정하지 않는다.

즉:

> **State Ownership은 하나의 Shard에 귀속한다.**

---

# 3. 기존 Multi-Event-Loop와의 차이

Netty와 같은 일반적인 Multi-Event-Loop 모델도 여러 Event Loop를 사용한다.

```text
EventLoopGroup
├─ EventLoop 0
├─ EventLoop 1
├─ EventLoop 2
└─ EventLoop 3
```

Per-Core Sharded Architecture와의 차이는 Event Loop의 개수가 아니라 **소유권과 리소스 배치에 대한 강한 제약**이다.

## Multi-Event-Loop

```text
EventLoop
   │
   ├─ Connection
   ├─ Request
   │
   └─ Shared Resource 접근 가능
```

## Per-Core Sharded

```text
CPU Core
   │
Pinned Thread
   │
io_context
   │
GatewayShard
   ├─ Connections
   ├─ Upstream Pool
   ├─ Buffer Pool
   ├─ Timers
   ├─ Cache
   └─ Metrics
```

Per-Core Sharded Architecture에서는 다음이 중요한 설계 조건이다.

```text
Core Affinity
+
Shard Ownership
+
Shared-Nothing
+
Local Resource
+
Minimal Cross-Core Communication
```

---

# 4. 전체 런타임 아키텍처

```text
                         Client
                           │
                           ▼
                   Listener / Accept
                           │
                  Connection Steering
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
         ▼                 ▼                 ▼

      Core #0           Core #1           Core #2
         │                 │                 │
    Thread #0         Thread #1         Thread #2
         │                 │                 │
   io_context #0     io_context #1     io_context #2
         │                 │                 │
   GatewayShard 0    GatewayShard 1    GatewayShard 2
         │
         ├─ Downstream Connections
         ├─ Upstream Connections
         ├─ Upstream Connection Pool
         ├─ Timer Manager
         ├─ Buffer Pool
         ├─ Request Context Pool
         ├─ Route Snapshot
         └─ Local Metrics
```

각 Shard는 사실상 작은 독립 Gateway Runtime으로 동작한다.

---

# 5. GatewayShard

GatewayShard가 런타임의 핵심 단위다.

예시:

```cpp
class GatewayShard {
public:
    void run();

private:
    boost::asio::io_context io_context_;

    DownstreamManager downstream_;
    UpstreamManager upstream_;

    TimerManager timers_;
    BufferPool buffers_;

    RouteSnapshot routes_;

    LocalMetrics metrics_;
};
```

각 Shard는 하나의 Thread에서만 실행한다.

```text
GatewayShard #0
      │
      └─ Thread #0
             │
             └─ CPU Core #0
```

---

# 6. Thread 및 CPU Affinity

Shard Thread는 특정 CPU Core에 고정한다.

```text
Thread #0 → CPU Core #0
Thread #1 → CPU Core #1
Thread #2 → CPU Core #2
Thread #3 → CPU Core #3
```

Linux에서는 다음과 같은 API를 사용할 수 있다.

```cpp
pthread_setaffinity_np(...)
```

목적은 Thread Migration을 줄이는 것이다.

Thread가 CPU Core를 이동하면 다음 비용이 발생할 수 있다.

```text
Core #0
  L1/L2 Cache
      │
      │ Thread Migration
      ▼
Core #3
  Cache Miss
      │
      ▼
Memory / LLC Access
```

Core Affinity를 유지하면 Request 처리에 자주 사용하는 데이터가 동일한 Core Cache에 머물 가능성이 높아진다.

---

# 7. Boost.Asio 구성

각 Shard는 독립적인 `boost::asio::io_context`를 가진다.

```text
Core #0
 └─ Thread #0
     └─ io_context #0

Core #1
 └─ Thread #1
     └─ io_context #1
```

예시:

```cpp
struct GatewayShard {
    boost::asio::io_context io;
    std::thread thread;
};
```

실행:

```cpp
void GatewayShard::start(int cpu) {
    thread = std::thread([this, cpu]() {
        pin_thread_to_cpu(cpu);
        io.run();
    });
}
```

중요한 점은 여러 Thread가 하나의 `io_context`를 공유하는 형태를 기본 구조로 사용하지 않는 것이다.

피해야 할 기본 구조:

```text
                io_context
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
     Thread 0    Thread 1    Thread 2
```

권장 구조:

```text
io_context 0 → Thread 0 → Core 0
io_context 1 → Thread 1 → Core 1
io_context 2 → Thread 2 → Core 2
```

---

# 8. Connection Distribution

새로운 Connection은 특정 Shard에 배분한다.

예:

```text
Listener
   │
   ├─ Connection A → Shard 0
   ├─ Connection B → Shard 1
   ├─ Connection C → Shard 2
   ├─ Connection D → Shard 3
   └─ Connection E → Shard 0
```

초기 구현은 Round-Robin으로 충분하다.

```cpp
shard_index = next++ % shard_count;
```

Linux에서는 이후 다음 방식을 검토할 수 있다.

- SO_REUSEPORT
- RSS
- CPU Affinity
- NUMA-aware steering

보다 발전된 구조에서는 각 Shard가 자체 Listen Socket을 가질 수도 있다.

```text
            Client
              │
          SO_REUSEPORT
              │
     ┌────────┼────────┐
     ▼        ▼        ▼

  Listener Listener Listener
   Core 0   Core 1   Core 2
     │        │        │
   Shard0   Shard1   Shard2
```

이 방식은 중앙 Accept Thread 자체도 제거할 수 있다.

---

# 9. Request Processing

Request 하나는 가능한 한 Shard 내부에서 끝까지 처리한다.

```text
Client
  │
  ▼
Shard #1
  │
  ├─ socket read
  │
  ├─ HTTP parsing
  │
  ├─ route lookup
  │
  ├─ policy/filter
  │
  ├─ load balancing
  │
  ├─ upstream pool lookup
  │
  ├─ upstream send
  │
  ├──────── I/O wait ────────┐
  │                           │
  │      다른 요청 처리       │
  │                           │
  ◀──── upstream response ────┘
  │
  ├─ response filter
  │
  └─ downstream send
```

Upstream 응답을 기다리는 동안 Thread가 Block되는 것이 아니다.

Boost.Asio의 Async I/O를 통해 Event Loop가 다른 Connection을 계속 처리한다.

---

# 10. Upstream Connection Pool

Connection Pool 역시 Shard Local로 구성한다.

```text
Shard 0
 └─ UpstreamPool 0
     ├─ Backend A connections
     ├─ Backend B connections
     └─ Backend C connections

Shard 1
 └─ UpstreamPool 1
     ├─ Backend A connections
     ├─ Backend B connections
     └─ Backend C connections
```

Global Connection Pool은 사용하지 않는 것을 기본 원칙으로 한다.

장점:

- Mutex 제거
- Connection Owner 명확화
- socket의 Event Loop 이동 방지
- Cache Locality 향상
- 구현 단순화

단점:

- Shard별 Connection 수 편차 가능
- 전체 Connection 사용률이 약간 비효율적일 수 있음

API Gateway에서는 일반적으로 이 단점보다 Hot Path 단순화의 장점이 더 크다.

---

# 11. Runtime Configuration

Route, Policy, Endpoint와 같은 설정은 모든 Shard가 읽어야 한다.

그러나 이를 Mutable Global Object 형태로 공유하면 안 된다.

권장 방식:

```text
Control / Config Thread
          │
          ▼
   RuntimeSnapshot V2
          │
       immutable
          │
    ┌─────┼─────┐
    ▼     ▼     ▼
 Shard0 Shard1 Shard2
```

설정 객체는 Immutable Snapshot으로 생성한다.

예:

```cpp
struct RuntimeSnapshot {
    RouteTable routes;
    EndpointTable endpoints;
    PolicyTable policies;
};
```

배포 시:

```text
V1 Snapshot
    │
Config Update
    ▼
V2 Snapshot 생성
    │
    ▼
Atomic Pointer Swap
    │
    ▼
새 Request → V2
기존 Request → 기존 Snapshot 사용 후 종료
```

Hot Path에서 Config Lock을 잡지 않는 것이 중요하다.

---

# 12. Cross-Core Communication

Cross-Core Communication은 완전히 제거하기 어렵다.

예:

- Config 변경
- Shutdown
- Metrics Aggregation
- Health 상태 변경
- Global Rate Limit
- Administrative Command

그러나 Request Hot Path에서는 피해야 한다.

권장 구조:

```text
Shard 0 ──┐
Shard 1 ──┼── Control / Aggregator
Shard 2 ──┤
Shard 3 ──┘
```

Cross-Core 전달이 필요한 경우 Message Passing을 사용한다.

```text
Shard 3
   │
   ▼
MPSC Queue
   │
   ▼
Shard 1
```

중요한 원칙:

> 다른 Shard의 Object를 직접 접근하지 않는다.

즉:

```cpp
shard1.connection->close();
```

같은 접근보다:

```cpp
shard1.post(CloseConnection{id});
```

형태가 적합하다.

---

# 13. Worker Pool 사용 원칙

Worker Pool은 Gateway의 기본 실행 모델이 아니다.

기본:

```text
Request
   │
   ▼
GatewayShard
   │
   └─ 대부분의 작업 처리
```

Worker Pool은 Event Loop를 장시간 점유하는 작업에만 사용한다.

예:

- 대용량 압축
- CPU Heavy 암호 연산
- 매우 복잡한 JSON Transformation
- Blocking SDK
- Blocking File I/O
- 동기 DB Client

```text
GatewayShard
      │
      │ Heavy Task
      ▼
 CPU Worker
      │
      │ Result
      ▼
GatewayShard Queue
      │
      ▼
 Request Resume
```

Worker가 Request Context를 직접 수정해서는 안 된다.

---

# 14. Memory Allocation

고성능 Gateway에서는 Thread Scheduling보다 Memory Allocation이 병목이 되는 경우도 많다.

따라서 Shard Local Allocation 전략을 사용한다.

```text
Shard 0
 ├─ Buffer Pool
 ├─ Request Context Pool
 └─ Temporary Object Pool

Shard 1
 ├─ Buffer Pool
 ├─ Request Context Pool
 └─ Temporary Object Pool
```

목표:

```text
malloc/free
    ↓
최소화

global allocator contention
    ↓
최소화

cross-core free
    ↓
최소화
```

가능하면 생성한 Core에서 해제한다.

```text
Allocate Core 2
      │
      ▼
    Object
      │
      ▼
Free Core 2
```

---

# 15. False Sharing 방지

서로 다른 Shard가 동일 Cache Line의 데이터를 수정하면 성능이 저하될 수 있다.

예:

```cpp
struct GlobalMetrics {
    std::atomic<uint64_t> shard0;
    std::atomic<uint64_t> shard1;
};
```

논리적으로는 다른 변수이지만 동일 Cache Line에 들어갈 수 있다.

권장:

```cpp
struct alignas(64) ShardMetrics {
    uint64_t requests;
    uint64_t errors;
};
```

각 Shard가 자신의 Metrics만 수정하고, 별도 Aggregator가 주기적으로 읽는다.

---

# 16. Timer

Timer도 Shard Local로 둔다.

```text
Shard 0 Timer
 ├─ Connection Timeout
 ├─ Request Timeout
 ├─ Upstream Timeout
 └─ Retry Timer
```

다음과 같은 Global Timer Thread는 피하는 것이 좋다.

```text
                 Global Timer
                     │
       ┌─────────────┼─────────────┐
       ▼             ▼             ▼
    Shard0         Shard1        Shard2
```

Timer 만료 역시 원래 Request를 소유한 Shard에서 처리한다.

Boost.Asio에서는 `steady_timer`를 해당 Shard의 `io_context`에 연결하면 자연스럽게 구현할 수 있다.

---

# 17. Load Balancing

Endpoint 선택 또한 Shard 내부에서 수행한다.

```text
Shard #0
   │
   ├─ Route Lookup
   │
   ├─ Egress Group
   │
   ├─ Local LB State
   │
   └─ Endpoint Select
```

Round Robin과 같이 상태가 필요한 경우 Global Counter 대신 Shard Local Counter를 사용하는 것을 우선 검토한다.

```text
Global Round Robin
      │
      ▼
 atomic fetch_add
```

보다:

```text
Shard 0 Round Robin Counter
Shard 1 Round Robin Counter
Shard 2 Round Robin Counter
```

형태가 Hot Path에 유리하다.

Gateway 전체 관점에서 완벽히 균일한 Round Robin보다 CPU Locality를 우선하는 설계다.

---

# 18. Metrics

Metrics 역시 요청마다 Global Atomic을 증가시키는 것을 피한다.

피해야 할 형태:

```text
Shard0 ─┐
Shard1 ─┼── global_requests.fetch_add(1)
Shard2 ─┤
Shard3 ─┘
```

권장:

```text
Shard0 → requests = 100
Shard1 → requests = 120
Shard2 → requests = 110
Shard3 → requests = 105

        │
        ▼

Metrics Aggregator

total = 435
```

---

# 19. 장애 격리

Shard가 독립적인 Runtime이라는 개념은 성능뿐 아니라 장애 분석에도 도움이 된다.

```text
Gateway Process
├─ Shard 0
├─ Shard 1
├─ Shard 2
└─ Shard 3
```

운영 시 다음 값을 Shard 단위로 관찰할 수 있다.

- Active Connection
- Request Rate
- Event Loop Lag
- Upstream Pending
- Buffer Usage
- Queue Depth
- Timeout Count
- CPU Usage
- Local Memory Usage

특정 Shard만 과부하되는 현상도 쉽게 확인할 수 있다.

---

# 20. 권장 클래스 구조

초기 구현은 다음 정도로 단순하게 유지한다.

```text
GatewayRuntime
│
├─ GatewayShard[]
│
├─ RuntimeConfigManager
│
└─ MetricsAggregator


GatewayShard
│
├─ asio::io_context
├─ Listener
├─ DownstreamManager
├─ UpstreamManager
├─ TimerManager
├─ BufferPool
├─ RouteSnapshot
└─ LocalMetrics


UpstreamManager
│
├─ EgressGroup
│
├─ Endpoint
│
├─ LoadBalancer
│
└─ ConnectionPool
```

중요한 점은 추상화 계층을 지나치게 많이 만들지 않는 것이다.

Hot Path는 가능한 한 다음처럼 짧아야 한다.

```text
Socket
  ↓
HTTP Parser
  ↓
Route
  ↓
Policy / Filter
  ↓
Endpoint Select
  ↓
Upstream Socket
```

---

# 21. 단계별 리팩토링 전략

기존 Gateway를 한 번에 재작성할 필요는 없다.

## Phase 1. Event Loop 분리

```text
1 io_context
   ↓
N Thread
```

구조라면 다음으로 변경한다.

```text
N io_context
   ↓
N Thread
   ↓
N Core
```

---

## Phase 2. Connection Ownership

모든 Connection이 특정 Shard에 귀속되도록 변경한다.

```text
Connection
    ↓
GatewayShard
```

---

## Phase 3. Upstream Pool Sharding

Global Upstream Pool을 제거한다.

```text
Global Pool
```

에서:

```text
Shard Local Pool
```

로 변경한다.

---

## Phase 4. Timer / Buffer / Metrics Local화

다음 공유 리소스를 하나씩 Shard Local로 이동한다.

- Timer
- Buffer
- Request Context
- Metrics
- Load Balancer State

---

## Phase 5. CPU Affinity

Shard Thread를 CPU Core에 Pinning한다.

---

## Phase 6. Hot Path Lock 제거

Profiler를 기반으로 Hot Path의 다음 항목을 제거한다.

- mutex
- atomic contention
- malloc/free
- thread hopping
- global queue

---

## Phase 7. Listener 최적화

필요하면 중앙 Acceptor 구조에서 다음 구조로 변경한다.

```text
SO_REUSEPORT
+
Per-Core Listener
```

---

# 22. 성능 검증 지표

아키텍처 변경 효과는 TPS만으로 판단하면 안 된다.

다음 항목을 함께 측정한다.

### Throughput

```text
Requests/sec
Connections/sec
Bandwidth
```

### Latency

```text
p50
p95
p99
p99.9
```

### CPU

```text
CPU utilization
context switches
CPU migrations
```

### Cache

```text
L1 cache miss
LLC cache miss
cache references
```

### Synchronization

```text
mutex contention
atomic contention
futex calls
```

### Memory

```text
allocations/request
bytes/request
RSS
```

Linux 환경에서는 다음 도구를 활용할 수 있다.

```text
perf
FlameGraph
bpftrace
valgrind massif
heaptrack
```

---

# 23. 설계 원칙 요약

Per-Core Sharded Architecture의 핵심은 다음 문장으로 요약할 수 있다.

> **CPU Core 하나를 하나의 독립적인 Gateway Runtime으로 구성하고, Request와 Connection의 생명주기를 가능한 한 동일한 Core 내부에서 처리한다.**

이를 위한 세부 원칙은 다음과 같다.

```text
1 Core
   ↓
1 Thread
   ↓
1 io_context
   ↓
1 GatewayShard
   ↓
Local Connections
Local Upstream Pool
Local Buffer
Local Timer
Local Metrics
```

그리고 Hot Path에서는 다음을 최소화한다.

```text
Shared State
Cross-Core Communication
Mutex
Atomic Contention
Heap Allocation
Thread Migration
```

즉 이 구조의 본질은 단순히 Event Loop를 여러 개 사용하는 것이 아니다.

**CPU Cache Locality와 State Ownership을 아키텍처 수준에서 강제하여 하나의 Core가 요청 처리에 필요한 대부분의 상태와 실행을 책임지도록 만드는 것**이 핵심이다.

---

# 24. 최종 목표 구조

```text
                             Client
                               │
                               ▼
                     SO_REUSEPORT / RSS
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
          ▼                    ▼                    ▼

      CPU Core 0           CPU Core 1           CPU Core 2
          │                    │                    │
      Thread 0             Thread 1             Thread 2
          │                    │                    │
    io_context 0         io_context 1         io_context 2
          │                    │                    │
   GatewayShard 0       GatewayShard 1       GatewayShard 2
          │
          ├─ Listener
          ├─ Downstream Connection
          ├─ HTTP Processing
          ├─ Route / Policy
          ├─ Load Balancer
          ├─ Upstream Connection Pool
          ├─ Buffer Pool
          ├─ Timers
          └─ Metrics

                 Hot Path = Core Local

                          │
                 Exceptional Only
                          ▼

                 Cross-Core Message
                 Worker / Control
```

이 구조를 기준으로 이후 각 Gateway 컴포넌트를 하나씩 리팩토링한다.
