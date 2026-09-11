#pragma once

#include <boost/asio.hpp>

#include "net/resolver.hpp"

namespace net::boost_asio {

class BoostResolver : public net::IResolver {
public:
    explicit BoostResolver(::boost::asio::io_context& io_context);

    std::pair<net::Error, std::vector<net::Endpoint>> resolve(const std::string& host, uint16_t port) override;

private:
    ::boost::asio::ip::tcp::resolver resolver_;
};

}  // namespace net::boost_asio
