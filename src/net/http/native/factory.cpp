#include "net/http/native/factory.hpp"

#include "net/http/native/request_serializer.hpp"
#include "net/http/native/response_serializer.hpp"

namespace net::http::native {

std::unique_ptr<IRequestSerializer> create_request_serializer() {
    return std::make_unique<RequestSerializer>();
}

std::unique_ptr<IResponseSerializer> create_response_serializer() {
    return std::make_unique<ResponseSerializer>();
}

}  // namespace net::http::native
