#include "net/llhttp/request_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using net::llhttp_backend::LlhttpRequestParser;

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

TEST(LlhttpRequestParser, 잘못된_요청은_에러로_처리된다) {
    LlhttpRequestParser parser;
    const std::string raw = "NOT A VALID REQUEST LINE AT ALL\r\n\r\n";

    parser.feed(buf(raw));

    EXPECT_TRUE(parser.has_error());
}
