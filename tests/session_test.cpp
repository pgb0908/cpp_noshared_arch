#include "session/session.hpp"

#include <gtest/gtest.h>

#include <string>

#include "fakes/fake_event_loop.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

namespace {

std::string to_string(const std::vector<char>& data) { return std::string(data.begin(), data.end()); }

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.upstreams = {{"host-a", 8000}};
        config_.connect_timeout_seconds = 5;
        config_.connection_pool_max_idle_per_endpoint = 2;
        config_.dns_refresh_interval_seconds = 30;
        config_.buffer_size = 64;

        manager_ = std::make_unique<UpstreamManager>(loop_, config_);
        manager_->start();

        loop_.last_resolver()->set_result_for("host-a", net::Error::none(), {net::Endpoint{"10.0.0.1", 8000}});
        FakeTimer* dns_timer = loop_.created_timers().back();
        dns_timer->fire();
        loop_.pump();

        buffer_pool_ = std::make_unique<BufferPool>(config_.buffer_size, 8);
    }

    // downstream 소켓을 만들고 Session을 시작해서, upstream connect까지
    // 완료된 상태로 만들어준다. 반환값: (session, downstream 원본 포인터,
    // upstream 원본 포인터).
    struct Established {
        std::shared_ptr<Session> session;
        FakeSocket* downstream;
        FakeSocket* upstream;
    };

    Established establish_session() {
        auto downstream = std::make_unique<FakeSocket>(loop_);
        FakeSocket* downstream_raw = downstream.get();

        auto session =
            std::make_shared<Session>(std::move(downstream), loop_, *manager_, *buffer_pool_, metrics_);
        session->start();

        // downstream은 loop_.create_socket()을 거치지 않고 직접
        // 만들었으므로, 이 시점의 created_sockets()엔 upstream 것만 있다.
        FakeSocket* upstream_raw = loop_.created_sockets().back();
        upstream_raw->complete_connect(net::Error::none());
        loop_.pump();

        return {session, downstream_raw, upstream_raw};
    }

    Config config_;
    FakeEventLoop loop_;
    std::unique_ptr<UpstreamManager> manager_;
    std::unique_ptr<BufferPool> buffer_pool_;
    LocalMetrics metrics_;
};

}  // namespace

TEST_F(SessionTest, start_하면_upstream에_connect하고_양방향_read가_걸린다) {
    auto [session, downstream, upstream] = establish_session();

    EXPECT_TRUE(downstream->has_pending_read());
    EXPECT_TRUE(upstream->has_pending_read());
    EXPECT_EQ(metrics_.connections_accepted.load(), 1u);
    EXPECT_EQ(metrics_.connections_active.load(), 1u);
}

TEST_F(SessionTest, downstream에서_온_데이터를_upstream으로_relay한다) {
    auto [session, downstream, upstream] = establish_session();

    downstream->deliver_read("hello");
    loop_.pump();

    EXPECT_EQ(to_string(upstream->written()), "hello");
    EXPECT_EQ(metrics_.bytes_downstream_to_upstream.load(), 5u);
    EXPECT_TRUE(downstream->has_pending_read());  // relay 루프가 재무장됨
}

TEST_F(SessionTest, upstream에서_온_데이터를_downstream으로_relay한다) {
    auto [session, downstream, upstream] = establish_session();

    upstream->deliver_read("world!");
    loop_.pump();

    EXPECT_EQ(to_string(downstream->written()), "world!");
    EXPECT_EQ(metrics_.bytes_upstream_to_downstream.load(), 6u);
    EXPECT_TRUE(upstream->has_pending_read());
}

TEST_F(SessionTest, downstream이_끊기면_healthy한_upstream은_풀로_반납된다) {
    auto [session, downstream, upstream] = establish_session();

    downstream->fail_read(net::Error{1, "eof"});
    loop_.pump();

    EXPECT_TRUE(downstream->closed());
    EXPECT_TRUE(downstream->shutdown_called());

    // upstream은 닫히지 않고, pending read만 취소된 뒤 풀로 반납돼야 함.
    EXPECT_TRUE(upstream->cancel_called());
    EXPECT_FALSE(upstream->closed());

    EXPECT_EQ(metrics_.connections_active.load(), 0u);
    EXPECT_EQ(metrics_.connections_closed.load(), 1u);

    // 버퍼 두 개(다운/업 방향)가 풀에 반납됐는지 확인.
    EXPECT_EQ(buffer_pool_->free_count(), 2u);

    // 실제로 풀에서 재사용되는지 확인.
    std::unique_ptr<net::ISocket> reused;
    manager_->acquire_connection(0, [&](const net::Error&, std::unique_ptr<net::ISocket> s) { reused = std::move(s); });
    EXPECT_EQ(reused.get(), upstream);
    EXPECT_EQ(loop_.created_sockets().size(), 1u);  // 새로 connect하지 않았음
}

TEST_F(SessionTest, upstream이_끊기면_downstream도_닫히고_upstream은_풀에_안_들어간다) {
    auto [session, downstream, upstream] = establish_session();

    upstream->fail_read(net::Error{1, "connection reset"});
    loop_.pump();

    EXPECT_TRUE(downstream->closed());
    EXPECT_TRUE(upstream->closed());  // unhealthy라 풀로 안 가고 그냥 닫힘

    EXPECT_EQ(metrics_.connections_active.load(), 0u);
    EXPECT_EQ(metrics_.connections_closed.load(), 1u);

    // 풀이 비어있어야 하므로, 다음 acquire는 새로 connect해야 함.
    manager_->acquire_connection(0, [](const net::Error&, std::unique_ptr<net::ISocket>) {});
    EXPECT_EQ(loop_.created_sockets().size(), 2u);  // upstream(1) + 새 connect(1)
}

TEST_F(SessionTest, upstream_write가_실패하면_upstream은_풀에_안_들어간다) {
    auto [session, downstream, upstream] = establish_session();

    upstream->set_next_write_error(net::Error{1, "broken pipe"});
    downstream->deliver_read("hello");
    loop_.pump();

    EXPECT_TRUE(upstream->closed());  // write 실패 -- unhealthy로 처리
    EXPECT_TRUE(downstream->closed());
}

TEST_F(SessionTest, upstream_connect가_실패하면_downstream도_닫히고_에러_카운터가_증가한다) {
    auto downstream = std::make_unique<FakeSocket>(loop_);
    FakeSocket* downstream_raw = downstream.get();

    auto session = std::make_shared<Session>(std::move(downstream), loop_, *manager_, *buffer_pool_, metrics_);
    session->start();

    FakeSocket* upstream_raw = loop_.created_sockets().back();
    upstream_raw->complete_connect(net::Error{1, "connection refused"});
    loop_.pump();

    EXPECT_TRUE(downstream_raw->closed());
    EXPECT_EQ(metrics_.upstream_connect_errors.load(), 1u);
    EXPECT_EQ(metrics_.connections_active.load(), 0u);
}
