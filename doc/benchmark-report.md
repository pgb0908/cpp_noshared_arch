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

- `kernel.kptr_restrict`를 낮춰서 `[unknown]` 스택을 실제 커널 함수로 해석
- `perf stat -e cache-references,cache-misses,LLC-loads,LLC-load-misses`로 캐시 미스 측정
  (doc 22번 섹션의 Cache 지표)
- `sudo bpftrace`로 futex 호출 여부 확인 (Synchronization 지표)
- `heaptrack`/`valgrind massif` 설치 후 요청당 할당량(Memory 지표) 측정
- **keep-alive 도입 여부를 이 실측 결과로 재검토** — `doc/plan.md`의 "다음 세션 체크
  리스트"에 있던 항목인데, 이번 baseline이 그 판단 근거가 될 첫 데이터가 됨
