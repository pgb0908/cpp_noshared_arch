#include "net/http/llhttp/response_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using net::http::llhttp_backend::LlhttpResponseParser;

namespace {

net::ConstBuffer buf(const std::string& s) { return net::ConstBuffer{s.data(), s.size()}; }

std::string drain_body(net::http::IResponseParser& p) {
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

TEST(LlhttpResponseParser, 상태줄과_헤더가_올바르게_파싱된다) {
    LlhttpResponseParser parser;
    const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

    parser.feed(buf(raw));

    ASSERT_TRUE(parser.header_done());
    EXPECT_EQ(parser.head().status, 200u);
    EXPECT_EQ(parser.head().reason, "OK");
    EXPECT_EQ(parser.head().version, 11u);
    ASSERT_TRUE(parser.message_done());
    EXPECT_EQ(drain_body(parser), "hi");
}

TEST(LlhttpResponseParser, 대용량_body도_한번에_feed하면_전부_파싱된다) {
    // 회귀 테스트: LlhttpRequestParser의 동일 테스트 참고.
    LlhttpResponseParser parser;
    const std::string body(200000, 'Y');
    const std::string raw =
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    parser.feed(buf(raw));

    EXPECT_FALSE(parser.has_error());
    ASSERT_TRUE(parser.message_done());
    EXPECT_EQ(drain_body(parser), body);
}

TEST(LlhttpResponseParser, 404_응답도_정상_파싱된다) {
    LlhttpResponseParser parser;
    const std::string raw = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

    parser.feed(buf(raw));

    EXPECT_EQ(parser.head().status, 404u);
    EXPECT_EQ(parser.head().reason, "Not Found");
    EXPECT_TRUE(parser.message_done());
}
