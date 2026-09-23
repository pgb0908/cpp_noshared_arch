#include "net/race_with_timeout.hpp"

#include <gtest/gtest.h>

#include "fakes/fake_event_loop.hpp"

namespace {

class RaceWithTimeoutTest : public ::testing::Test {
protected:
    // start_op를 호출하면 그 완료 콜백을 여기 저장해둔다 -- 테스트가 나중에
    // op_done_(...)을 직접 불러서 "작업이 지금 끝났다"를 흉내낸다.
    net::ErrorCallback op_done_;
    int start_op_calls_ = 0;
    int cancel_op_calls_ = 0;
    std::vector<net::Error> on_done_results_;

    net::StartRacingOp make_start_op() {
        return [this](net::ErrorCallback cb) {
            ++start_op_calls_;
            op_done_ = std::move(cb);
        };
    }

    net::VoidCallback make_cancel_op() {
        return [this] { ++cancel_op_calls_; };
    }

    net::ErrorCallback make_on_done() {
        return [this](const net::Error& err) { on_done_results_.push_back(err); };
    }

    FakeEventLoop loop_;
};

}  // namespace

TEST_F(RaceWithTimeoutTest, 작업이_먼저_끝나면_그_결과로_완료되고_취소는_안_불린다) {
    net::race_with_timeout(loop_, std::chrono::seconds(5), make_start_op(), make_cancel_op(), make_on_done());
    ASSERT_EQ(start_op_calls_, 1);

    op_done_(net::Error::none());
    loop_.pump();

    ASSERT_EQ(on_done_results_.size(), 1u);
    EXPECT_TRUE(on_done_results_[0].ok());
    EXPECT_EQ(cancel_op_calls_, 0);
}

TEST_F(RaceWithTimeoutTest, 타임아웃이_먼저_끝나면_취소되고_타임아웃_에러로_완료된다) {
    net::race_with_timeout(loop_, std::chrono::seconds(5), make_start_op(), make_cancel_op(), make_on_done());

    ASSERT_EQ(loop_.created_timers().size(), 1u);
    FakeTimer* timer = loop_.created_timers().back();
    EXPECT_EQ(timer->last_duration(), std::chrono::seconds(5));

    timer->fire();
    loop_.pump();

    ASSERT_EQ(on_done_results_.size(), 1u);
    EXPECT_FALSE(on_done_results_[0].ok());
    EXPECT_EQ(cancel_op_calls_, 1);
}

TEST_F(RaceWithTimeoutTest, 타임아웃_이후_뒤늦게_작업이_끝나도_완료_콜백은_한_번만_불린다) {
    net::race_with_timeout(loop_, std::chrono::seconds(5), make_start_op(), make_cancel_op(), make_on_done());
    FakeTimer* timer = loop_.created_timers().back();

    timer->fire();
    loop_.pump();
    ASSERT_EQ(on_done_results_.size(), 1u);

    // 타임아웃이 이미 이긴 뒤에 작업이 뒤늦게 성공해도 무시돼야 함.
    op_done_(net::Error::none());
    loop_.pump();

    EXPECT_EQ(on_done_results_.size(), 1u);
}

TEST_F(RaceWithTimeoutTest, 작업이_이긴_뒤에_타이머가_뒤늦게_fire돼도_완료_콜백은_한_번만_불린다) {
    net::race_with_timeout(loop_, std::chrono::seconds(5), make_start_op(), make_cancel_op(), make_on_done());
    FakeTimer* timer = loop_.created_timers().back();

    op_done_(net::Error::none());
    loop_.pump();
    ASSERT_EQ(on_done_results_.size(), 1u);

    // cancel()된 타이머라 실제로는 fire()해도 콜백이 없어야 하지만,
    // 방어적으로 done 플래그 자체도 확인.
    timer->fire();
    loop_.pump();

    EXPECT_EQ(on_done_results_.size(), 1u);
}
