#include "util/cpu_topology.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

// hwloc이 실제 하드웨어에 의존하므로(P-core/E-core 구성은 머신마다 다름),
// "이 머신에서 P-core가 정확히 N개다" 같은 값 자체는 검증할 수 없다. 대신
// build_shard_cpu_plan()이 어떤 하드웨어에서 실행되든 지켜야 하는 불변조건만
// 확인한다.
TEST(CpuTopology, 결과가_비어있지_않다) {
    const ShardCpuPlan plan = build_shard_cpu_plan();
    EXPECT_FALSE(plan.cpus.empty());
}

TEST(CpuTopology, cpu_번호가_중복되지_않는다) {
    const ShardCpuPlan plan = build_shard_cpu_plan();
    const std::set<int> unique_cpus(plan.cpus.begin(), plan.cpus.end());
    EXPECT_EQ(unique_cpus.size(), plan.cpus.size());
}

TEST(CpuTopology, cpu_번호는_모두_0_이상이다) {
    const ShardCpuPlan plan = build_shard_cpu_plan();
    EXPECT_TRUE(std::all_of(plan.cpus.begin(), plan.cpus.end(), [](int cpu) { return cpu >= 0; }));
}

TEST(CpuTopology, top_tier_count는_1_이상_전체_이하다) {
    const ShardCpuPlan plan = build_shard_cpu_plan();
    EXPECT_GE(plan.top_tier_count, 1u);
    EXPECT_LE(plan.top_tier_count, plan.cpus.size());
}
