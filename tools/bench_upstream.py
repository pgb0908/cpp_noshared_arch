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
    # keep-alive를 켜두면(HTTP/1.1 기본값) 스레드가 다음 요청을 기다리며
    # 계속 살아있는데, 게이트웨이(HttpSession)는 keep-alive를 안 하므로
    # 요청 하나 처리 후 항상 새 연결을 닫아버린다. HTTP/1.0으로 고정해서
    # 이 서버도 매 응답 후 바로 닫도록 맞춘다.
    protocol_version = "HTTP/1.0"

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
