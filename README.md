# DinoDB

DinoDB is a small, from-scratch, Redis-protocol-compatible in-memory
key-value store written in modern C++. You can talk to it with `redis-cli`
or any standard Redis client library.

It was inspired by [FyroDB](https://github.com/Rana718/FyroDB) (formerly
FlashDB), a Rust in-memory database built around lock-free reads and
epoch-based reclamation. DinoDB borrows the general idea — an in-memory,
Redis-wire-compatible store built for concurrency — but is an independent
implementation in a different language with a deliberately different
architecture. See [Design differences](#design-differences-from-fyrodb)
below.

## Features

- RESP protocol server (works with `redis-cli` and standard Redis clients)
- Data types: strings, hashes, lists, sets, sorted sets
- Key management: `EXPIRE` / `TTL` / `PERSIST`, `KEYS` glob matching, `TYPE`
- Simple binary snapshot persistence (`SAVE`, periodic autosave, loaded at
  startup)
- Multi-threaded, sharded, concurrent store — no global lock

## Design differences from FyroDB

DinoDB is not a port of FyroDB's code (it doesn't have access to it beyond
its public README) — it's a separate implementation that makes different
tradeoffs on purpose:

| | FyroDB | DinoDB |
|---|---|---|
| Language | Rust | C++20 |
| Concurrency | Lock-free reads via epoch-based reclamation + seqlocks; per-key spinlocks for writes | Sharded store, one `std::shared_mutex` per shard; reads take a shared lock, writes an exclusive lock on just that shard |
| I/O model | epoll, one event loop pinned per core, `SO_REUSEPORT` kernel-level connection distribution | A dedicated acceptor thread hands off connections round-robin to a fixed worker pool, each worker running its own `poll()` loop — portable across Linux and macOS without conditional-compiled epoll/kqueue paths |
| Memory layout | Inline storage for short keys/values, adaptive reclamation, defragmentation | One struct per key (simpler ownership, higher per-key overhead, no defrag) |
| Data types | Strings, hashes, lists, sets, sorted sets, JSON, streams, bitmaps, HyperLogLog, geospatial | Strings, hashes, lists, sets, sorted sets |
| Persistence | RDB-compatible snapshots | Its own simple length-prefixed binary snapshot format |

In short: FyroDB chases maximum single-node throughput with lock-free
structures and per-core event loops. DinoDB optimizes for a small,
readable codebase with an easy-to-reason-about locking model, at some cost
to peak throughput.

## Building

Requires CMake 3.16+ and a C++20 compiler.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/dinodb` (the server) and `build/tests/dinodb_tests`
(the test suite).

## Running

```sh
./build/dinodb --port 7379
```

Then, from another terminal:

```sh
redis-cli -p 7379 SET greeting "hello dino"
redis-cli -p 7379 GET greeting
```

### Configuration

Set via flags or environment variables:

| Flag | Env var | Default | Meaning |
|---|---|---|---|
| `--port` | `DINODB_PORT` | `7379` | TCP port to listen on |
| `--bind` | `DINODB_BIND` | `0.0.0.0` | Bind address |
| `--workers` | `DINODB_WORKERS` | number of CPU cores | Worker thread count |
| `--rdb` | `DINODB_RDB_PATH` | `dinodb.rdb` | Snapshot file path |
| — | `DINODB_AUTOSAVE_SEC` | `300` | Autosave interval in seconds (`0` disables) |

## Supported commands

- **Generic:** `DEL`, `EXISTS`, `TYPE`, `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST`, `KEYS`, `FLUSHALL`/`FLUSHDB`, `DBSIZE`
- **Strings:** `SET` (`EX`/`PX`/`NX`/`XX`), `GET`, `GETSET`, `APPEND`, `STRLEN`, `MSET`, `MGET`, `INCR`, `DECR`, `INCRBY`, `DECRBY`
- **Hashes:** `HSET`, `HGET`, `HDEL`, `HEXISTS`, `HGETALL`, `HKEYS`, `HVALS`, `HLEN`, `HINCRBY`
- **Lists:** `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LRANGE`, `LINDEX`
- **Sets:** `SADD`, `SREM`, `SISMEMBER`, `SMEMBERS`, `SCARD`
- **Sorted sets:** `ZADD`, `ZSCORE`, `ZREM`, `ZCARD`, `ZINCRBY`, `ZRANGE` (with `WITHSCORES`)
- **Server:** `PING`, `ECHO`, `INFO`, `SAVE`/`BGSAVE`, `SELECT`, `CONFIG` (stub)

Not yet implemented (contributions welcome): pub/sub, transactions
(`MULTI`/`EXEC`), streams, bitmaps, HyperLogLog, geospatial commands, `SCAN`
cursors.

## Testing

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Docker

```sh
docker build -t dinodb .
docker run -p 7379:7379 dinodb
```

## License

MIT — see [LICENSE](LICENSE).
