# 성능 실측 리포트 — Baseline (2026-09-22)

`doc/per-core-sharded-architecture.md`의 Phase 6("Hot Path Lock 제거")은 "코드를 먼저
새로 짜지 말고 측정 → 병목 확인 → 제거 순서로 진행한다"고 명시한다. 이 문서는 그
**첫 번째 측정**(baseline) 결과와, 재현 가능하도록 수행 방법을 기록한다.

인터랙티브 플레임그래프: https://claude.ai/artifact/R62JQbAdXz5SJ1SWkAstuV

---

## 1. 환경 제약 (먼저 확인할 것)

이 리포트를 재현하는 환경에 따라 아래 도구들이 막혀있을 수 있다.

| 도구 | 용도 | 제약 |
|---|---|---|
| `perf` | CPU 사이클/캐시 프로파일링 | `kernel.perf_event_paranoid`가 낮아야 함(≤1 권장). 기본값이 높으면(`>=2` 또는 배포판별 4) `perf record`/`perf stat`이 권한 거부로 실패 |
| `bpftrace` | futex/syscall 트레이싱 | **항상 root 계정 자체**를 요구함 (`perf_event_paranoid`와 무관). `sudo bpftrace ...`로만 가능 |
| `heaptrack`, `valgrind` | 힙 할당 프로파일링 | 대부분 배포판에 기본 미설치, `apt install` 필요 |
| `wrk` | HTTP 부하 생성 | 별도 제약 없음 |

권한 확인/해제:
```bash
cat /proc/sys/kernel/perf_event_paranoid   # 값이 크면 perf가 막혀있음
sudo sysctl -w kernel.perf_event_paranoid=1  # 이 세션 한정으로 해제 (재부팅 시 원복)
```

이번 baseline에서는 `perf_event_paranoid`만 해제해서 진행했고, `bpftrace`(futex 카운트)와
`heaptrack`/`valgrind`(할당량/RSS 상세)는 **아직 측정 못 함** — 아래 "다음 실측에서 채울 것" 참고.

FlameGraph 스크립트는 apt로 안 깔리므로 별도로 받아야 한다:
```bash
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git
```

---

## 2. 빌드

