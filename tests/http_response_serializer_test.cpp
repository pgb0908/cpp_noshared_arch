#include "net/http/native/response_serializer.hpp"

#include <gtest/gtest.h>

using net::http::native::ResponseSerializer;

namespace {

net::ConstBuffer buf(const std::string& s) { return net::ConstBuffer{s.data(), s.size()}; }

std::string pull_all(net::http::IResponseSerializer& s, std::size_t chunk_size = 4) {
    std::string out;
    std::vector<char> scratch(chunk_size);
    while (!s.done()) {
        const std::size_t n = s.pull(net::MutableBuffer{scratch.data(), scratch.size()});
        out.append(scratch.data(), n);
        if (n == 0 && !s.done()) {
            break;
        }
    }
    return out;
}

}  // namespace

TEST(ResponseSerializer, 상태줄과_헤더와_body를_직렬화한다) {
    ResponseSerializer s;
    net::http::ResponseHead head;
    head.status = 200;
    head.reason = "OK";
    head.version = 11;
    head.headers.push_back({"Content-Length", "2"});

    s.start(head);
    s.provide_body(buf("hi"), true);

    EXPECT_EQ(pull_all(s), "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    EXPECT_TRUE(s.done());
}

TEST(ResponseSerializer, body_없는_응답도_직렬화한다) {
    ResponseSerializer s;
    net::http::ResponseHead head;
    head.status = 404;
    head.reason = "Not Found";
    head.headers.push_back({"Content-Length", "0"});

    s.start(head);
    s.provide_body(net::ConstBuffer{nullptr, 0}, true);

    EXPECT_EQ(pull_all(s), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
}

TEST(ResponseSerializer, chunked_응답을_직렬화한다) {
    ResponseSerializer s;
    net::http::ResponseHead head;
    head.status = 200;
    head.reason = "OK";
    head.headers.push_back({"Transfer-Encoding", "chunked"});
    s.start(head);

    s.provide_body(buf("x"), false);
    s.provide_body(buf("yz"), true);

    const std::string expected_head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string expected_body = "1\r\nx\r\n2\r\nyz\r\n0\r\n\r\n";
    EXPECT_EQ(pull_all(s), expected_head + expected_body);
}
