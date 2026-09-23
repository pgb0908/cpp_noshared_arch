#include "util/cpu_topology.hpp"

#include <hwloc.h>

#include <algorithm>
#include <thread>

namespace {

// hwloc이 코어 종류를 못 찾았을 때(동종 CPU, 또는 OS가 정보를 노출 안 함)
// 쓰는 대체 경로 -- Phase 1 이전의 순진한 0..N-1 매핑과 동일하다.
ShardCpuPlan homogeneous_fallback() {
    ShardCpuPlan plan;
    const unsigned n = std::thread::hardware_concurrency();
    plan.cpus.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        plan.cpus.push_back(static_cast<int>(i));
    }
    plan.top_tier_count = plan.cpus.size();
    return plan;
}

}  // namespace

ShardCpuPlan build_shard_cpu_plan() {
    hwloc_topology_t topology;
    if (hwloc_topology_init(&topology) != 0) {
        return homogeneous_fallback();
    }
    if (hwloc_topology_load(topology) != 0) {
        hwloc_topology_destroy(topology);
        return homogeneous_fallback();
    }

    const int nr_kinds = hwloc_cpukinds_get_nr(topology, 0);
    if (nr_kinds <= 0) {
        // 0: 코어 종류 정보 없음(동종 CPU 또는 미지원 OS). -1: 잘못된 flags
        // (여긴 항상 0을 넘기므로 도달 안 함) -- 둘 다 fallback으로 처리.
        hwloc_topology_destroy(topology);
        return homogeneous_fallback();
    }

    // hwloc 문서(cpukinds.h): kind_index가 낮을수록 efficiency가 낮다
    // (E-core), 높을수록 efficiency가 높다(P-core, "power-hungry
    // high-performance"). 그래서 kind_index를 그대로 내림차순 정렬하면
    // P-core가 먼저 오는 순서가 된다 -- efficiency 값 자체를 다시 읽을
    // 필요 없이 kind_index 순서만 뒤집으면 충분하다.
    struct KindCpus {
        std::vector<int> cpus;
    };
    std::vector<KindCpus> kinds(static_cast<std::size_t>(nr_kinds));

    for (int i = 0; i < nr_kinds; ++i) {
        hwloc_bitmap_t cpuset = hwloc_bitmap_alloc();
        int efficiency = -1;
        unsigned nr_infos = 0;
        struct hwloc_info_s* infos = nullptr;
        if (hwloc_cpukinds_get_info(topology, static_cast<unsigned>(i), cpuset, &efficiency, &nr_infos, &infos, 0) !=
            0) {
            hwloc_bitmap_free(cpuset);
            continue;
        }
        int cpu;
        hwloc_bitmap_foreach_begin(cpu, cpuset) { kinds[static_cast<std::size_t>(i)].cpus.push_back(cpu); }
        hwloc_bitmap_foreach_end();
        hwloc_bitmap_free(cpuset);
    }

    hwloc_topology_destroy(topology);

    ShardCpuPlan plan;
    // kind_index가 가장 큰 것(가장 높은 efficiency = P-core)부터.
    for (int i = nr_kinds - 1; i >= 0; --i) {
        std::sort(kinds[static_cast<std::size_t>(i)].cpus.begin(), kinds[static_cast<std::size_t>(i)].cpus.end());
        for (int cpu : kinds[static_cast<std::size_t>(i)].cpus) {
            plan.cpus.push_back(cpu);
        }
        if (i == nr_kinds - 1) {
            plan.top_tier_count = plan.cpus.size();
        }
    }

    if (plan.cpus.empty()) {
        return homogeneous_fallback();
    }
    return plan;
}
