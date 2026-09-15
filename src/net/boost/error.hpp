#pragma once

#include <boost/system/error_code.hpp>

#include "net/types.hpp"

// 이 프로젝트에서 <boost/asio.hpp>(혹은 boost::asio/boost::system 헤더)를
// 직접 include해도 되는 파일은 net/boost/ 아래뿐이다 -- 그 외 모든 곳은
// net/ 인터페이스에만 의존한다.
namespace net::boost_asio {

inline Error to_net_error(const boost::system::error_code& ec) {
    if (!ec) {
        return Error::none();
    }
    return Error{ec.value(), ec.message()};
}

}  // namespace net::boost_asio
