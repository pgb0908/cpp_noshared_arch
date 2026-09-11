#!/usr/bin/env python3
"""Sequential TCP test client for exercising perCoreShard's relay/pooling path.

Opens `--count` connections to the proxy, one at a time (connect -> send ->
recv -> close, with a short pause between each), so the resulting behavior
on the upstream side (see tools/test_upstream.py) can be inspected to
confirm connection reuse.

Usage:
    python3 tools/test_client.py [--host 127.0.0.1] [--port 8080] [--count 3] [--message hello]
"""
import argparse
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--message", default="hello")
    parser.add_argument("--pause", type=float, default=0.3, help="seconds between requests")
    parser.add_argument("--timeout", type=float, default=3.0, help="per-connection socket timeout")
    args = parser.parse_args()

    for i in range(args.count):
        payload = f"{args.message}-{i}".encode()
        print(f"client {i} connecting to {args.host}:{args.port} ...", flush=True)
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(args.timeout)
        try:
            s.connect((args.host, args.port))
            s.sendall(payload)
            print(f"client {i} sent {payload!r}, waiting for recv...", flush=True)
            data = s.recv(4096)
            print(f"client {i} received: {data!r}", flush=True)
        except OSError as e:
            print(f"client {i} error: {e}", flush=True)
        finally:
            s.close()
            print(f"client {i} closed", flush=True)
        time.sleep(args.pause)


if __name__ == "__main__":
    main()
