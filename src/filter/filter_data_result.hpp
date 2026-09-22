#pragma once

#include <string>
#include <utility>

#include "filter/filter_direct_response.hpp"
#include "net/http/types.hpp"

// IFilter::on_request_data()/on_response_data() 한 스텝의 결과. FilterHeaderResult
// (헤더 단계)와 형태는 비슷하지만 전용 타입으로 분리한다 -- Envoy도
// FilterHeadersStatus/FilterDataStatus를 분리하는데, 데이터 단계에만
// 의미 있는 kStopIterationAndBuffer를 헤더 단계 타입에 섞으면 "헤더
// 필터가 이 값을 리턴하면 뭘 해야 하나"라는 무의미한 케이스가 생기기
// 때문이다. 공통 필드(direct_response)는 filter_direct_response.hpp로 뽑음.
enum class FilterDataAction {
    kContinue,                // data를 그대로(혹은 필터가 고친 대로) 다음 단계로
    kRespondDirectly,         // 즉시 응답 -- 요청 relay/response relay 중단
    kStopIterationAndBuffer,  // 아직 판단 불가 -- 이 청크까지 누적해서 계속 들고 있어달라
};

struct FilterDataResult {
    FilterDataAction action = FilterDataAction::kContinue;
    // action이 kRespondDirectly일 때만 유효.
    DirectResponse direct_response;

    static FilterDataResult continue_() { return FilterDataResult{}; }

    static FilterDataResult stop_and_buffer() {
        FilterDataResult r;
        r.action = FilterDataAction::kStopIterationAndBuffer;
        return r;
    }

    static FilterDataResult respond(net::http::ResponseHead head, std::string body) {
        FilterDataResult r;
        r.action = FilterDataAction::kRespondDirectly;
        r.direct_response = DirectResponse{std::move(head), std::move(body)};
        return r;
    }
};
