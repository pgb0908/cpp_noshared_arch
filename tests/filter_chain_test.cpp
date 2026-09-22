#include "filter/filter_chain.hpp"

#include <gtest/gtest.h>

#include <cctype>

namespace {

// 요청/응답 양쪽에 같은 이름의 헤더를 추가하는 테스트용 필터.
class AddHeaderFilter : public IFilter {
public:
    explicit AddHeaderFilter(std::string name, std::string value) : name_(std::move(name)), value_(std::move(value)) {}

    FilterHeaderResult on_request(net::http::RequestHead& head, FilterContext&) override {
        head.headers.push_back(net::http::Header{name_, value_});
        return FilterHeaderResult::continue_();
    }

    FilterHeaderResult on_response(net::http::ResponseHead& head, FilterContext&) override {
        head.headers.push_back(net::http::Header{name_, value_});
        return FilterHeaderResult::continue_();
    }

private:
    std::string name_;
    std::string value_;
};

// 특정 헤더가 없으면 401로 거부하는 테스트용 인증 필터 (요청만 본다).
class RequireAuthHeaderFilter : public IFilter {
public:
    FilterHeaderResult on_request(net::http::RequestHead& head, FilterContext&) override {
        for (const auto& h : head.headers) {
            if (h.name == "Authorization") {
                return FilterHeaderResult::continue_();
            }
        }
        net::http::ResponseHead resp;
        resp.status = 401;
        resp.reason = "Unauthorized";
        resp.headers.push_back({"Content-Length", "0"});
        return FilterHeaderResult::respond(resp, "");
    }
};

// FilterContext에 값을 심기만 하는 필터 (요청 단계).
const ContextKey<std::string> kTestUserId{"test_user_id"};

class WriteUserIdFilter : public IFilter {
public:
    FilterHeaderResult on_request(net::http::RequestHead&, FilterContext& ctx) override {
        ctx.set(kTestUserId, std::string("alice"));
        return FilterHeaderResult::continue_();
    }
};

// FilterContext에서 값을 읽어 응답 헤더에 반영하는 필터.
class ReadUserIdIntoResponseFilter : public IFilter {
public:
    FilterHeaderResult on_response(net::http::ResponseHead& head, FilterContext& ctx) override {
        std::string* user_id = ctx.get(kTestUserId);
        head.headers.push_back({"X-User-Id", user_id ? *user_id : "missing"});
        return FilterHeaderResult::continue_();
    }
};

// 청크를 대문자로 바꿔서 통과시키는 필터 (스트리밍 -- 버퍼링 안 함).
class UppercaseDataFilter : public IFilter {
public:
    FilterDataResult on_request_data(std::string& data, bool, FilterContext&) override {
        uppercase(data);
        ++call_count;
        return FilterDataResult::continue_();
    }

    FilterDataResult on_response_data(std::string& data, bool, FilterContext&) override {
        uppercase(data);
        ++call_count;
        return FilterDataResult::continue_();
    }

    int call_count = 0;

private:
    static void uppercase(std::string& data) {
        for (char& c : data) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
};

// end_stream이 오기 전까지는 계속 kStopIterationAndBuffer를 반환해서
// 바디 전체를 누적하는 필터.
class BufferUntilEndFilter : public IFilter {
public:
    FilterDataResult on_request_data(std::string& data, bool end_stream, FilterContext&) override {
        ++call_count;
        if (!end_stream) {
            return FilterDataResult::stop_and_buffer();
        }
        return FilterDataResult::continue_();
    }

