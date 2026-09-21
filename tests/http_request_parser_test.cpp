#include "net/http/llhttp/request_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using net::http::llhttp_backend::LlhttpRequestParser;

namespace {

net::ConstBuffer buf(const std::string& s) { return net::ConstBuffer{s.data(), s.size()}; }

std::string drain_body(net::http::IRequestParser& p) {
    std::string out;
    char scratch[256];
    for (;;) {
        const std::size_t n = p.read_body(net::MutableBuffer{scratch, sizeof(scratch)});
        if (n == 0) {
            break;
        }
        out.append(scratch, n);
    }
    return out;
}

}  // namespace

TEST(LlhttpRequestParser, GET_요청은_body_없이_한번에_파싱된다) {
    LlhttpRequestParser parser;
    const std::string raw = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";

    const std::size_t consumed = parser.feed(buf(raw));

    EXPECT_EQ(consumed, raw.size());
    ASSERT_TRUE(parser.header_done());
    EXPECT_EQ(parser.head().method, "GET");
    EXPECT_EQ(parser.head().target, "/index.html");
    EXPECT_EQ(parser.head().version, 11u);
    ASSERT_EQ(parser.head().headers.size(), 1u);
    EXPECT_EQ(parser.head().headers[0].name, "Host");
    EXPECT_EQ(parser.head().headers[0].value, "example.com");
    EXPECT_TRUE(parser.message_done());
    EXPECT_FALSE(parser.has_error());
}

TEST(LlhttpRequestParser, Content_Length_body가_올바르게_파싱된다) {
    LlhttpRequestParser parser;
    const std::string raw = "POST /submit HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello";

    parser.feed(buf(raw));

    ASSERT_TRUE(parser.header_done());
    ASSERT_TRUE(parser.message_done());
    EXPECT_EQ(drain_body(parser), "hello");
}

TEST(LlhttpRequestParser, chunked_body가_디코딩돼서_파싱된다) {
    LlhttpRequestParser parser;
    const std::string raw =
        "POST /submit HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";

    parser.feed(buf(raw));

    ASSERT_TRUE(parser.message_done());
    EXPECT_EQ(drain_body(parser), "hello world");
}

TEST(LlhttpRequestParser, 바이트_단위로_쪼개서_feed해도_동일하게_파싱된다) {
    LlhttpRequestParser parser;
    const std::string raw = "POST /submit HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello";

    for (char c : raw) {
        const std::string one(1, c);
        const std::size_t consumed = parser.feed(buf(one));
        EXPECT_EQ(consumed, 1u);
    }

    ASSERT_TRUE(parser.message_done());
    EXPECT_EQ(drain_body(parser), "hello");
}

TEST(LlhttpRequestParser, 대용량_body도_한번에_feed하면_전부_파싱된다) {
    // 회귀 테스트: on_body 콜백에서 HPE_PAUSED를 리턴해 고정 크기(16KB)
    // scratch를 채우면 멈추려던 예전 구현은, llhttp의 on_body가
    // HPE_PAUSED를 지원하지 않아 그 리턴값이 그냥 무시되는 버그가 있었다
    // (실사용에서 body가 64KB 넘는 순간 relay가 멈춰버림). 이 테스트는
    // scratch 상한(16KB)을 훨씬 넘는 body를 한 번의 feed()로 흘려보내서
    // 그 버그가 재발하면 바로 잡히게 한다.
    LlhttpRequestParser parser;
    const std::string body(200000, 'X');
    const std::string raw =
        "POST /submit HTTP/1.1\r\nHost: example.com\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    const std::size_t consumed = parser.feed(buf(raw));

    EXPECT_EQ(consumed, raw.size());
    EXPECT_FALSE(parser.has_error());
    ASSERT_TRUE(parser.message_done());
    EXPECT_EQ(drain_body(parser), body);
}

TEST(LlhttpRequestParser, 잘못된_요청은_에러로_처리된다) {
    LlhttpRequestParser parser;
    const std::string raw = "NOT A VALID REQUEST LINE AT ALL\r\n\r\n";

    parser.feed(buf(raw));

    EXPECT_TRUE(parser.has_error());
}
