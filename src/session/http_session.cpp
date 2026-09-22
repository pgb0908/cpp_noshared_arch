#include "session/http_session.hpp"

#include <cassert>

#include "net/http/llhttp/factory.hpp"
#include "net/http/native/detail.hpp"
#include "net/http/native/factory.hpp"

void HttpSession::start() {
    metrics_.on_accept();
    read_buf_ = buffer_pool_.acquire();
    write_buf_ = buffer_pool_.acquire();
    request_parser_ = net::http::llhttp_backend::create_request_parser();
    request_serializer_ = net::http::native::create_request_serializer();
    read_request_chunk();
}

// keep-alive로 같은 downstream 소켓에서 다음 요청을 받기 전에 요청별
// 상태를 전부 새로 만든다. read_buf_/write_buf_(BufferPool에서 빌린
// 버퍼)는 커넥션과 함께 재사용 -- 매 요청마다 반납/재획득할 이유가 없다.
void HttpSession::reset_for_next_request() {
    request_parser_ = net::http::llhttp_backend::create_request_parser();
    request_serializer_ = net::http::native::create_request_serializer();
    request_filtered_ = false;
    request_serializer_started_ = false;
    request_final_body_provided_ = false;
    upstream_connecting_ = false;
    upstream_wrote_once_ = false;
    upstream_retry_used_ = false;
    upstream_whole_request_captured_ = false;
    upstream_whole_request_snapshot_.clear();
    upstream_response_read_any_ = false;

    response_parser_.reset();
    response_serializer_.reset();
    response_filtered_ = false;
    response_serializer_started_ = false;
    response_final_body_provided_ = false;
    direct_response_mode_ = false;

    downstream_wants_keep_alive_ = false;
    downstream_keep_alive_effective_ = false;

    filter_ctx_ = FilterContext{};
    body_filter_gate_.reset();

    read_request_chunk();
}

void HttpSession::assert_on_owning_thread() const {
    assert(event_loop_.is_current_thread() && "HttpSession I/O callback running on a non-owning thread");
}

// ---------- 1단계: downstream 요청 파싱 -> 필터 -> (upstream 연결) -> upstream 전송 ----------

void HttpSession::read_request_chunk() {
    auto self = shared_from_this();
    downstream_->async_read_some(
        net::MutableBuffer{read_buf_->data(), read_buf_->size()}, [this, self](const net::Error& err, std::size_t n) {
            assert_on_owning_thread();
            if (!err.ok()) {
                close();
                return;
            }
            process_downstream_chunk(net::ConstBuffer{read_buf_->data(), n});
        });
}

