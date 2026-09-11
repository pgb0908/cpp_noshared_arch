#pragma once

#include <boost/system/error_code.hpp>

#include "net/types.hpp"

// The only files in this project allowed to include <boost/asio.hpp> (or
// any boost::asio/boost::system header) directly are the ones under
// net/boost/ -- everything else depends only on the net/ interfaces.
namespace net::boost_asio {

inline Error to_net_error(const boost::system::error_code& ec) {
    if (!ec) {
        return Error::none();
    }
    return Error{ec.value(), ec.message()};
}

}  // namespace net::boost_asio
