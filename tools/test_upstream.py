#!/usr/bin/env python3
"""Persistent-connection test upstream server.

Unlike `python3 -m http.server` (which closes every connection after one
response), this keeps each accepted connection open and echoes back
`ack:<payload>` for every message received, so it can be used to verify
perCoreShard's upstream connection pooling: if pooling works, sending N
requests through the same shard/endpoint should log only 1 "ACCEPTED",
not N.

Usage:
    python3 tools/test_upstream.py [--host 127.0.0.1] [--port 9010]
"""
import argparse
import socket
import threading


def handle(conn, conn_id):
    while True:
        try:
            data = conn.recv(65536)
        except OSError as e:
            print(f"  conn#{conn_id} recv error: {e}", flush=True)
            return
        if not data:
            print(f"  conn#{conn_id} closed by peer", flush=True)
            return
        print(f"  conn#{conn_id} recv: {data!r}", flush=True)
        conn.sendall(b"ack:" + data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9010)
    args = parser.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    s.listen(50)
    print(f"listening on {args.host}:{args.port}", flush=True)

    count = 0
    while True:
        conn, addr = s.accept()
        count += 1
        print(f"ACCEPTED new connection #{count} from {addr}", flush=True)
        threading.Thread(target=handle, args=(conn, count), daemon=True).start()


if __name__ == "__main__":
    main()
