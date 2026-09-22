#pragma once

#include <string>

#include "net/http/types.hpp"

// FilterHeaderResult(헤더 단계)와 FilterDataResult(데이터 단계)가 공통으로
// 갖는 "즉시 응답" 페이로드. 두 타입 모두 action이 kRespondDirectly일 때만
// 이 필드가 유효하다.
struct DirectResponse {
    net::http::ResponseHead head;
    std::string body;
};
