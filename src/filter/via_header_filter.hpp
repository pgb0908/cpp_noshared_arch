#pragma once

#include "filter/filter.hpp"

// RFC 7230 section 5.7.1: 프록시를 거친 메시지에는 Via 헤더를 추가하는
// 게 관례다 (실제 nginx/Envoy 등도 이런 종류의 헤더를 붙인다). 필터
// 체인이 실제로 동작한다는 걸 보여주는 가장 단순하고 현실적인 예시로
// 기본 체인에 등록해둔다 -- doc/plan.md의 필터 아키텍처 항목 참고.
//
// 무상태 필터라 request/response 양쪽 메서드를 한 클래스에 대칭으로
// 구현하는 것 자체가 FilterContext 없이도 자명하게 성립하는 예시다.
class ViaHeaderFilter : public IFilter {
public:
    FilterHeaderResult on_request(net::http::RequestHead& head, FilterContext&) override {
        head.headers.push_back(net::http::Header{"Via", "1.1 perCoreShard"});
        return FilterHeaderResult::continue_();
    }

    FilterHeaderResult on_response(net::http::ResponseHead& head, FilterContext&) override {
        head.headers.push_back(net::http::Header{"Via", "1.1 perCoreShard"});
        return FilterHeaderResult::continue_();
    }
};