    int call_count = 0;
};

// 데이터를 보자마자 거부하는 필터.
class RejectOnDataFilter : public IFilter {
public:
    FilterDataResult on_request_data(std::string&, bool, FilterContext&) override {
        net::http::ResponseHead resp;
        resp.status = 400;
        return FilterDataResult::respond(resp, "bad body");
    }
};

}  // namespace

TEST(FilterChain, 필터가_없으면_그대로_continue를_반환한다) {
    FilterChain chain;
    FilterContext ctx;
    net::http::RequestHead head;
    head.method = "GET";

    const FilterHeaderResult result = chain.apply_request(head, ctx);

    EXPECT_EQ(result.action, FilterHeaderAction::kContinue);
    EXPECT_TRUE(head.headers.empty());
}

TEST(FilterChain, 요청은_등록된_순서대로_실행되며_head를_고친다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<AddHeaderFilter>("X-A", "1"));
    chain.add_filter(std::make_unique<AddHeaderFilter>("X-B", "2"));

    FilterContext ctx;
    net::http::RequestHead head;
    const FilterHeaderResult result = chain.apply_request(head, ctx);

    EXPECT_EQ(result.action, FilterHeaderAction::kContinue);
    ASSERT_EQ(head.headers.size(), 2u);
    EXPECT_EQ(head.headers[0].name, "X-A");
    EXPECT_EQ(head.headers[1].name, "X-B");
}

TEST(FilterChain, 응답은_등록_역순으로_실행된다_onion_모델) {
    FilterChain chain;
    chain.add_filter(std::make_unique<AddHeaderFilter>("X-A", "1"));
    chain.add_filter(std::make_unique<AddHeaderFilter>("X-B", "2"));

    FilterContext ctx;
    net::http::ResponseHead head;
    const FilterHeaderResult result = chain.apply_response(head, ctx);

    EXPECT_EQ(result.action, FilterHeaderAction::kContinue);
    ASSERT_EQ(head.headers.size(), 2u);
    // 요청은 A -> B 순으로 등록됐지만, 응답은 역순(B -> A)으로 실행된다.
    EXPECT_EQ(head.headers[0].name, "X-B");
    EXPECT_EQ(head.headers[1].name, "X-A");
}

TEST(FilterChain, 필터가_거부하면_이후_필터는_실행되지_않는다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<RequireAuthHeaderFilter>());
    chain.add_filter(std::make_unique<AddHeaderFilter>("X-Should-Not-Appear", "x"));

    FilterContext ctx;
    net::http::RequestHead head;  // Authorization 헤더 없음
    const FilterHeaderResult result = chain.apply_request(head, ctx);

    EXPECT_EQ(result.action, FilterHeaderAction::kRespondDirectly);
    EXPECT_EQ(result.direct_response.head.status, 401u);
    // 거부한 필터 뒤에 있던 필터는 실행되지 않아야 함 -- head는 그대로.
    EXPECT_TRUE(head.headers.empty());
}

TEST(FilterChain, Authorization_헤더가_있으면_통과한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<RequireAuthHeaderFilter>());

    FilterContext ctx;
    net::http::RequestHead head;
    head.headers.push_back({"Authorization", "Bearer token"});

    const FilterHeaderResult result = chain.apply_request(head, ctx);

    EXPECT_EQ(result.action, FilterHeaderAction::kContinue);
}

TEST(FilterChain, 한_필터가_요청_단계에서_심은_값을_다른_필터가_응답_단계에서_읽는다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<WriteUserIdFilter>());
    chain.add_filter(std::make_unique<ReadUserIdIntoResponseFilter>());

    FilterContext ctx;  // HttpSession이 세션마다 하나씩 소유하는 것과 동일한 역할
    net::http::RequestHead req_head;
    ASSERT_EQ(chain.apply_request(req_head, ctx).action, FilterHeaderAction::kContinue);

    net::http::ResponseHead resp_head;
    ASSERT_EQ(chain.apply_response(resp_head, ctx).action, FilterHeaderAction::kContinue);

    ASSERT_EQ(resp_head.headers.size(), 1u);
    EXPECT_EQ(resp_head.headers[0].name, "X-User-Id");
    EXPECT_EQ(resp_head.headers[0].value, "alice");
}

