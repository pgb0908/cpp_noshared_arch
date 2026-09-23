#pragma once

#include <cstddef>
#include <vector>

// P-core/E-core처럼 성능이 다른 코어가 섞인 이기종(hybrid) CPU에서, shard를
// 배정할 논리 CPU 번호를 "성능 높은 코어부터" 순서로 돌려준다 (Phase 5,
// doc/plan.md 참고). hwloc의 cpukinds API로 감지하며, 이 API가 아무 정보도
// 못 찾으면(동종 CPU이거나 OS가 코어 종류를 노출 안 하는 경우) 그냥
// 0..N-1을 순서대로 돌려줘서 기존 동작(Phase 1의 순진한 매핑)과 동일하게
// 동작한다.
struct ShardCpuPlan {
    // 성능 높은 코어부터 정렬된 논리 CPU 번호 목록.
    std::vector<int> cpus;
    // cpus[0 .. top_tier_count)가 가장 성능이 높은 그룹(P-core)의 개수.
    // 이기종 정보가 없으면 cpus.size()와 같다(전부 "top tier"로 취급 --
    // 경고를 찍을 이유가 없다는 뜻).
    std::size_t top_tier_count = 0;
};

ShardCpuPlan build_shard_cpu_plan();
