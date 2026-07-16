#!/usr/bin/env python3
import socket
import struct
import sys
import time


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <port> [--verbose]", file=sys.stderr)
        sys.exit(1)
    port = int(sys.argv[1])
    verbose = "--verbose" in sys.argv

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.settimeout(0.5)
    s.bind(("0.0.0.0", port))
    print(f"tsync-server listening on port {port}", flush=True)

    try:
        while True:
            try:
                data, addr = s.recvfrom(1024)
            except socket.timeout:
                continue
            if len(data) < 12 or data[:4] != b"PSYN":
                continue
            t1 = struct.unpack("<Q", data[4:12])[0]
            t2 = int(time.time() * 1_000_000)
            t3 = int(time.time() * 1_000_000)
            rsp = struct.pack("<4sQQQ", b"PSYN", t1, t2, t3)
            s.sendto(rsp, addr)
            if verbose:
                print(f"sync from {addr[0]}:{addr[1]} t1={t1}", flush=True)
    except KeyboardInterrupt:
        print("", flush=True)


if __name__ == "__main__":
    main()
