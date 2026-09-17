#pragma once

#include <memory>

#include "net/http/serializer.hpp"

namespace net::http::native {

std::unique_ptr<IRequestSerializer> create_request_serializer();
std::unique_ptr<IResponseSerializer> create_response_serializer();

}  // namespace net::http::native
