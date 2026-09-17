#pragma once

#include <string>
#include <vector>

// HTTP 메시지의 라이브러리 독립적인 표현. net:: 최상위 네임스페이스와
// 마찬가지로, 이 안의 어떤 것도 Beast(혹은 다른 HTTP 라이브러리)를
// 알아서는 안 된다. 구체 구현은 net/boost/http/ 아래.
namespace net::http {

struct Header {
    std::string name;
    std::string value;
};

// 요청의 header 부분만 (start-line + headers). body는 별도로 스트리밍
// relay되므로 여기 포함하지 않는다.
struct RequestHead {
    std::string method;
    std::string target;
    unsigned version = 11;  // 11 = HTTP/1.1, 10 = HTTP/1.0
    std::vector<Header> headers;
};

struct ResponseHead {
    unsigned status = 0;
    std::string reason;
    unsigned version = 11;
    std::vector<Header> headers;
};

}  // namespace net::http