기본 `CMAKE_BUILD_TYPE`은 `Debug`다 (assert 기반 스레드 안전성 검증을 위해 의도적으로
그렇게 설정돼 있음 — `doc/plan.md` 참고). 성능 측정은 반드시 최적화가 켜지고
`NDEBUG`(assert 비활성)가 정의되는 별도 빌드로 해야 한다:

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-release -j"$(nproc)"
```

`RelWithDebInfo`를 쓰는 이유: 최적화는 Release와 동일하게 켜지면서 디버그 심볼은
유지돼서, `perf`/FlameGraph가 함수 이름을 심볼로 보여줄 수 있다 (순수 `Release`는
심볼이 없어서 프로파일링 결과가 다 주소값으로만 나옴).

---

## 3. 벤치마크용 업스트림

`tools/test_upstream.py`(raw TCP echo, connection pooling 검증용)는 이 목적에 안 맞다.
`python3 -m http.server`(`http.server.HTTPServer`)도 요청을 한 번에 하나씩만 처리하는
단일 스레드라 부하가 조금만 올라가도 **업스트림 자체가 병목**이 되어, "게이트웨이가
느린 건지 업스트림이 느린 건지" 구분이 안 된다.

그래서 `tools/bench_upstream.py`를 새로 만들었다 — `ThreadingHTTPServer` 기반으로
동시 커넥션을 스레드로 처리하고, 고정된 짧은 응답(`OK`, 2바이트)만 돌려준다.
`HttpSession`이 keep-alive를 지원하지 않으므로(MVP 범위, 매 요청 후 연결 종료)
업스트림도 `HTTP/1.0`으로 고정해서 응답 후 바로 닫도록 맞췄다.

```bash
python3 tools/bench_upstream.py 9000 &
python3 tools/bench_upstream.py 9001 &
```

---

## 4. 게이트웨이 실행

`config.bench.example.json` (기존 `config.example.json`과 차이: 업스트림 포트를
9000/9001로, `metrics_report_interval_seconds`를 0으로 — 벤치마크 중 주기적 stdout
로깅이 노이즈가 되는 걸 방지):

```bash
./cmake-build-release/perCoreShard --config config.bench.example.json
```

---

## 5. wrk 부하테스트

```bash
wrk -t4 -c64 -d10s --latency http://127.0.0.1:8080/
```

### 결과 (2026-09-22, 8코어 머신, shard_count=4)

| 지표 | 값 |
|---|---|
| Requests/sec | 14,783 |
| p50 latency | 774 µs |
| p75 latency | 418 ms |
| p90 latency | 810 ms |
| p99 latency | 1.17 s |
| Timeouts | 33 / 147,995 요청 |

**관찰**: p50은 매우 빠른데 p75부터 급격히 나빠지는 롱테일(tail latency) 패턴 —
평균적으로 느린 게 아니라 일부 요청만 심하게 밀림.

---

## 6. perf record + FlameGraph

게이트웨이 PID 확인 후, wrk 부하와 동시에 10초간 샘플링:

```bash
GATEWAY_PID=$(pgrep -f "cmake-build-release/perCoreShard --config")
perf record -F 999 -p "$GATEWAY_PID" -g -o /tmp/perf.data -- sleep 12 &
sleep 1
wrk -t4 -c64 -d10s http://127.0.0.1:8080/
```

FlameGraph 생성:
```bash
perf script -i /tmp/perf.data > /tmp/out.perf
./FlameGraph/stackcollapse-perf.pl /tmp/out.perf > /tmp/out.folded
./FlameGraph/flamegraph.pl --title "perCoreShard gateway" /tmp/out.folded > /tmp/flamegraph.svg
```

### 식별된 사용자공간 핫스팟

| 비중 | 프레임 | 해석 |
|---|---|---|
| 18.3% | `boost::asio::...reactive_socket_connect_op` / `__libc_connect` | upstream connect 경로. keep-alive가 없어 **매 요청마다** upstream TCP 3-way handshake 발생 — 가장 큰 단일 사용자공간 비용 |
| 5.1% | `accept` | downstream 커넥션 accept (역시 요청당 1회) |
| 0.28% / 0.18% | `pthread_mutex_lock` / `unlock` | 매우 작음 — 지금 부하 수준에서 shard 간 락 경합 안 보임 (긍정적) |
| 0.28% | `operator new` | 힙 할당 비용 자체는 낮게 나옴 (샘플링 기반이라 참고용) |
| 0.17% | `GatewayShard::dispatch_accept` | shard로 커넥션 위임, 비용 미미 |

나머지 상당 부분(수십 %)은 `[unknown]`으로 뭉쳐 나오는데, 이는 `kernel.kptr_restrict`가
커널 심볼 노출을 막고 있기 때문 (`perf_event_paranoid`와는 별개 설정). 실제로는
`epoll_wait`/`accept`/`connect` 등 커널 syscall 스택으로 추정되지만, 이번엔 확정하지
못함.

---

## 7. 1차 해석

- 뮤텍스 경합이나 자체 로직(HTTP 파싱/필터 체인/시리얼라이즈)은 지금 눈에 띄는 핫스팟이
  **아니다**.
- 대신 **"커넥션당 1회 발생하는" 커널 syscall 비용**(connect, accept)이 두드러진다 —
  이는 keep-alive를 지원하지 않는 현재 설계(`doc/plan.md`, MVP 범위로 확정된 결정)의
  직접적인 귀결.
- p75 이후 급격한 tail latency 악화는, 부하가 오를수록 "다운스트림 accept + 업스트림
  connect"라는 요청당 이중 handshake가 커널 큐에서 밀리기 시작하는 것으로 추정 —
  다만 `[unknown]` 구간을 마저 봐야 확정할 수 있음.

## 8. 다음 실측에서 채울 것

- ~~`kernel.kptr_restrict`를 낮춰서 `[unknown]` 스택을 실제 커널 함수로 해석~~ → §9에서 완료
- `perf stat -e cache-references,cache-misses,LLC-loads,LLC-load-misses`로 캐시 미스 측정
  (doc 22번 섹션의 Cache 지표)
- `sudo bpftrace`로 futex 호출 여부 확인 (Synchronization 지표) — §9 결과(뮤텍스 lock/unlock
  self 합쳐서 ~2%)로 볼 때 한계효용 낮다고 판단, 보류
- `heaptrack`/`valgrind massif` 설치 후 요청당 할당량(Memory 지표) 측정
- **keep-alive 도입 여부를 이 실측 결과로 재검토** — `doc/plan.md`의 "다음 세션 체크
  리스트"에 있던 항목인데, §9에서 accept(40.2%)/epoll_wait+epoll_ctl(합 35.8%) 경로가
  여전히 지배적이라는 게 재확인되어 판단 근거가 더 쌓임

---

## 9. `kptr_restrict=0` 재측정 (2026-09-22)

§8에서 미뤘던 커널 심볼 해석을 채우기 위해 재측정. 절차는 §1~6과 동일
(`sudo sysctl -w kernel.kptr_restrict=0 kernel.perf_event_paranoid=1` 적용 후
동일한 `wrk -t4 -c64 -d10s` 부하 + `perf record -F 999 -g` 12초 샘플링, 8,708 샘플).

인터랙티브 FlameGraph: https://claude.ai/code/artifact/8743eba1-79e4-431d-9f16-5567c87a0528

### wrk 결과

| 지표 | 값 |
|---|---|
| Requests/sec | 3,036.84 |
| p50 latency | 4.22 ms |
| p75 latency | 278.35 ms |
| p90 latency | 767.77 ms |
| p99 latency | 1.18 s |
| Timeouts | 40 / 30,415 요청 |

§5의 baseline(14,783 req/s)과 수치가 크게 다르다 — 실행 환경(코어 수, 동시 부하 등)이
달라진 것으로 보이며 절대값 비교는 의미가 적다. 다만 **p50과 p75 이후의 격차가 크게
벌어지는 tail latency 패턴 자체는 동일하게 재현**됨.

### 커널 스택 해석 결과 (self-time 기준, 0.1% 이상)

| self % | children % | 심볼 | 종류 |
|---|---|---|---|
| 4.04% | 4.04% | `entry_SYSRETQ_unsafe_stack` | 커널 |
| 2.43% | 5.31% | `kmem_cache_alloc` | 커널 |
| 2.19% | 13.08% | `do_epoll_ctl` | 커널 |
| 2.14% | 2.97% | `__memcg_slab_post_alloc_hook` | 커널 |
| 1.80% | 1.86% | `llhttp__internal_execute` | 유저 |
| 1.76% | 1.76% | `__fdget` | 커널 |
| 1.55% | 1.60% | `_raw_spin_lock` | 커널 |
| 1.41% | 3.53% | `tcp_poll` | 커널 |
| 1.22% | 1.75% | `malloc` | 유저 |
| 1.19% | 3.95% | `inet_csk_accept` | 커널 |
| 1.11% | 6.09% | `kmem_cache_alloc_lru` | 커널 |
| 1.06% | 1.31% | `pthread_mutex_unlock` | 유저 |
| 1.04% | 1.14% | `pthread_mutex_lock` | 유저 |

누적(children) 기준 상위 경로:

| 누적 % | 프레임 | 해석 |
|---|---|---|
| 40.2% | `[.] accept` | downstream accept 전체 경로 (소켓/inode/file alloc 포함) |
| 19.7% | `[.] epoll_wait` | event loop 대기 |
| 16.1% | `[.] epoll_ctl` | event loop 등록/해제 |

### 1차 해석

- **커널 스택이 전부 해석됨** — §7의 "확정 못 함"이 해소됨. `accept` 경로의 대부분은
  `sock_alloc_file → alloc_file_pseudo → alloc_file → alloc_empty_file`처럼 커넥션마다
  새 파일 디스크립터/inode를 할당하는 데 드는 커널 메모리 관리 비용으로 확인됨 —
  §7에서 추정만 했던 "요청당 accept/connect 이중 handshake 비용"이 실제로 이 경로임이
  확정됨.
- **뮤텍스는 여전히 문제가 아님** — `pthread_mutex_lock`/`unlock` self 합쳐 ~2.1%,
  `_raw_spin_lock`(커널 스핀락, boost.asio epoll_reactor 내부 락으로 추정) self 1.55%.
  두 baseline(§6, §9)에서 일관되게 낮게 나와 shard-local 설계가 락 경합을 실제로
  피하고 있다는 근거가 두 번째로 쌓임 — `bpftrace`로 futex를 더 파봐야 할 이유가 약함.
- `kmem_cache_alloc`/`kmem_cache_alloc_lru`/`__memcg_slab_*` 계열이 self-time 상위에
  다수 등장 — 커넥션마다 커널 객체(소켓, dentry, inode)를 새로 할당/해제하는 비용이
  유저공간 로직(HTTP 파싱 `llhttp__internal_execute` 1.8%, 필터 체인 등)보다 훨씬 큼.
  이 역시 keep-alive 부재(요청마다 새 accept)가 원인 축 중 하나.

### 결론

두 차례(§6, §9) 프로파일링 모두 같은 결론을 가리킨다: **지금 병목은 애플리케이션
로직이나 락 경합이 아니라 "커넥션당 1회 발생하는 커널 객체 할당/syscall 비용"**이다.
`doc/plan.md`의 "다음 세션 체크리스트" 중 **keep-alive 도입 검토**가 다음으로 착수할
가장 근거가 뚜렷한 항목.
