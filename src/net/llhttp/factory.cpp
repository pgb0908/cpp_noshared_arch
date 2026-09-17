#include "net/llhttp/factory.hpp"

#include "net/llhttp/request_parser.hpp"
#include "net/llhttp/response_parser.hpp"

namespace net::llhttp_backend {

std::unique_ptr<net::http::IRequestParser> create_request_parser() {
    return std::make_unique<LlhttpRequestParser>();
}

std::unique_ptr<net::http::IResponseParser> create_response_parser() {
    return std::make_unique<LlhttpResponseParser>();
}

}  // namespace net::llhttp_backend
