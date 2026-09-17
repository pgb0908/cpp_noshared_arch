#pragma once

#include <memory>

#include "net/http/parser.hpp"

// 이 헤더가 llhttp 기반 구현체를 얻기 위해 외부에서 include해야 하는
// 유일한 진입점 -- net::llhttp_backend 네임스페이스의 구체 클래스들은
// 여기서만 생성되고, 다른 도메인 코드는 net::http:: 인터페이스만 본다.
namespace net::llhttp_backend {

std::unique_ptr<net::http::IRequestParser> create_request_parser();
std::unique_ptr<net::http::IResponseParser> create_response_parser();

}  // namespace net::llhttp_backend
