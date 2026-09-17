#include "net/http/native/request_serializer.hpp"

#include <gtest/gtest.h>

using net::http::native::RequestSerializer;

namespace {

net::ConstBuffer buf(const std::string& s) { return net::ConstBuffer{s.data(), s.size()}; }

// out 버퍼를 작게 줘서, pull()이 여러 번 호출돼야 다 뽑힐 수 있는 상황을
// 강제로 만들면서 전체를 뽑아낸다.
std::string pull_all(net::http::IRequestSerializer& s, std::size_t chunk_size = 4) {
    std::string out;
    std::vector<char> scratch(chunk_size);
    while (!s.done()) {
        const std::size_t n = s.pull(net::MutableBuffer{scratch.data(), scratch.size()});
        out.append(scratch.data(), n);
        if (n == 0 && !s.done()) {
            break;  // provide_body()가 더 필요한 상태 -- 테스트가 알아서 채워줘야 함
        }
    }
    return out;
}

}  // namespace

TEST(RequestSerializer, body_없는_요청을_직렬화한다) {
    RequestSerializer s;
    net::http::RequestHead head;
    head.method = "GET";
    head.target = "/x";
    head.version = 11;
    head.headers.push_back({"Host", "example.com"});

    s.start(head);
    s.provide_body(net::ConstBuffer{nullptr, 0}, true);

    EXPECT_EQ(pull_all(s), "GET /x HTTP/1.1\r\nHost: example.com\r\n\r\n");
    EXPECT_TRUE(s.done());
}

TEST(RequestSerializer, Content_Length_body를_직렬화한다) {
    RequestSerializer s;
    net::http::RequestHead head;
    head.method = "POST";
    head.target = "/submit";
    head.version = 11;
    head.headers.push_back({"Content-Length", "5"});

    s.start(head);
    s.provide_body(buf("hello"), true);

    EXPECT_EQ(pull_all(s), "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
}

TEST(RequestSerializer, 작은_out_버퍼로_나눠_pull해도_전체가_맞게_나온다) {
    RequestSerializer s;
    net::http::RequestHead head;
    head.method = "POST";
    head.target = "/submit";
    head.headers.push_back({"Content-Length", "5"});

    s.start(head);
    s.provide_body(buf("hello"), true);

    EXPECT_EQ(pull_all(s, /*chunk_size=*/3), "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
}

TEST(RequestSerializer, body를_여러_조각으로_나눠_제공해도_이어붙여진다) {
    RequestSerializer s;
    net::http::RequestHead head;
    head.method = "POST";
    head.target = "/submit";
    head.headers.push_back({"Content-Length", "5"});
    s.start(head);

    char scratch[256];
    std::string out;

    s.provide_body(buf("hel"), false);
    std::size_t n = s.pull(net::MutableBuffer{scratch, sizeof(scratch)});
    out.append(scratch, n);
    EXPECT_FALSE(s.done());

    s.provide_body(buf("lo"), true);
    n = s.pull(net::MutableBuffer{scratch, sizeof(scratch)});
    out.append(scratch, n);

    EXPECT_EQ(out, "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
    EXPECT_TRUE(s.done());
}

TEST(RequestSerializer, chunked_인코딩으로_직렬화한다) {
    RequestSerializer s;
    net::http::RequestHead head;
    head.method = "POST";
    head.target = "/submit";
    head.headers.push_back({"Transfer-Encoding", "chunked"});
    s.start(head);

    s.provide_body(buf("abc"), false);
    s.provide_body(buf("de"), true);

    const std::string result = pull_all(s);
    const std::string expected_head = "POST /submit HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string expected_body = "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    EXPECT_EQ(result, expected_head + expected_body);
}