void HttpSession::process_downstream_chunk(net::ConstBuffer raw) {
    while (raw.size > 0) {
        const std::size_t consumed = request_parser_->feed(raw);
        raw = net::ConstBuffer{raw.data + consumed, raw.size - consumed};

        if (!request_filtered_ && request_parser_->header_done()) {
            request_filtered_ = true;
            // 실제로 요청 헤더가 파싱된 시점에만 카운트 -- start()/
            // reset_for_next_request()에서 미리 세면, keep-alive 커넥션이
            // 다음 요청 없이 그냥 닫히는 경우까지 "요청"으로 잘못 잡힌다.
            metrics_.requests_handled.fetch_add(1, std::memory_order_relaxed);
            // 클라이언트가 keep-alive를 원했는지는 요청 자체(버전 +
            // Connection 헤더)만으로 결정되고, 이후 응답 프레이밍과
            // 무관하게 고정된다 -- 최종 판단(downstream_keep_alive_effective_)은
            // 응답 헤더가 파싱된 뒤 process_upstream_chunk()에서 확정.
            downstream_wants_keep_alive_ = request_parser_->should_keep_alive();
            net::http::RequestHead head = request_parser_->head();
            const FilterHeaderResult result = filter_chain_.apply_request(head, filter_ctx_);
            if (result.action == FilterHeaderAction::kRespondDirectly) {
                respond_directly(result.direct_response.head, result.direct_response.body);
                return;
            }
            // upstream과의 Connection은 downstream/upstream hop을
            // 독립적으로 판단하는 설계(doc/plan.md "keep-alive 도입"
            // 참고)에 따라 클라이언트가 뭘 보냈든 항상 keep-alive를
            // 요청한다 -- 그래야 UpstreamManager pool 재사용이 가능해짐.
            // 실제로 재사용 가능한지는 응답의 should_keep_alive()로 다시 확인.
            net::http::set_header(head.headers, "Connection", "keep-alive");
            request_serializer_->start(head);
            request_serializer_started_ = true;
        }

        if (request_serializer_started_) {
            drain_request_body_into_serializer();
            if (direct_response_mode_) {
                return;  // 바디 필터가 거부(413 등) -- upstream 연결/전송 로직 스킵
            }
        }

        if (consumed == 0) {
            if (request_parser_->has_error()) {
                close();
                return;
            }
            break;  // 진행 불가 (정상적으로는 도달하지 않음) -- 방어적 중단
        }
    }

    if (!request_serializer_started_) {
        read_request_chunk();  // 헤더가 아직 안 끝남 -- 계속 읽기
        return;
    }

    if (request_parser_->message_done() && !request_final_body_provided_) {
        request_final_body_provided_ = true;
        request_serializer_->provide_body(net::ConstBuffer{nullptr, 0}, true);
    }

    if (!upstream_connecting_) {
        // 필터를 통과했을 때만 여기 도달한다 (거부됐으면 위에서 이미
        // return). 지금 이 시점에 upstream 연결을 시작 -- 그 전에는
        // 절대 connect하지 않는다.
        upstream_connecting_ = true;
        connect_upstream();
        return;
    }
    if (upstream_) {
        pull_and_write_request_to_upstream();
    }
    // else: 아직 connect 중 -- connect_upstream()의 콜백이 이어서 처리
}

// 파서에서 뽑은 바디 조각을 시리얼라이저로 바로 넘기지 않고, 먼저
// 필터 체인을 거친다. 어느 청크가 메시지의 마지막인지(end_stream)를
// 알아야 하는데, read_body()는 "지금 당장 더 없다"만 알려주지
// "메시지 전체가 끝났다"는 알려주지 않으므로, 청크 하나를 미리
// 들고 있다가(pending) 다음 청크가 있으면 false로 흘려보내고,
// 루프가 끝나서 더 없다는 걸 확인한 뒤에야 그 마지막 pending을
// message_done()으로 판단한 end_stream과 함께 내보낸다.
void HttpSession::drain_request_body_into_serializer() {
    std::string pending;
    bool has_pending = false;
    for (;;) {
        const std::size_t n = request_parser_->read_body(net::MutableBuffer{body_scratch_.data(), body_scratch_.size()});
        if (n == 0) {
            break;
        }
        if (has_pending) {
            if (!process_request_data_chunk(std::move(pending), false)) {
                return;  // 필터가 거부했거나(413) 세션이 직접 응답 모드로 전환됨
            }
            pending.clear();
        }
        pending.assign(body_scratch_.data(), n);
        has_pending = true;
    }
    if (has_pending) {
        process_request_data_chunk(std::move(pending), request_parser_->message_done());
    }
}

