#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "filter/body_filter_gate.hpp"
#include "filter/filter_chain.hpp"
#include "filter/filter_context.hpp"
#include "net/event_loop.hpp"
#include "net/http/parser.hpp"
#include "net/http/serializer.hpp"
#include "net/socket.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// Connection 하나: 이 shard가 생명주기 전체를 소유한다 (session.hpp와
// 동일한 원칙). raw byte relay였던 Session을 HTTP 인식 relay로 완전히
// 대체한다 -- doc/plan.md 참고, session.hpp는 참고용으로 남겨둠.
//
// keep-alive 없음 (이번 MVP 범위): downstream/upstream 둘 다 요청/응답
// 1회만 처리하고 양쪽 다 close한다. 그래서 UpstreamManager의 connection
// pool도 여기선 안 쓴다 (release_connection()을 절대 호출하지 않음).
//
// HTTP는 request를 다 보내야 response를 받을 수 있는 반이중(half-duplex)
// 프로토콜이라(파이프라이닝 미지원 전제), raw relay처럼 양방향을 동시에
// 안 돌리고 "요청 relay 완료 -> 응답 relay 시작"을 순차로 진행한다.
//
// 필터 체인은 요청 헤더가 다 파싱된 시점(upstream 연결 전!)에 한 번,
// 응답 헤더가 다 파싱된 시점에 한 번 실행된다 (등록 순서 -> 역순인 onion
// 모델, filter_chain.hpp 참고). 필터가 kRespondDirectly를 반환하면 --
// 인증 실패, rate limit 등 -- upstream에 아예 연결하지 않고 바로
// 응답한다. 이게 이 세션이 "필터 통과 전엔 upstream을 connect하지 않는"
// 순서로 짜인 이유다.
//
// 구현은 http_session.cpp에 있다 -- 소켓 I/O 오케스트레이션 부분만
// 여기 선언돼 있고, 필터/watermark 판단 정책은 BodyFilterGate로 이미
// 분리돼 있다 (doc/plan.md의 "BodyFilterGate 추출" 절 참고).
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(std::unique_ptr<net::ISocket> downstream_socket,
                net::IEventLoop& event_loop,
                UpstreamManager& upstream_manager,
                BufferPool& buffer_pool,
                const FilterChain& filter_chain,
                std::size_t body_buffer_high_watermark_bytes,
                LocalMetrics& metrics)
        : downstream_(std::move(downstream_socket)),
          event_loop_(event_loop),
          upstream_manager_(upstream_manager),
          buffer_pool_(buffer_pool),
          filter_chain_(filter_chain),
          body_filter_gate_(filter_chain_, filter_ctx_, body_buffer_high_watermark_bytes),
          metrics_(metrics) {}

    void start();

