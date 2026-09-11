#include "net/boost/resolver.hpp"

#include "net/boost/error.hpp"

namespace net::boost_asio {

BoostResolver::BoostResolver(::boost::asio::io_context& io_context) : resolver_(io_context) {}

std::pair<net::Error, std::vector<net::Endpoint>> BoostResolver::resolve(const std::string& host, uint16_t port) {
    ::boost::system::error_code ec;
    auto results = resolver_.resolve(host, std::to_string(port), ec);
    if (ec) {
        return {to_net_error(ec), {}};
    }

    std::vector<net::Endpoint> endpoints;
    endpoints.reserve(results.size());
    for (const auto& entry : results) {
        auto ep = entry.endpoint();
        endpoints.push_back(net::Endpoint{ep.address().to_string(), ep.port()});
    }
    return {net::Error::none(), std::move(endpoints)};
}

}  // namespace net::boost_asio