TEST(FilterChain, context에_값이_없으면_get은_nullptr를_반환한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<ReadUserIdIntoResponseFilter>());

    FilterContext ctx;  // WriteUserIdFilter를 안 거쳤으므로 비어있음
    net::http::ResponseHead head;
    chain.apply_response(head, ctx);

    ASSERT_EQ(head.headers.size(), 1u);
    EXPECT_EQ(head.headers[0].value, "missing");
}

TEST(FilterChainData, 버퍼링_없이_매_청크가_즉시_통과하며_필터가_고칠_수_있다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<UppercaseDataFilter>());

    FilterContext ctx;
    FilterChain::DataIterationState state;

    std::string chunk1 = "hello ";
    ASSERT_EQ(chain.apply_request_data(chunk1, false, state, ctx).action, FilterDataAction::kContinue);
    EXPECT_EQ(chunk1, "HELLO ");
    EXPECT_FALSE(state.blocked);  // 버퍼링 안 했으므로 다음 청크도 바로 흘러감

    std::string chunk2 = "world";
    ASSERT_EQ(chain.apply_request_data(chunk2, true, state, ctx).action, FilterDataAction::kContinue);
    EXPECT_EQ(chunk2, "WORLD");
}

TEST(FilterChainData, 필터가_버퍼링을_요청하면_end_stream까지_뒤_필터로_안_넘어간다) {
    FilterChain chain;
    auto* buffering = new BufferUntilEndFilter();
    auto* uppercase = new UppercaseDataFilter();
    chain.add_filter(std::unique_ptr<IFilter>(buffering));
    chain.add_filter(std::unique_ptr<IFilter>(uppercase));

    FilterContext ctx;
    FilterChain::DataIterationState state;

    std::string chunk1 = "ab";
    ASSERT_EQ(chain.apply_request_data(chunk1, false, state, ctx).action, FilterDataAction::kStopIterationAndBuffer);
    EXPECT_TRUE(state.blocked);
    EXPECT_EQ(uppercase->call_count, 0);  // 뒤 필터는 아직 한 번도 안 불림

    std::string chunk2 = "cd";
    ASSERT_EQ(chain.apply_request_data(chunk2, false, state, ctx).action, FilterDataAction::kStopIterationAndBuffer);
    EXPECT_EQ(buffering->call_count, 2);
    EXPECT_EQ(uppercase->call_count, 0);

    std::string chunk3 = "ef";
    const FilterDataResult result = chain.apply_request_data(chunk3, true, state, ctx);
    ASSERT_EQ(result.action, FilterDataAction::kContinue);
    EXPECT_FALSE(state.blocked);
    // 누적된 전체("abcdef")가 한 번에 뒤 필터로 전달되어 대문자로 바뀐다.
    EXPECT_EQ(chunk3, "ABCDEF");
    EXPECT_EQ(uppercase->call_count, 1);
}

TEST(FilterChainData, 데이터_필터가_거부하면_즉시_응답한다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<RejectOnDataFilter>());

    FilterContext ctx;
    FilterChain::DataIterationState state;
    std::string chunk = "payload";

    const FilterDataResult result = chain.apply_request_data(chunk, false, state, ctx);

    EXPECT_EQ(result.action, FilterDataAction::kRespondDirectly);
    EXPECT_EQ(result.direct_response.head.status, 400u);
}

TEST(FilterChainData, 응답_데이터도_등록_역순으로_실행된다) {
    FilterChain chain;
    chain.add_filter(std::make_unique<AddHeaderFilter>("X-Unused", "1"));  // 헤더만, 데이터엔 영향 없음
    chain.add_filter(std::make_unique<UppercaseDataFilter>());

    FilterContext ctx;
    FilterChain::DataIterationState state;
    std::string chunk = "abc";

    const FilterDataResult result = chain.apply_response_data(chunk, true, state, ctx);

    EXPECT_EQ(result.action, FilterDataAction::kContinue);
    EXPECT_EQ(chunk, "ABC");
}
