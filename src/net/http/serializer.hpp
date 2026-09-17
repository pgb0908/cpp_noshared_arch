#pragma once

#include "net/http/types.hpp"
#include "net/types.hpp"

// HTTP 메시지 시리얼라이저: parser와 대칭으로, I/O와 분리된 순수 상태
// 기계다. 실제 소켓 write는 net::ISocket이 담당하고, 여기는 "다음에
// 내보낼 바이트"만 만들어준다.
//
// 사용 패턴 (스트리밍 relay 루프):
//   serializer.start(head)
//   loop {
//       n = serializer.pull(out_buf)
//       if (n > 0) downstream/upstream으로 write(out_buf[0:n])
//       if (serializer.done()) break
//       if (n == 0) {
//           // 더 내보낼 게 없어서 멈춘 것 -- body가 필요하다는 뜻
//           chunk = 반대편에서 다음 body chunk를 read
//           serializer.provide_body(chunk, is_last)
//       }
//   }
// body가 아예 없는 메시지(GET, Content-Length: 0 등)라면 start() 직후
// provide_body({nullptr, 0}, true)를 한 번 호출해야 한다 -- 안 그러면
// done()이 영원히 true가 되지 않는다.
namespace net::http {

class IRequestSerializer {
public:
    virtual ~IRequestSerializer() = default;

    virtual void start(const RequestHead& head) = 0;

    // relay 루프가 읽어온 body chunk를 등록한다. is_last=true면 이게
    // 이 메시지의 마지막 chunk (message 종료).
    virtual void provide_body(ConstBuffer chunk, bool is_last) = 0;

    // 지금 내보내야 할 바이트를 out에 채워 넣는다. 반환값: 채운 바이트 수.
    // 0을 반환했는데 done()도 false라면 provide_body()가 필요하다는 뜻.
    virtual std::size_t pull(MutableBuffer out) = 0;

    virtual bool done() const = 0;
};

class IResponseSerializer {
public:
    virtual ~IResponseSerializer() = default;

    virtual void start(const ResponseHead& head) = 0;
    virtual void provide_body(ConstBuffer chunk, bool is_last) = 0;
    virtual std::size_t pull(MutableBuffer out) = 0;
    virtual bool done() const = 0;
};

}  // namespace net::http
