#pragma once

#include <string>
#include <utility>

#include "filter/filter_direct_response.hpp"
#include "net/http/types.hpp"

// 필터 체인 한 스텝의 결과. kContinue면 (필터가 head를 고쳤을 수도 있는
// 채로) 다음 필터로 넘어가고, kRespondDirectly면 그 자리에서 즉시
// 응답을 만들어 downstream에 돌려주고 relay 자체를 중단한다 -- 예를
// 들어 인증 실패(401), rate limit(429) 같은 경우 upstream에는 아예
// 연결하지 않아도 된다.
enum class FilterHeaderAction {
    kContinue,
    kRespondDirectly,
};

struct FilterHeaderResult {
    FilterHeaderAction action = FilterHeaderAction::kContinue;
    // action이 kRespondDirectly일 때만 유효.
    DirectResponse direct_response;

    static FilterHeaderResult continue_() { return FilterHeaderResult{}; }

    static FilterHeaderResult respond(net::http::ResponseHead head, std::string body) {
        FilterHeaderResult r;
        r.action = FilterHeaderAction::kRespondDirectly;
        r.direct_response = DirectResponse{std::move(head), std::move(body)};
        return r;
    }
};
