#include "util/local_metrics.hpp"

#include <gtest/gtest.h>

TEST(LocalMetrics, 초기값은_모두_0이다) {
    LocalMetrics m;
    EXPECT_EQ(m.connections_accepted.load(), 0u);
    EXPECT_EQ(m.connections_active.load(), 0u);
    EXPECT_EQ(m.connections_closed.load(), 0u);
    EXPECT_EQ(m.bytes_downstream_to_upstream.load(), 0u);
    EXPECT_EQ(m.bytes_upstream_to_downstream.load(), 0u);
    EXPECT_EQ(m.upstream_connect_errors.load(), 0u);
}

TEST(LocalMetrics, on_accept은_accepted와_active를_함께_증가시킨다) {
    LocalMetrics m;
    m.on_accept();
    EXPECT_EQ(m.connections_accepted.load(), 1u);
    EXPECT_EQ(m.connections_active.load(), 1u);

    m.on_accept();
    EXPECT_EQ(m.connections_accepted.load(), 2u);
    EXPECT_EQ(m.connections_active.load(), 2u);
}

TEST(LocalMetrics, on_close는_closed를_증가시키고_active를_감소시킨다) {
    LocalMetrics m;
    m.on_accept();
    m.on_accept();
    m.on_close();

    EXPECT_EQ(m.connections_accepted.load(), 2u);
    EXPECT_EQ(m.connections_active.load(), 1u);
    EXPECT_EQ(m.connections_closed.load(), 1u);
}

TEST(LocalMetrics, alignas64로_인해_구조체_크기는_64의_배수다) {
    // shard별 LocalMetrics가 서로 다른 cache line에 놓여 false sharing을
    // 피하기 위한 것 -- 크기가 실제로 64바이트 정렬돼 있는지 확인.
    EXPECT_EQ(sizeof(LocalMetrics) % 64u, 0u);
    EXPECT_EQ(alignof(LocalMetrics), 64u);
}
