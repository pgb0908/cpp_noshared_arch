#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>

#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// A Connection: owned by exactly one shard for its whole lifetime. All I/O
// for this session runs on that shard's io_context thread only -- no
// cross-shard access, no locking.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket downstream_socket,
            UpstreamManager& upstream_manager,
            BufferPool& buffer_pool,
            LocalMetrics& metrics)
        : downstream_(std::move(downstream_socket)),
          upstream_manager_(upstream_manager),
          buffer_pool_(buffer_pool),
          metrics_(metrics) {}

    void start() {
        metrics_.on_accept();
        connect_upstream();
    }

private:
    void connect_upstream() {
        endpoint_index_ = upstream_manager_.select_endpoint();

        if (auto pooled = upstream_manager_.acquire_pooled_connection(endpoint_index_)) {
            upstream_ = std::move(pooled);
            on_upstream_ready();
            return;
        }

        upstream_ = std::make_unique<boost::asio::ip::tcp::socket>(downstream_.get_executor());
        auto self = shared_from_this();
        upstream_->async_connect(
            upstream_manager_.endpoint_address(endpoint_index_), [this, self](const boost::system::error_code& ec) {
                if (ec) {
                    ++metrics_.upstream_connect_errors;
                    upstream_healthy_ = false;
                    close();
                    return;
                }
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
        downstream_.async_read_some(
            boost::asio::buffer(*down_to_up_buf_),
            [this, self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) {
                    // Downstream side failed -- upstream connection may
                    // still be healthy, so don't mark it unhealthy here.
                    close();
                    return;
                }
                boost::asio::async_write(
                    *upstream_, boost::asio::buffer(down_to_up_buf_->data(), n),
                    [this, self](const boost::system::error_code& write_ec, std::size_t written) {
                        if (write_ec) {
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
            boost::asio::buffer(*up_to_down_buf_),
            [this, self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) {
                    upstream_healthy_ = false;
                    close();
                    return;
                }
                boost::asio::async_write(
                    downstream_, boost::asio::buffer(up_to_down_buf_->data(), n),
                    [this, self](const boost::system::error_code& write_ec, std::size_t written) {
                        if (write_ec) {
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

        boost::system::error_code ec;
        downstream_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        downstream_.close(ec);

        if (upstream_) {
            if (upstream_healthy_ && upstream_->is_open()) {
                // relay_upstream_to_downstream() always keeps one
                // async_read_some pending on upstream_ while relaying, to
                // detect the next chunk (or peer close). That read is still
                // outstanding at this point -- cancel it before handing the
                // socket to the pool, otherwise a reused connection would
                // end up with two pending reads (this stale one + the next
                // Session's), and either one can steal the other's data.
                // cancel() completes the stale read with operation_aborted,
                // which close()'s early-return guard (closing_) then makes
                // a harmless no-op for this already-closing Session.
                upstream_->cancel(ec);

                // Return the still-good upstream connection to the shard's
                // pool instead of tearing it down -- see UpstreamManager.
                upstream_manager_.release_connection(endpoint_index_, std::move(upstream_));
            } else {
                upstream_->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                upstream_->close(ec);
            }
        }

        metrics_.on_close();
    }

    boost::asio::ip::tcp::socket downstream_;
    std::unique_ptr<boost::asio::ip::tcp::socket> upstream_;
    UpstreamManager& upstream_manager_;
    std::size_t endpoint_index_ = 0;
    bool upstream_healthy_ = true;

    BufferPool& buffer_pool_;
    std::unique_ptr<std::vector<char>> down_to_up_buf_;
    std::unique_ptr<std::vector<char>> up_to_down_buf_;

    LocalMetrics& metrics_;
    bool closing_ = false;
};