// 바디 청크 하나를 필터 체인(BodyFilterGate)에 통과시킨 뒤 시리얼라이저로
// 넘긴다. 필터 적용/watermark 판단 자체는 BodyFilterGate 책임 --
// 여기서는 그 결과를 소켓/시리얼라이저 동작으로 옮기기만 한다.
// 반환값이 false면 필터가 즉시 응답(거부)해서 세션이 direct response
// 모드로 전환됐다는 뜻 -- 호출자는 이후 정상 relay 로직(다음 필터,
// upstream 연결 등)을 계속하면 안 된다.
bool HttpSession::process_request_data_chunk(std::string chunk, bool end_stream) {
    BodyFilterGate::Result result = body_filter_gate_.apply_request(std::move(chunk), end_stream);
    switch (result.outcome) {
        case BodyFilterGate::Outcome::kReject:
            respond_directly(result.reject_head, result.reject_body);
            return false;
        case BodyFilterGate::Outcome::kStillBuffering:
            return true;  // 계속 읽어들임 -- 아직 upstream엔 아무것도 안 보냄
        case BodyFilterGate::Outcome::kForward:
            if (end_stream) {
                request_final_body_provided_ = true;
            }
            request_serializer_->provide_body(net::ConstBuffer{result.data.data(), result.data.size()}, end_stream);
            return true;
    }
    return true;  // 도달 불가 -- switch가 enum 전체를 다룸
}

void HttpSession::connect_upstream() {
    endpoint_index_ = upstream_manager_.select_endpoint();
    auto self = shared_from_this();
    upstream_manager_.acquire_connection(
        endpoint_index_, [this, self](const net::Error& err, std::unique_ptr<net::ISocket> socket) {
            assert_on_owning_thread();
            if (!err.ok() || !socket) {
                metrics_.upstream_connect_errors.fetch_add(1, std::memory_order_relaxed);
                close();
                return;
            }
            upstream_ = std::move(socket);
            pull_and_write_request_to_upstream();
        });
}

void HttpSession::pull_and_write_request_to_upstream() {
    const std::size_t n = request_serializer_->pull(net::MutableBuffer{write_buf_->data(), write_buf_->size()});
    if (n == 0) {
        if (request_serializer_->done()) {
            start_response_phase();
        } else if (!request_parser_->message_done()) {
            read_request_chunk();  // 더 내보낼 게 없음 -- downstream에서 더 읽어야 함
        } else {
            close();  // 이 상태(메시지는 끝났는데 시리얼라이저가 안 끝남)는 도달 불가여야 함 -- 방어적 종료
        }
        return;
    }

    // 이번 write 한 번으로 요청 전체(헤더+바디)가 다 나간다면 -- 바디
    // 없는 요청 등, 실무에서 흔한 케이스 -- 나중에 읽기 실패로 재시도가
    // 필요해질 경우를 위해 그대로 스냅샷해둔다 (upstream_whole_request_captured_
    // 주석 참고). 스트리밍 중인 큰 바디는 여기 안 걸려서 스냅샷을 안 남김.
    if (!upstream_wrote_once_ && request_serializer_->done()) {
        upstream_whole_request_snapshot_.assign(write_buf_->data(), n);
        upstream_whole_request_captured_ = true;
    }

    auto self = shared_from_this();
    upstream_->async_write(net::ConstBuffer{write_buf_->data(), n}, [this, self, n](const net::Error& err, std::size_t written) {
        assert_on_owning_thread();
        if (!err.ok()) {
            // pool에서 꺼낸 커넥션이 이미 peer에 의해 끊겨 있었을 수
            // 있음(Phase 3에서 알려진 갭) -- 이 커넥션으로 아직 한
            // 바이트도 성공적으로 못 보냈고, 재시도를 아직 안 썼다면
            // fresh connect로 딱 1회 재시도. 이미 일부라도 보낸 적
            // 있으면 재시도는 안전하지 않음(중복 전송) -- 그냥 종료.
            if (!upstream_wrote_once_ && !upstream_retry_used_) {
                upstream_retry_used_ = true;
                retry_upstream_write_once(n);
                return;
            }
            close();
            return;
        }
        upstream_wrote_once_ = true;
        metrics_.bytes_downstream_to_upstream.fetch_add(written, std::memory_order_relaxed);
        pull_and_write_request_to_upstream();
    });
}

