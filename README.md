# DinoDB

DinoDB is a small, from-scratch, Redis-protocol-compatible in-memory
key-value store written in modern C++. You can talk to it with `redis-cli`
or any standard Redis client library.

It speaks enough of the RESP wire protocol and command set to be a drop-in
for basic Redis workloads, but its internals are quite different — see
[Comparison with Redis](#comparison-with-redis) below.

## Features

- RESP protocol server (works with `redis-cli` and standard Redis clients)
- Data types: strings, hashes, lists, sets, sorted sets
- Key management: `EXPIRE` / `TTL` / `PERSIST`, `KEYS` glob matching, `TYPE`
- Simple binary snapshot persistence (`SAVE`, periodic autosave, loaded at
  startup; a corrupt snapshot stops startup instead of being overwritten)
- Multi-threaded, sharded, concurrent store — no global lock; every
  single-key command is atomic
- Lazy expiry plus a background sweep, so expired keys free their memory
- Redis's protocol limits (1M args, 512MB bulk strings) to stop runaway
  allocations from malformed clients

## Comparison with Redis

DinoDB is not a Redis fork or port — it's a small independent
implementation of a similar idea (an in-memory, RESP-speaking key-value
store) with a much smaller feature set and a different concurrency model:

| | Redis | DinoDB |
|---|---|---|
| Language | C | C++20 |
| Command execution | Single-threaded event loop (one thread runs all commands, so no locking is needed); optional multi-threaded socket I/O since Redis 6 | Multi-threaded throughout: keys are sharded across N `std::shared_mutex`-guarded buckets, and commands on different shards run genuinely in parallel |
| I/O model | Custom `ae` event loop over epoll/kqueue/select | A dedicated acceptor thread round-robins new connections across a fixed worker pool, each worker running its own `poll()` loop |
| Memory layout | Adaptive small-object encodings per type (`listpack`, `intset`, `embstr`, etc.) that switch representation by size | One fixed-layout struct per key holding all possible container types; simpler, but higher per-key memory overhead |
| Data types | Strings, hashes, lists, sets, sorted sets, streams, bitmaps, HyperLogLog, geospatial | Strings, hashes, lists, sets, sorted sets |
| Persistence | RDB snapshots and/or an append-only file (AOF) with configurable fsync policy | Periodic/manual snapshots only, in its own simple length-prefixed binary format |
| Transactions / scripting | `MULTI`/`EXEC`, Lua scripting, functions | Not implemented |
| Pub/Sub | Yes | Not implemented |
| Replication / clustering | Primary-replica replication, Sentinel, Redis Cluster | Not implemented (single standalone process) |

In short: Redis gets its safety from running commands on a single thread;
DinoDB instead accepts real lock overhead in exchange for genuine
multi-core parallelism on independent keys. Redis's feature set (scripting,
replication, many more data types, tunable durability) is far broader —
DinoDB is a much smaller project focused on the core KV/data-structure
surface.

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
| `--autosave` | `DINODB_AUTOSAVE_SEC` | `300` | Autosave interval in seconds (`0` disables; a final save still happens on clean shutdown) |

## Supported commands

- **Generic:** `DEL`, `EXISTS`, `TYPE`, `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST`, `KEYS`, `FLUSHALL`/`FLUSHDB`, `DBSIZE`
- **Strings:** `SET` (`EX`/`PX`/`NX`/`XX`), `GET`, `GETSET`, `APPEND`, `STRLEN`, `MSET`, `MGET`, `INCR`, `DECR`, `INCRBY`, `DECRBY`
- **Hashes:** `HSET`, `HGET`, `HDEL`, `HEXISTS`, `HGETALL`, `HKEYS`, `HVALS`, `HLEN`, `HINCRBY`
- **Lists:** `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LRANGE`, `LINDEX`
- **Sets:** `SADD`, `SREM`, `SISMEMBER`, `SMEMBERS`, `SCARD`
- **Sorted sets:** `ZADD`, `ZSCORE`, `ZREM`, `ZCARD`, `ZINCRBY`, `ZRANGE` (with `WITHSCORES`)
- **Server:** `PING`, `ECHO`, `INFO`, `SAVE`/`BGSAVE`, `SELECT`, `CONFIG` (stub)

Multi-key commands (`MSET`, `MGET`, `DEL`, `EXISTS`) apply each key
atomically but are not atomic as a whole, unlike in Redis. `SELECT` only
accepts database `0`.

Not yet implemented (contributions welcome): pub/sub, transactions
(`MULTI`/`EXEC`), streams, bitmaps, HyperLogLog, geospatial commands, `SCAN`
cursors.

## Testing

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The concurrency tests are most useful under a sanitizer build:

```sh
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DDINODB_SANITIZE=thread
cmake --build build-tsan -j && ./build-tsan/tests/dinodb_tests
```

`DINODB_SANITIZE` also accepts `address`, `undefined`, or
`address,undefined`. There is also an end-to-end stress test that drives
a running server from 16 concurrent clients and checks that no `INCR`
is lost:

```sh
./build/dinodb --port 7379 &
python3 scripts/stress.py 7379
```

CI (GitHub Actions) runs all of this on Linux and macOS, in release mode
and under TSan and ASan+UBSan, and also builds the Docker image.

## Docker

```sh
docker build -t dinodb .
docker run -p 7379:7379 dinodb
```

## License

MIT — see [LICENSE](LICENSE).
