#pragma once

#include <cassert>
#include <memory>
#include <vector>

#include "net/event_loop.hpp"
#include "net/socket.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// Connection 하나: 생명주기 전체를 정확히 하나의 shard가 소유한다.
// 이 session의 모든 I/O는 그 shard의 event loop 스레드에서만 실행된다
// -- shard 간 접근 없음, 락 없음.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::unique_ptr<net::ISocket> downstream_socket,
            net::IEventLoop& event_loop,
            UpstreamManager& upstream_manager,
            BufferPool& buffer_pool,
            LocalMetrics& metrics)
        : downstream_(std::move(downstream_socket)),
          event_loop_(event_loop),
          upstream_manager_(upstream_manager),
          buffer_pool_(buffer_pool),
          metrics_(metrics) {}

    void start() {
        metrics_.on_accept();
        connect_upstream();
    }

private:
    // 이 콜백이 실제로 이 Session을 소유한 shard 스레드에서 실행되고
    // 있는지 검증한다. 여기서 실제 버그가 발견된 뒤 추가함: Listener가
    // accept한 소켓이 Listener의 event loop에 그대로 바인딩된 채로
    // 남아있어서, 모든 Session I/O 콜백이 소유 shard가 아니라 조용히
    // Listener 스레드에서 실행되고 있었음 -- 자세한 건
    // net/event_loop.hpp의 adopt_socket() 주석 참고. 이 assert는
    // 정확히 그 종류의 버그에 대한 회귀 방지 장치이며, 디버그 빌드에서만
    // 활성화된다 (CMakeLists.txt의 기본 빌드 타입 설명 참고).
    void assert_on_owning_thread() const {
        assert(event_loop_.is_current_thread() && "Session I/O callback running on a non-owning thread");
    }

    void connect_upstream() {
        endpoint_index_ = upstream_manager_.select_endpoint();
        auto self = shared_from_this();
        upstream_manager_.acquire_connection(
            endpoint_index_, [this, self](const net::Error& err, std::unique_ptr<net::ISocket> socket) {
                assert_on_owning_thread();
                if (!err.ok() || !socket) {
                    metrics_.upstream_connect_errors.fetch_add(1, std::memory_order_relaxed);
                    upstream_healthy_ = false;
                    close();
                    return;
                }
                upstream_ = std::move(socket);
                on_upstream_ready();
            });
    }

    void on_upstream_ready() {
        down_to_up_buf_ = buffer_pool_.acquire();
        up_to_down_buf_ = buffer_pool_.acquire();
        relay_downstream_to_upstream();
        relay_upstream_to_downstream();
    }

    void relay_downstream_to_upstream() {
        auto self = shared_from_this();
        downstream_->async_read_some(
            net::MutableBuffer{down_to_up_buf_->data(), down_to_up_buf_->size()},
            [this, self](const net::Error& err, std::size_t n) {
                assert_on_owning_thread();
                if (!err.ok()) {
                    // downstream 쪽 실패 -- upstream 연결은 여전히
                    // 정상일 수 있으므로 여기서 unhealthy로 표시하지 않음.
                    close();
                    return;
                }
                upstream_->async_write(
                    net::ConstBuffer{down_to_up_buf_->data(), n},
                    [this, self](const net::Error& write_err, std::size_t written) {
                        assert_on_owning_thread();
                        if (!write_err.ok()) {
                            upstream_healthy_ = false;
                            close();
                            return;
                        }
                        metrics_.bytes_downstream_to_upstream.fetch_add(written, std::memory_order_relaxed);
                        relay_downstream_to_upstream();
                    });
            });
    }

    void relay_upstream_to_downstream() {
        auto self = shared_from_this();
        upstream_->async_read_some(
            net::MutableBuffer{up_to_down_buf_->data(), up_to_down_buf_->size()},
            [this, self](const net::Error& err, std::size_t n) {
                assert_on_owning_thread();
                if (!err.ok()) {
                    upstream_healthy_ = false;
                    close();
                    return;
                }
                downstream_->async_write(
                    net::ConstBuffer{up_to_down_buf_->data(), n},
                    [this, self](const net::Error& write_err, std::size_t written) {
                        assert_on_owning_thread();
                        if (!write_err.ok()) {
                            // downstream 쪽 실패 -- upstream은 여전히 정상.
                            close();
                            return;
                        }
                        metrics_.bytes_upstream_to_downstream.fetch_add(written, std::memory_order_relaxed);
                        relay_upstream_to_downstream();
                    });
            });
    }

    void close() {
        // 두 relay 방향 다 같은 shard 스레드에서 실행되므로, 이 함수가
        // 동시에 호출될 일은 없다 -- 평범한 bool guard로 충분.
        if (closing_) {
            return;
        }
        closing_ = true;

        downstream_->shutdown();
        downstream_->close();

        if (upstream_) {
            if (upstream_healthy_ && upstream_->is_open()) {
                // relay_upstream_to_downstream()은 relay 도중 항상
                // upstream_에 async_read_some을 하나 걸어둔 채로 다음
                // chunk(혹은 peer close)를 기다린다. 이 시점에도 그 read는
                // 여전히 pending 상태 -- 소켓을 풀에 넘기기 전에 취소하지
                // 않으면, 재사용된 소켓에 (이 낡은 것 + 다음 Session의)
                // read가 두 개 동시에 걸려서 둘 중 하나가 서로의 데이터를
                // 가로챌 수 있다. cancel()이 낡은 read를 에러로
                // 완료시키는데, close()의 조기 반환 guard(closing_) 덕분에
                // 이미 닫히는 중인 이 Session에서는 그냥 아무 일도 안
                // 일어난다.
                upstream_->cancel();

                // 아직 멀쩡한 upstream 연결은 그냥 닫지 않고 shard의
                // 풀에 반납한다 -- UpstreamManager 참고.
                upstream_manager_.release_connection(endpoint_index_, std::move(upstream_));
            } else {
                upstream_->shutdown();
                upstream_->close();
            }
        }

        // 다 쓴 버퍼는 버리지 않고 shard-local BufferPool의 free-list에
        // 반납해서 다음 Session이 재사용하게 한다.
        if (down_to_up_buf_) {
            buffer_pool_.release(std::move(down_to_up_buf_));
        }
        if (up_to_down_buf_) {
            buffer_pool_.release(std::move(up_to_down_buf_));
        }

        metrics_.on_close();
    }

    std::unique_ptr<net::ISocket> downstream_;
    net::IEventLoop& event_loop_;
    std::unique_ptr<net::ISocket> upstream_;
    UpstreamManager& upstream_manager_;
    std::size_t endpoint_index_ = 0;
    bool upstream_healthy_ = true;

    BufferPool& buffer_pool_;
    std::unique_ptr<std::vector<char>> down_to_up_buf_;
    std::unique_ptr<std::vector<char>> up_to_down_buf_;

    LocalMetrics& metrics_;
    bool closing_ = false;
};