void HttpSession::retry_upstream_write_once(std::size_t n) {
    metrics_.upstream_retries.fetch_add(1, std::memory_order_relaxed);
    if (upstream_) {
        upstream_->close();  // pool에 반납하지 않고 그냥 버림 -- 죽어있던 소켓
    }
    upstream_.reset();
    auto self = shared_from_this();
    upstream_manager_.acquire_fresh_connection(
        endpoint_index_, [this, self, n](const net::Error& err, std::unique_ptr<net::ISocket> socket) {
            assert_on_owning_thread();
            if (!err.ok() || !socket) {
                metrics_.upstream_connect_errors.fetch_add(1, std::memory_order_relaxed);
                close();
                return;
            }
            upstream_ = std::move(socket);
            // write_buf_[0, n)은 실패한 첫 시도와 정확히 같은 바이트다
            // (serializer의 pull()은 실패 여부와 무관하게 내부 offset을
            // 이미 그만큼 전진시켜 놨으므로, 시리얼라이저를 다시 건드리지
            // 않고 이 버퍼 그대로 재전송하면 된다).
            auto retry_self = shared_from_this();
            upstream_->async_write(net::ConstBuffer{write_buf_->data(), n},
                                    [this, retry_self](const net::Error& write_err, std::size_t written) {
                                        assert_on_owning_thread();
                                        if (!write_err.ok()) {
                                            close();
                                            return;
                                        }
                                        upstream_wrote_once_ = true;
                                        metrics_.bytes_downstream_to_upstream.fetch_add(written, std::memory_order_relaxed);
                                        pull_and_write_request_to_upstream();
                                    });
        });
}

void HttpSession::retry_upstream_after_read_failure() {
    metrics_.upstream_retries.fetch_add(1, std::memory_order_relaxed);
    if (upstream_) {
        upstream_->close();  // pool에 반납하지 않고 그냥 버림 -- 죽어있던 소켓
    }
    upstream_.reset();
    auto self = shared_from_this();
    upstream_manager_.acquire_fresh_connection(
        endpoint_index_, [this, self](const net::Error& err, std::unique_ptr<net::ISocket> socket) {
            assert_on_owning_thread();
            if (!err.ok() || !socket) {
                metrics_.upstream_connect_errors.fetch_add(1, std::memory_order_relaxed);
                close();
                return;
            }
            upstream_ = std::move(socket);
            auto retry_self = shared_from_this();
            upstream_->async_write(
                net::ConstBuffer{upstream_whole_request_snapshot_.data(), upstream_whole_request_snapshot_.size()},
                [this, retry_self](const net::Error& write_err, std::size_t written) {
                    assert_on_owning_thread();
                    if (!write_err.ok()) {
                        close();
                        return;
                    }
                    metrics_.bytes_downstream_to_upstream.fetch_add(written, std::memory_order_relaxed);
                    start_response_phase();  // 파서/시리얼라이저를 새로 만들고 다시 read
                });
        });
}

// ---------- 2단계: upstream 응답 파싱 -> 필터 -> downstream 전송 ----------

void HttpSession::start_response_phase() {
    response_parser_ = net::http::llhttp_backend::create_response_parser();
    response_serializer_ = net::http::native::create_response_serializer();
    read_response_chunk();
}

void HttpSession::read_response_chunk() {
    auto self = shared_from_this();
    upstream_->async_read_some(
        net::MutableBuffer{read_buf_->data(), read_buf_->size()}, [this, self](const net::Error& err, std::size_t n) {
            assert_on_owning_thread();
            if (!err.ok()) {
                // 응답을 한 바이트도 못 받은 채 첫 read가 실패 -- 죽어있는
                // pooled 커넥션의 실제 실패 양상(위 upstream_whole_request_captured_
                // 주석 참고). 요청 전체를 스냅샷해뒀고 재시도를 아직 안
                // 썼다면 fresh connect로 1회 재시도.
                if (!upstream_response_read_any_ && upstream_whole_request_captured_ && !upstream_retry_used_) {
                    upstream_retry_used_ = true;
                    retry_upstream_after_read_failure();
                    return;
                }
                close();
                return;
            }
            upstream_response_read_any_ = true;
            process_upstream_chunk(net::ConstBuffer{read_buf_->data(), n});
        });
}

