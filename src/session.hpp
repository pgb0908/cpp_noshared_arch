#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>

#include "buffer_pool.hpp"
#include "local_metrics.hpp"

// A Connection: owned by exactly one shard for its whole lifetime. All I/O
// for this session runs on that shard's io_context thread only -- no
// cross-shard access, no locking.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket downstream_socket,
            boost::asio::ip::tcp::endpoint upstream_endpoint,
            BufferPool& buffer_pool,
            LocalMetrics& metrics)
        : downstream_(std::move(downstream_socket)),
          upstream_(downstream_.get_executor()),
          upstream_endpoint_(std::move(upstream_endpoint)),
          buffer_pool_(buffer_pool),
          metrics_(metrics) {}

    void start() {
        metrics_.on_accept();
        connect_upstream();
    }

private:
    void connect_upstream() {
        auto self = shared_from_this();
        upstream_.async_connect(upstream_endpoint_, [this, self](const boost::system::error_code& ec) {
            if (ec) {
                ++metrics_.upstream_connect_errors;
                close();
                return;
            }

            down_to_up_buf_ = buffer_pool_.acquire();
            up_to_down_buf_ = buffer_pool_.acquire();

            relay_downstream_to_upstream();
            relay_upstream_to_downstream();
        });
    }

    void relay_downstream_to_upstream() {
        auto self = shared_from_this();
        downstream_.async_read_some(
            boost::asio::buffer(*down_to_up_buf_),
            [this, self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) {
                    close();
                    return;
                }
                boost::asio::async_write(
                    upstream_, boost::asio::buffer(down_to_up_buf_->data(), n),
                    [this, self](const boost::system::error_code& write_ec, std::size_t written) {
                        if (write_ec) {
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
        upstream_.async_read_some(
            boost::asio::buffer(*up_to_down_buf_),
            [this, self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) {
                    close();
                    return;
                }
                boost::asio::async_write(
                    downstream_, boost::asio::buffer(up_to_down_buf_->data(), n),
                    [this, self](const boost::system::error_code& write_ec, std::size_t written) {
                        if (write_ec) {
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
        upstream_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        upstream_.close(ec);

        metrics_.on_close();
    }

    boost::asio::ip::tcp::socket downstream_;
    boost::asio::ip::tcp::socket upstream_;
    boost::asio::ip::tcp::endpoint upstream_endpoint_;

    BufferPool& buffer_pool_;
    std::unique_ptr<std::vector<char>> down_to_up_buf_;
    std::unique_ptr<std::vector<char>> up_to_down_buf_;

    LocalMetrics& metrics_;
    bool closing_ = false;
};