private:
    void assert_on_owning_thread() const;

    // ---------- 1단계: downstream 요청 파싱 -> 필터 -> (upstream 연결) -> upstream 전송 ----------

    void read_request_chunk();
    void process_downstream_chunk(net::ConstBuffer raw);
    void drain_request_body_into_serializer();
    bool process_request_data_chunk(std::string chunk, bool end_stream);
    void connect_upstream();
    void pull_and_write_request_to_upstream();
    // pull_and_write_request_to_upstream()의 첫 write가 실패했을 때 --
    // pool에서 꺼낸 커넥션이 이미 peer에 의해 끊겨 있었을 가능성이 커서
    // (Phase 3에서 알려진 갭), fresh connect로 딱 1회만 같은 바이트를
    // 재전송해본다. n=이번에 쓰려고 했던 바이트 수(write_buf_ 앞 n바이트).
    void retry_upstream_write_once(std::size_t n);
    // read_response_chunk()의 첫 read가 응답 바이트를 하나도 못 받고
    // 실패했을 때 -- upstream_whole_request_captured_일 때만 호출 가능.
    void retry_upstream_after_read_failure();

    // ---------- 2단계: upstream 응답 파싱 -> 필터 -> downstream 전송 ----------

    void start_response_phase();
    void read_response_chunk();
    void process_upstream_chunk(net::ConstBuffer raw);
    void drain_response_body_into_serializer();
    bool process_response_data_chunk(std::string chunk, bool end_stream);
    void pull_and_write_response_to_downstream();

    // ---------- 필터의 즉시 응답 처리 ----------

    void respond_directly(const net::http::ResponseHead& head, const std::string& body);

    // ---------- keep-alive ----------

    // 요청 1개 사이클이 정상 종료됐고 downstream이 keep-alive 가능할 때,
    // close() 대신 이걸 호출해서 같은 downstream 소켓 위에서 다음 요청을
    // 받는다 (upstream 쪽은 이미 이 시점에 pool로 반납됐거나 닫혔음 --
    // pull_and_write_response_to_downstream() 참고). 요청별 파서/시리얼라이저/
    // 필터 상태를 전부 새로 만든다.
    void reset_for_next_request();

    // ---------- 종료 ----------

    void close();

    std::unique_ptr<net::ISocket> downstream_;
    net::IEventLoop& event_loop_;
    std::unique_ptr<net::ISocket> upstream_;
    UpstreamManager& upstream_manager_;
    const FilterChain& filter_chain_;
    // 이 세션(요청 하나)만을 위한 필터 상태 저장소. 필터 인스턴스 자체는
    // shard당 싱글턴으로 공유되므로, request 단계에서 심은 값을 response
    // 단계나 다른 필터에서 읽고 싶을 때 여기에 담는다 (filter_context.hpp
    // 참고). body_filter_gate_보다 먼저 선언돼야 함 -- 아래에서 그 참조를
    // 바인딩한다.
    FilterContext filter_ctx_;
    // 바디 청크를 필터에 통과시키고 watermark(413/502)를 판단하는 정책을
    // 캡슐화 (body_filter_gate.hpp 참고). 방향별 버퍼링 상태도 이 안에서
    // 관리한다.
    BodyFilterGate body_filter_gate_;
    std::size_t endpoint_index_ = 0;
    bool upstream_connecting_ = false;
    // keep-alive 재시도(retry-once, doc/plan.md "keep-alive 도입" 참고):
    // 이 upstream 커넥션에 대한 첫 write가 성공적으로 완료된 적이
    // 있는지 -- 이게 true가 된 뒤의 실패는 재시도하지 않는다(이미
    // 일부 요청 바이트가 진짜로 upstream에 도달했을 수 있어서 재시도가
    // 안전하지 않음). upstream_retry_used_는 재시도를 이미 1회 썼는지.
    bool upstream_wrote_once_ = false;
    bool upstream_retry_used_ = false;
    // 실측 중 발견: 죽어있는 pooled 커넥션에 대한 첫 write는 보통 로컬
    // send buffer에 조용히 성공(에러 없음)하고, 그 직후 응답을 기다리는
    // 첫 read에서 곧바로 실패로 드러난다 (SIGKILL로 직접 재현해서 확인
    // -- doc/plan.md "keep-alive 도입" 참고). 그래서 재시도는 "첫 write
    // 실패"뿐 아니라 "응답을 1바이트도 못 받은 채 첫 read 실패"도 잡아야
    // 한다. 후자를 재시도하려면 이미 보낸 요청 바이트를 다시 보내야 하는데,
    // 시리얼라이저는 이미 다 드레인돼서 재구성이 안 되므로 -- 요청 전체가
    // 정확히 write 1번으로 나간 경우(바디 없음/작은 요청, 실무 트래픽
    // 대부분)에 한해 그 바이트를 스냅샷으로 보관해뒀다가 재사용한다.
    // 스트리밍 중인 큰 바디는 스냅샷을 안 남기고(메모리 무제한 증가 방지),
    // 그런 경우 read 실패는 재시도하지 않고 바로 종료한다.
    bool upstream_whole_request_captured_ = false;
    std::string upstream_whole_request_snapshot_;
    bool upstream_response_read_any_ = false;

    BufferPool& buffer_pool_;
    std::unique_ptr<std::vector<char>> read_buf_;
    std::unique_ptr<std::vector<char>> write_buf_;
    std::array<char, 4096> body_scratch_{};

    std::unique_ptr<net::http::IRequestParser> request_parser_;
    std::unique_ptr<net::http::IRequestSerializer> request_serializer_;
    bool request_filtered_ = false;
    bool request_serializer_started_ = false;
    bool request_final_body_provided_ = false;

    std::unique_ptr<net::http::IResponseParser> response_parser_;
    std::unique_ptr<net::http::IResponseSerializer> response_serializer_;
    bool response_filtered_ = false;
    bool response_serializer_started_ = false;
    bool response_final_body_provided_ = false;
    bool direct_response_mode_ = false;

    // ---------- keep-alive 판단 ----------
    // 클라이언트의 요청(버전 + Connection 헤더)만으로 결정 -- 응답 프레이밍과
    // 무관하게 "클라이언트가 원했는지"만 담는다 (요청 헤더 파싱 직후 1회 설정).
    bool downstream_wants_keep_alive_ = false;
    // 위 값과 "응답이 명확한 프레이밍(Content-Length 또는 chunked)을
    // 가졌는지"를 합친 최종 판단 -- 응답 헤더가 파싱된 시점에 1회
    // 계산되고, 이 응답의 Connection 헤더 값 결정과 사이클 종료 후
    // 다음 요청으로 넘어갈지 판단 둘 다에 쓰인다. close-delimited
    // 응답(Content-Length도 chunked도 없음)에서 true가 되면 클라이언트에
    // keep-alive라고 거짓말하는 셈이라 반드시 막아야 함.
    bool downstream_keep_alive_effective_ = false;

    LocalMetrics& metrics_;
    bool closing_ = false;
};
