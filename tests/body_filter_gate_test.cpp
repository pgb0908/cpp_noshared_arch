#include "filter/body_filter_gate.hpp"

#include <gtest/gtest.h>

namespace {

// 항상 kStopIterationAndBuffer를 반환하는 필터 -- end_stream이 와도
// 계속 버퍼링을 요청하는 "계약 위반" 필터를 흉내낸다 (BodyFilterGate가
// 이런 필터 하나 때문에 죽지 않고 방어적으로 처리하는지 검증하기 위함).
class AlwaysBufferFilter : public IFilter {
public:
    FilterDataResult on_request_data(std::string&, bool, FilterContext&) override {
        return FilterDataResult::stop_and_buffer();
    }
    FilterDataResult on_response_data(std::string&, bool, FilterContext&) override {
        return FilterDataResult::stop_and_buffer();
    }
};

// end_stream이 오기 전까지만 버퍼링하고, end_stream엔 정상적으로
// continue하는 "계약을 지키는" 필터.
class BufferUntilEndFilter : public IFilter {
public:
    FilterDataResult on_request_data(std::string&, bool end_stream, FilterContext&) override {
        return end_stream ? FilterDataResult::continue_() : FilterDataResult::stop_and_buffer();
    }
    FilterDataResult on_response_data(std::string&, bool end_stream, FilterContext&) override {
        return end_stream ? FilterDataResult::continue_() : FilterDataResult::stop_and_buffer();
    }
};

class RejectFilter : public IFilter {
public:
    FilterDataResult on_request_data(std::string&, bool, FilterContext&) override {
        net::http::ResponseHead resp;
        resp.status = 400;
        return FilterDataResult::respond(resp, "nope");
    }
};

}  // namespace

TEST(BodyFilterGate, 필터가_없으면_그대로_통과한다) {
    FilterChain chain;
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/1024);

    const BodyFilterGate::Result result = gate.apply_request("hello", true);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kForward);
    EXPECT_EQ(result.data, "hello");
}

TEST(BodyFilterGate, 필터가_버퍼링을_요청하면_watermark_이내에선_계속_기다린다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<BufferUntilEndFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/1024);

    const BodyFilterGate::Result result = gate.apply_request("small chunk", /*end_stream=*/false);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kStillBuffering);
}

TEST(BodyFilterGate, 버퍼가_watermark를_넘으면_요청은_413으로_거부한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<BufferUntilEndFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/8);

    const BodyFilterGate::Result result = gate.apply_request("this is way more than 8 bytes", /*end_stream=*/false);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kReject);
    EXPECT_EQ(result.reject_head.status, 413u);
}

TEST(BodyFilterGate, 버퍼가_watermark를_넘으면_응답은_502로_거부한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<BufferUntilEndFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/8);

    const BodyFilterGate::Result result = gate.apply_response("this is way more than 8 bytes", /*end_stream=*/false);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kReject);
    EXPECT_EQ(result.reject_head.status, 502u);
}

// 회귀 테스트: 실측 중 발견했던 버그 -- 작은 메시지가 소켓 read 한 번에
// 통째로 들어와서 end_stream=true인 게 유일한 호출인 경우, watermark
// 체크가 뒤늦게(end_stream 처리보다 나중에) 실행되면 초과분이 그냥
// 통과해버렸다. 지금은 watermark 체크가 항상 먼저이므로 이 케이스도
// 정확히 거부돼야 한다.
TEST(BodyFilterGate, 단일_청크_end_stream_요청도_watermark_초과면_거부된다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<AlwaysBufferFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/8);

    // 첫 호출이자 유일한 호출이 곧 end_stream=true.
    const BodyFilterGate::Result result = gate.apply_request("this exceeds the tiny watermark", /*end_stream=*/true);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kReject);
    EXPECT_EQ(result.reject_head.status, 413u);
}

// 회귀 테스트: 실측 중 발견했던 버그 -- 필터가 end_stream 청크에서도
// kStopIterationAndBuffer를 반환하면(계약 위반) 예전엔 assert()로 프로세스
// 전체가 죽었다. 지금은 watermark 이내인 한 방어적으로 강제
// forward해야 한다 (죽지 않아야 함).
TEST(BodyFilterGate, 필터가_end_stream에서도_버퍼링을_요청하면_watermark_이내라면_강제로_통과시킨다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<AlwaysBufferFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/1024);

    const BodyFilterGate::Result result = gate.apply_request("tiny", /*end_stream=*/true);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kForward);
    EXPECT_EQ(result.data, "tiny");
}

TEST(BodyFilterGate, 필터가_거부하면_즉시_reject를_반환한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<RejectFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/1024);

    const BodyFilterGate::Result result = gate.apply_request("payload", false);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kReject);
    EXPECT_EQ(result.reject_head.status, 400u);
    EXPECT_EQ(result.reject_body, "nope");
}

TEST(BodyFilterGate, 여러_청크에_걸쳐_누적되다가_end_stream에서_한번에_통과한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<BufferUntilEndFilter>());
    FilterContext ctx;
    BodyFilterGate gate(chain, ctx, /*high_watermark_bytes=*/1024);

    EXPECT_EQ(gate.apply_request("ab", false).outcome, BodyFilterGate::Outcome::kStillBuffering);
    EXPECT_EQ(gate.apply_request("cd", false).outcome, BodyFilterGate::Outcome::kStillBuffering);

    const BodyFilterGate::Result result = gate.apply_request("ef", true);

    EXPECT_EQ(result.outcome, BodyFilterGate::Outcome::kForward);
    EXPECT_EQ(result.data, "abcdef");
}