void HttpSession::process_upstream_chunk(net::ConstBuffer raw) {
    while (raw.size > 0) {
        const std::size_t consumed = response_parser_->feed(raw);
        raw = net::ConstBuffer{raw.data + consumed, raw.size - consumed};

        if (!response_filtered_ && response_parser_->header_done()) {
            response_filtered_ = true;
            net::http::ResponseHead head = response_parser_->head();
            const FilterHeaderResult result = filter_chain_.apply_response(head, filter_ctx_);
            if (result.action == FilterHeaderAction::kRespondDirectly) {
                respond_directly(result.direct_response.head, result.direct_response.body);
                return;
            }
            // downstream keep-alive 최종 판단: 클라이언트가 원했는지
            // (downstream_wants_keep_alive_) + 이 응답이 명확한 프레이밍
            // (Content-Length 또는 chunked)을 가졌는지. 프레이밍이
            // 불명확한(close-delimited) 응답에서 keep-alive라고 하면
            // 클라이언트가 다음 요청을 언제 보내도 되는지 알 방법이
            // 없어져 거짓 약속이 된다.
            const bool response_framed =
                net::http::has_header(head.headers, "Content-Length") || net::http::native::detail::has_chunked_encoding(head.headers);
            downstream_keep_alive_effective_ = downstream_wants_keep_alive_ && response_framed;
            net::http::set_header(head.headers, "Connection", downstream_keep_alive_effective_ ? "keep-alive" : "close");
            response_serializer_->start(head);
            response_serializer_started_ = true;
        }

        if (response_serializer_started_) {
            drain_response_body_into_serializer();
            if (direct_response_mode_) {
                return;  // 바디 필터가 거부 -- 아래 정상 relay 로직 스킵
            }
        }

        if (consumed == 0) {
            if (response_parser_->has_error()) {
                close();
                return;
            }
            break;
        }
    }

    if (!response_serializer_started_) {
        read_response_chunk();
        return;
    }

    if (response_parser_->message_done() && !response_final_body_provided_) {
        response_final_body_provided_ = true;
        response_serializer_->provide_body(net::ConstBuffer{nullptr, 0}, true);
    }

    pull_and_write_response_to_downstream();
}

// drain_request_body_into_serializer()와 대칭 (end_stream 판단을
// 위한 pending-하나-미리보기 방식도 동일).
void HttpSession::drain_response_body_into_serializer() {
    std::string pending;
    bool has_pending = false;
    for (;;) {
        const std::size_t n =
            response_parser_->read_body(net::MutableBuffer{body_scratch_.data(), body_scratch_.size()});
        if (n == 0) {
            break;
        }
        if (has_pending) {
            if (!process_response_data_chunk(std::move(pending), false)) {
                return;
            }
            pending.clear();
        }
        pending.assign(body_scratch_.data(), n);
        has_pending = true;
    }
    if (has_pending) {
        process_response_data_chunk(std::move(pending), response_parser_->message_done());
    }
}

// process_request_data_chunk()와 대칭 (BodyFilterGate::apply_response()가
// 등록 역순(onion) 실행과 413 대신 502를 쓰는 것까지 알아서 처리한다).
bool HttpSession::process_response_data_chunk(std::string chunk, bool end_stream) {
    BodyFilterGate::Result result = body_filter_gate_.apply_response(std::move(chunk), end_stream);
    switch (result.outcome) {
        case BodyFilterGate::Outcome::kReject:
            respond_directly(result.reject_head, result.reject_body);
            return false;
        case BodyFilterGate::Outcome::kStillBuffering:
            return true;
        case BodyFilterGate::Outcome::kForward:
            if (end_stream) {
                response_final_body_provided_ = true;
            }
            response_serializer_->provide_body(net::ConstBuffer{result.data.data(), result.data.size()}, end_stream);
            return true;
    }
    return true;  // 도달 불가
}

