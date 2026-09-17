#pragma once

#include "net/http/types.hpp"
#include "net/types.hpp"

// HTTP 메시지 파서: I/O와 완전히 분리된 순수 상태 기계다. 실제 소켓
// read는 net::ISocket이 담당하고, 그렇게 읽은 raw 바이트를 여기에
// feed()로 먹인다 -- 그래서 Beast 같은 구현체도 스트림 개념 없이
// 순수 파싱 엔진으로만 감쌀 수 있다 (자세한 배경은 doc/plan.md 참고).
//
// 사용 패턴 (스트리밍 relay 루프):
//   while (아직 다 못 읽음) {
//       raw = downstream에서 read
//       offset = 0
//       while (offset < raw.size()) {
//           consumed = parser.feed(raw[offset:])
//           offset += consumed
//           n = parser.read_body(scratch_buf)   // 파싱된 body를 꺼내서
//           if (n > 0) upstream으로 write(scratch_buf[0:n])  // 바로 relay
//       }
//       if (parser.message_done()) break
//   }
//
// feed()가 raw를 전부 소비하지 못하고 일부만 소비했다면(반환값 <
// raw.size()), 파서 내부 body 스크래치 버퍼가 가득 찼다는 뜻 --
// read_body()로 비운 뒤 남은 raw를 다시 feed()해야 한다.
namespace net::http {

class IRequestParser {
public:
    virtual ~IRequestParser() = default;

    virtual std::size_t feed(ConstBuffer raw) = 0;

    virtual bool header_done() const = 0;
    // header_done() 이후에만 유효.
    virtual const RequestHead& head() const = 0;

    // 파싱된 body 바이트를 out으로 꺼내간다 (최대 out.size만큼).
    virtual std::size_t read_body(MutableBuffer out) = 0;

    virtual bool message_done() const = 0;
    virtual bool has_error() const = 0;
    virtual const Error& error() const = 0;
};

class IResponseParser {
public:
    virtual ~IResponseParser() = default;

    virtual std::size_t feed(ConstBuffer raw) = 0;

    virtual bool header_done() const = 0;
    virtual const ResponseHead& head() const = 0;

    virtual std::size_t read_body(MutableBuffer out) = 0;

    virtual bool message_done() const = 0;
    virtual bool has_error() const = 0;
    virtual const Error& error() const = 0;
};

}  // namespace net::http
