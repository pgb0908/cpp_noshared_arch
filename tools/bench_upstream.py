#!/usr/bin/env python3
"""wrk 부하테스트용 업스트림. tools/test_upstream.py(raw TCP echo, connection
pooling 검증용)와 달리 이건 실제 HTTP 응답을 즉시 돌려준다.

http.server.HTTPServer는 요청을 한 번에 하나씩만 처리해서(단일 스레드) 부하가
조금만 올라가도 업스트림 자체가 병목이 돼버린다. ThreadingHTTPServer로 동시
커넥션을 처리해야 "게이트웨이가 느린 건지 업스트림이 느린 건지"를 구분할 수
있다 (doc/benchmark-report.md 참고).

Usage:
    python3 tools/bench_upstream.py [port]
"""
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BODY = b"OK"


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.1 기본값(keep-alive)을 그대로 쓴다 -- 게이트웨이(HttpSession)가
    # upstream 커넥션도 재사용하므로(doc/plan.md "keep-alive 도입" 참고),
    # 이 서버가 매 응답 후 바로 닫아버리면 pool 재사용 경로를 테스트할 수
    # 없다. keep-alive 없이 매번 새 연결을 강제하고 싶으면 HTTP/1.0으로
    # 바꿔서 실행.
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, fmt, *args):
        pass  # 부하테스트 중 로그 I/O 자체가 병목이 되지 않도록 끔


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    print(f"listening on 127.0.0.1:{port}", flush=True)
    server.serve_forever()
