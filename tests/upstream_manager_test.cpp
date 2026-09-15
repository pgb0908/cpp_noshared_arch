#include "upstream/upstream_manager.hpp"

#include <gtest/gtest.h>

#include "fakes/fake_event_loop.hpp"

namespace {

class UpstreamManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.upstreams = {{"host-a", 8000}, {"host-b", 8001}};
        config_.connect_timeout_seconds = 5;
        config_.connection_pool_max_idle_per_endpoint = 2;
        config_.dns_refresh_interval_seconds = 30;

        manager_ = std::make_unique<UpstreamManager>(loop_, config_);
        manager_->start();

        // start()의 최초 resolve_all()은 FakeResolver가 아직 아무것도
        // 설정 안 된 상태(빈 결과)라 실패로 끝난다. 실제 주소를 채우기
        // 위해 resolver 결과를 지금 설정한 뒤, DNS refresh timer를
        // 강제로 fire()시켜 재조회를 유도한다.
        loop_.last_resolver()->set_result_for("host-a", net::Error::none(), {net::Endpoint{"10.0.0.1", 8000}});
        loop_.last_resolver()->set_result_for("host-b", net::Error::none(), {net::Endpoint{"10.0.0.2", 8001}});

        ASSERT_EQ(loop_.created_timers().size(), 1u);  // 이 시점엔 DNS refresh timer 하나뿐
        FakeTimer* dns_timer = loop_.created_timers().back();
        dns_timer->fire();
        loop_.pump();
    }

    Config config_;
    FakeEventLoop loop_;
    std::unique_ptr<UpstreamManager> manager_;
};

}  // namespace

TEST_F(UpstreamManagerTest, select_endpoint은_round_robin으로_순환한다) {
    EXPECT_EQ(manager_->select_endpoint(), 0u);
    EXPECT_EQ(manager_->select_endpoint(), 1u);
    EXPECT_EQ(manager_->select_endpoint(), 0u);  // 다시 처음으로
    EXPECT_EQ(manager_->select_endpoint(), 1u);
}

TEST_F(UpstreamManagerTest, pool이_비어있으면_새로_connect하고_성공하면_소켓을_전달한다) {
    bool called = false;
    net::Error result_err;
    std::unique_ptr<net::ISocket> result_sock;
    manager_->acquire_connection(0, [&](const net::Error& err, std::unique_ptr<net::ISocket> sock) {
        called = true;
        result_err = err;
        result_sock = std::move(sock);
    });

    ASSERT_EQ(loop_.created_sockets().size(), 1u);
    FakeSocket* sock = loop_.created_sockets().back();
    EXPECT_TRUE(sock->has_pending_connect());
    EXPECT_EQ(sock->last_connect_endpoint().host, "10.0.0.1");
    EXPECT_FALSE(called);  // connect 완료 전이라 아직 콜백 안 불림

    sock->complete_connect(net::Error::none());
    loop_.pump();

    EXPECT_TRUE(called);
    EXPECT_TRUE(result_err.ok());
    EXPECT_EQ(result_sock.get(), sock);
}

TEST_F(UpstreamManagerTest, connect가_timeout보다_먼저_끝나면_timer가_취소된다) {
    manager_->acquire_connection(0, [](const net::Error&, std::unique_ptr<net::ISocket>) {});
    FakeSocket* sock = loop_.created_sockets().back();
    ASSERT_GE(loop_.created_timers().size(), 2u);  // dns timer + connect timeout timer
    FakeTimer* connect_timer = loop_.created_timers().back();

    sock->complete_connect(net::Error::none());
    loop_.pump();

    EXPECT_FALSE(connect_timer->has_pending());  // connect가 이겨서 timer가 cancel됨
    EXPECT_FALSE(sock->cancel_called());
}

TEST_F(UpstreamManagerTest, timeout이_connect보다_먼저_끝나면_에러_콜백과_함께_소켓이_취소된다) {
    bool called = false;
    net::Error result_err;
    std::unique_ptr<net::ISocket> result_sock;
    manager_->acquire_connection(0, [&](const net::Error& err, std::unique_ptr<net::ISocket> sock) {
        called = true;
        result_err = err;
        result_sock = std::move(sock);
    });
    FakeSocket* sock = loop_.created_sockets().back();
    FakeTimer* connect_timer = loop_.created_timers().back();

    connect_timer->fire();  // 타임아웃 발생
    loop_.pump();

    EXPECT_TRUE(called);
    EXPECT_FALSE(result_err.ok());
    EXPECT_EQ(result_sock, nullptr);
    EXPECT_TRUE(sock->cancel_called());
}

TEST_F(UpstreamManagerTest, timeout_이후_뒤늦게_connect가_성공해도_콜백은_한_번만_불린다) {
    int call_count = 0;
    manager_->acquire_connection(0, [&](const net::Error&, std::unique_ptr<net::ISocket>) { ++call_count; });
    FakeSocket* sock = loop_.created_sockets().back();
    FakeTimer* connect_timer = loop_.created_timers().back();

    connect_timer->fire();
    loop_.pump();
    EXPECT_EQ(call_count, 1);

    sock->complete_connect(net::Error::none());  // 뒤늦은 connect 완료 -- done 플래그로 무시돼야 함
    loop_.pump();
    EXPECT_EQ(call_count, 1);  // 여전히 1번만
}

TEST_F(UpstreamManagerTest, release한_connection은_다음_acquire에서_재사용된다) {
    std::unique_ptr<net::ISocket> sock1;
    manager_->acquire_connection(0, [&](const net::Error&, std::unique_ptr<net::ISocket> s) { sock1 = std::move(s); });
    FakeSocket* raw = loop_.created_sockets().back();
    raw->complete_connect(net::Error::none());
    loop_.pump();
    ASSERT_NE(sock1, nullptr);

    manager_->release_connection(0, std::move(sock1));

    bool called = false;
    std::unique_ptr<net::ISocket> sock2;
    manager_->acquire_connection(0, [&](const net::Error&, std::unique_ptr<net::ISocket> s) {
        called = true;
        sock2 = std::move(s);
    });

    EXPECT_TRUE(called);           // 풀 히트는 콜백이 동기적으로 실행됨
    EXPECT_EQ(sock2.get(), raw);   // 같은 소켓을 재사용
    EXPECT_EQ(loop_.created_sockets().size(), 1u);  // 새로 만들지 않았음
}

TEST_F(UpstreamManagerTest, pool_상한을_넘는_release는_소켓을_닫는다) {
    std::vector<std::unique_ptr<net::ISocket>> sockets;
    for (int i = 0; i < 3; ++i) {
        std::unique_ptr<net::ISocket> s;
        manager_->acquire_connection(
            0, [&](const net::Error&, std::unique_ptr<net::ISocket> sock) { s = std::move(sock); });
        FakeSocket* raw = loop_.created_sockets().back();
        raw->complete_connect(net::Error::none());
        loop_.pump();
        sockets.push_back(std::move(s));
    }

    auto* third_raw = static_cast<FakeSocket*>(sockets[2].get());

    manager_->release_connection(0, std::move(sockets[0]));
    manager_->release_connection(0, std::move(sockets[1]));
    EXPECT_FALSE(third_raw->closed());
    manager_->release_connection(0, std::move(sockets[2]));  // 상한(2) 초과

    EXPECT_TRUE(third_raw->closed());
}
