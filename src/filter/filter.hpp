#pragma once

#include <string>

#include "filter/filter_context.hpp"
#include "filter/filter_data_result.hpp"
#include "filter/filter_header_result.hpp"
#include "net/http/types.hpp"

// downstream <-> upstream을 오가는 요청/응답 양쪽에 개입할 수 있는 필터
// 하나. Envoy/Proxygen처럼 request 경로와 response 경로 메서드가 대칭으로
// 하나의 인터페이스에 있다 -- 필터 하나가 둘 다 구현할 수도, 한쪽만
// override해서 나머지는 기본 continue_()로 둘 수도 있다.
//
// 인스턴스는 shard당 하나씩만 만들어져 그 shard의 모든 세션이 공유한다
// (요청마다 새로 만들지 않음). 그래서 필터 자신의 멤버 변수에 "이 요청만의"
// 상태를 담으면 안 된다 -- 그런 상태(요청 시작 시각, 인증 결과 등)는
// FilterContext에 담아서 세션 스코프로 관리한다 (doc/plan.md 참고).
class IFilter {
public:
    virtual ~IFilter() = default;

    // head는 이 필터가 직접 수정 가능 (헤더 추가/삭제, target 재작성 등).
    virtual FilterHeaderResult on_request(net::http::RequestHead& head, FilterContext& ctx) {
        (void)head;
        (void)ctx;
        return FilterHeaderResult::continue_();
    }

    virtual FilterHeaderResult on_response(net::http::ResponseHead& head, FilterContext& ctx) {
        (void)head;
        (void)ctx;
        return FilterHeaderResult::continue_();
    }

    // 바디 청크 하나(혹은, 이 필터가 이전에 kStopIterationAndBuffer를
    // 반환해서 계속 누적 중이었다면 그 누적본 전체)를 본다. data는
    // 이 필터가 직접 고칠 수 있다(압축/치환 등). end_stream이 true면
    // 이게 이 메시지의 마지막 데이터라는 뜻 -- 이 시점엔
    // kStopIterationAndBuffer를 반환하면 안 된다(더 기다릴 데이터가 없음).
    virtual FilterDataResult on_request_data(std::string& data, bool end_stream, FilterContext& ctx) {
        (void)data;
        (void)end_stream;
        (void)ctx;
        return FilterDataResult::continue_();
    }

    virtual FilterDataResult on_response_data(std::string& data, bool end_stream, FilterContext& ctx) {
        (void)data;
        (void)end_stream;
        (void)ctx;
        return FilterDataResult::continue_();
    }
};