void HttpSession::pull_and_write_response_to_downstream() {
    const std::size_t n = response_serializer_->pull(net::MutableBuffer{write_buf_->data(), write_buf_->size()});
    if (n == 0) {
        if (response_serializer_->done()) {
            // upstream 재사용 여부는 downstream keep-alive와 독립적으로
            // 판단(hop-by-hop decouple, doc/plan.md "keep-alive 도입"
            // 참고) -- 클라이언트가 이 커넥션을 끊으려 해도, 응답 자체가
            // keep-alive 가능했다면 upstream 커넥션은 다음 세션을 위해
            // pool에 반납한다. direct_response_mode_(필터 거부/에러
            // 합성 응답)일 땐 response_parser_가 진짜 upstream 응답을
            // 대표하지 않으므로(혹은 아예 null이므로) 절대 반납하지 않음.
            if (!direct_response_mode_ && response_parser_ && response_parser_->should_keep_alive() && upstream_) {
                upstream_manager_.release_connection(endpoint_index_, std::move(upstream_));
            }
            if (!direct_response_mode_ && downstream_keep_alive_effective_) {
                reset_for_next_request();  // 같은 downstream 소켓에서 다음 요청 대기
            } else {
                close();
            }
        } else if (!direct_response_mode_ && response_parser_ && !response_parser_->message_done()) {
            read_response_chunk();
        } else {
            close();  // 방어적 종료 (도달 불가여야 함)
        }
        return;
    }

    auto self = shared_from_this();
    downstream_->async_write(net::ConstBuffer{write_buf_->data(), n}, [this, self](const net::Error& err, std::size_t written) {
        assert_on_owning_thread();
        if (!err.ok()) {
            close();
            return;
        }
        metrics_.bytes_upstream_to_downstream.fetch_add(written, std::memory_order_relaxed);
        pull_and_write_response_to_downstream();
    });
}

// ---------- 필터의 즉시 응답 처리 ----------

// 요청 필터(upstream 연결 전) 또는 응답 필터(upstream 응답 대신)가
// kRespondDirectly를 반환했을 때 호출된다. upstream_이 null일 수도
// 있다는 게 핵심 -- 요청 필터가 거부한 경우 upstream엔 아예 연결
// 안 한 상태다.
void HttpSession::respond_directly(const net::http::ResponseHead& head, const std::string& body) {
    direct_response_mode_ = true;
    response_serializer_ = net::http::native::create_response_serializer();
    response_serializer_started_ = true;
    response_serializer_->start(head);
    response_serializer_->provide_body(net::ConstBuffer{body.data(), body.size()}, true);
    pull_and_write_response_to_downstream();
}

// ---------- 종료 ----------

void HttpSession::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    std::cerr << "[debug] close() called, upstream_wrote_once=" << upstream_wrote_once_
              << " upstream_retry_used=" << upstream_retry_used_ << " has_upstream=" << (bool)upstream_ << "\n";

    downstream_->shutdown();
    downstream_->close();

    // keep-alive를 지원하지 않으므로 upstream 연결도 항상 닫는다 --
    // UpstreamManager의 pool에는 절대 반납하지 않음. upstream_이
    // null일 수 있음(필터가 요청을 거부해서 애초에 연결 안 한 경우).
    if (upstream_) {
        upstream_->shutdown();
        upstream_->close();
    }

    if (read_buf_) {
        buffer_pool_.release(std::move(read_buf_));
    }
    if (write_buf_) {
        buffer_pool_.release(std::move(write_buf_));
    }

    metrics_.on_close();
}
