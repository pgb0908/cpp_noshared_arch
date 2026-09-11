#pragma once

#include <cassert>
#include <memory>
#include <vector>

#include "net/event_loop.hpp"
#include "net/socket.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// A Connection: owned by exactly one shard for its whole lifetime. All I/O
// for this session runs on that shard's event loop thread only -- no
// cross-shard access, no locking.
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
    // Verifies this callback is actually running on the shard thread that
    // owns this Session. Added after a real bug was found here: a socket
    // accepted via Listener stayed bound to the Listener's event loop, so
    // every Session I/O callback silently ran on the Listener thread
    // instead of the owning shard's -- see net/event_loop.hpp's
    // adopt_socket() doc comment. This assert is the regression guard for
    // exactly that class of bug; only active in debug builds (see
    // CMakeLists.txt's default build type note).
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
                    ++metrics_.upstream_connect_errors;
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
                    // Downstream side failed -- upstream connection may
                    // still be healthy, so don't mark it unhealthy here.
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
                        metrics_.bytes_downstream_to_upstream += written;
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
                            // Downstream side failed -- upstream is still fine.
                            close();
                            return;
                        }
                        metrics_.bytes_upstream_to_downstream += written;
                        relay_upstream_to_downstream();
                    });
            });
    }

    void close() {
        // Both relay directions run on the same shard thread, so this is
        // never called concurrently -- a plain bool guard is sufficient.
        if (closing_) {
            return;
        }
        closing_ = true;

        downstream_->shutdown();
        downstream_->close();

        if (upstream_) {
            if (upstream_healthy_ && upstream_->is_open()) {
                // relay_upstream_to_downstream() always keeps one
                // async_read_some pending on upstream_ while relaying, to
                // detect the next chunk (or peer close). That read is still
                // outstanding at this point -- cancel it before handing the
                // socket to the pool, otherwise a reused connection would
                // end up with two pending reads (this stale one + the next
                // Session's), and either one can steal the other's data.
                // cancel() completes the stale read with an error, which
                // close()'s early-return guard (closing_) then makes a
                // harmless no-op for this already-closing Session.
                upstream_->cancel();

                // Return the still-good upstream connection to the shard's
                // pool instead of tearing it down -- see UpstreamManager.
                upstream_manager_.release_connection(endpoint_index_, std::move(upstream_));
            } else {
                upstream_->shutdown();
                upstream_->close();
            }
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
