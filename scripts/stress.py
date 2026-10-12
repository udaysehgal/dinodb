#!/usr/bin/env python3
"""Concurrency smoke test: many clients INCR one counter and LPUSH/LPOP one
list in parallel; the final counter must equal the number of INCRs sent."""
import socket, sys, threading

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 7379
THREADS, OPS = 16, 2000


def cmd(*args):
    out = f"*{len(args)}\r\n"
    for a in args:
        a = str(a)
        out += f"${len(a.encode())}\r\n{a}\r\n"
    return out.encode()


def read_reply(f):
    line = f.readline()
    t, rest = line[:1], line[1:-2]
    if t in (b"+", b"-", b":"):
        return rest
    if t == b"$":
        n = int(rest)
        return None if n < 0 else f.read(n + 2)[:-2]
    if t == b"*":
        return [read_reply(f) for _ in range(int(rest))]
    raise RuntimeError(f"bad reply {line!r}")


def worker():
    s = socket.create_connection((HOST, PORT))
    f = s.makefile("rb")
    for i in range(OPS):
        s.sendall(cmd("INCR", "ctr") + cmd("RPUSH", "lst", i) + cmd("LPOP", "lst"))
        for _ in range(3):
            read_reply(f)
    s.close()


s = socket.create_connection((HOST, PORT))
f = s.makefile("rb")
s.sendall(cmd("DEL", "ctr", "lst")); read_reply(f)
ts = [threading.Thread(target=worker) for _ in range(THREADS)]
for t in ts: t.start()
for t in ts: t.join()
s.sendall(cmd("GET", "ctr")); got = int(read_reply(f))
want = THREADS * OPS
print(f"counter={got} expected={want} {'OK' if got == want else 'LOST UPDATES'}")
sys.exit(0 if got == want else 1)
