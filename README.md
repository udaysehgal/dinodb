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
| I/O model | Custom `ae` event loop over epoll/kqueue/select | A dedicated acceptor thread round-robins new connections across a fixed worker pool, each worker running its own epoll/kqueue loop |
| Memory layout | Adaptive small-object encodings per type (`listpack`, `intset`, `embstr`, etc.) that switch representation by size | One fixed-layout struct per key holding all possible container types; simpler, but higher per-key memory overhead |
| Data types | Strings, hashes, lists, sets, sorted sets, streams, bitmaps, HyperLogLog, geospatial | Strings, hashes, lists, sets, sorted sets |
| Persistence | RDB snapshots and/or an append-only file (AOF) with configurable fsync policy | Periodic/manual snapshots only, in its own simple length-prefixed binary format |
| Transactions / scripting | `MULTI`/`EXEC`, Lua scripting, functions | Not implemented |
| Pub/Sub | Yes | Not implemented |
| Replication / clustering | Primary-replica replication, Sentinel, Redis Cluster | Not implemented (single standalone process) |

In short: Redis gets its safety from running commands on a single thread;
DinoDB instead accepts real lock overhead in exchange for genuine
multi-core parallelism, on independent keys and on concurrent reads of
the same key. [Benchmarks](#benchmarks) show where that trade pays off
(server-bound work, ~1.4x Redis on 4 cores) and where it doesn't
(per-core efficiency). Redis's feature set (scripting,
replication, many more data types, tunable durability) is far broader —
DinoDB is a much smaller project focused on the core KV/data-structure
surface.

## Benchmarks

Measured with `scripts/bench.py` (wraps `redis-benchmark`) on an Apple M4
(4 performance + 6 efficiency cores), macOS, against Redis 8.10.2. Client
and server share the machine, so treat these as relative numbers. Each
cell is the median of 3 runs on a fresh server; throughput comes from
wall-clock time, and "CPU" is the server's user+sys CPU time per request.
50 clients, `redis-benchmark --threads 4`; `redis-io4` is Redis with
`--io-threads 4`, `dinodb-N` is DinoDB with `--workers N`.

**Server-bound reads** (`LRANGE` on one shared 600-element list, pipeline
depth 4). Each reply encodes 100 or 300 elements, so the work is in the
server. This is the case DinoDB's design targets: readers share a
`shared_lock`, so many cores serve the same hot key at once.

| Config | LRANGE 0 99 (req/s) | p99 ms | CPU µs/req | LRANGE 0 299 (req/s) | p99 ms | CPU µs/req |
|---|---:|---:|---:|---:|---:|---:|
| redis | 295,930 | 1.13 | 3.14 | 121,089 | 2.39 | 7.77 |
| redis-io4 | 332,873 | 0.98 | 5.47 | 140,240 | 2.00 | 9.95 |
| dinodb-1 | 249,823 | 1.46 | 3.92 | 105,226 | 3.58 | 9.46 |
| dinodb-2 | 332,926 | 1.10 | 5.54 | 150,797 | 2.30 | 12.32 |
| dinodb-4 | **399,452** | 0.77 | 8.12 | 173,735 | 1.60 | 16.24 |
| dinodb-8 | 399,389 | **0.63** | 8.92 | **181,575** | **1.24** | 17.27 |

With 4+ workers DinoDB serves 1.35–1.5x Redis's throughput (1.2–1.3x
Redis with I/O threads), with lower tail latency. Beyond 4 workers it
levels off: there are only 4 performance cores, and the benchmark client
needs them too.

**Small requests** (`SET`/`GET`/`INCR` on random keys from a 1M keyspace):

| Config | SET, P=1 | GET, P=1 | SET, P=16 | GET, P=16 | INCR, P=16 |
|---|---:|---:|---:|---:|---:|
| redis | 137,794 | 142,644 | 1,728,552 | 2,204,891 | 2,131,296 |
| redis-io4 | 137,794 | 137,787 | 1,998,467 | 2,368,189 | 2,131,400 |
| dinodb-1 | 147,985 | 147,996 | 1,390,442 | 1,599,068 | 1,522,788 |
| dinodb-2 | 137,785 | 142,704 | 1,728,453 | 2,368,011 | 1,776,511 |
| dinodb-4 | 147,975 | 147,986 | 2,204,967 | 2,367,638 | 2,131,030 |

Here every server hits the same ceiling: about 140–150k req/s without
pipelining and about 2.2–2.4M with it. That ceiling is the load generator
and loopback on one laptop, not the servers, so these workloads can't
separate them. They do show DinoDB scaling with workers (`SET` at P=16:
1.39M → 2.20M from 1 to 4 workers).

**Where DinoDB loses** (in the M4 runs above, which predate the
epoll/kqueue loop and the reply batching described below):

- **Per-core efficiency.** Single-threaded, DinoDB needed about 25–30%
  more CPU per request than Redis (e.g. 0.75 vs 0.57 µs for pipelined
  `SET`), and `dinodb-1` trailed Redis throughout. DinoDB pays for locks
  and per-request allocations that Redis's single event loop avoids.
- **CPU cost under light load.** Without pipelining, CPU per request
  grew with the worker count (6.6 → 13.8 → 23.7 µs for 1/2/4 workers at
  the same throughput). Each worker gets fewer requests per wake-up, so the
  fixed cost of waking up isn't amortized. If you aren't CPU-bound, fewer
  workers is cheaper. Redis's I/O threads show the same effect (15 µs vs 6).

**Since then** (Linux, 4 vCPUs, Redis 7.0, 1M keys, same harness). Not
directly comparable with the M4 numbers, but measured head to head on
one machine:

- Workers use `epoll`/`kqueue` instead of rebuilding a `poll()` set every
  iteration. On Linux, CPU per request without pipelining now stays flat
  at about 12–14 µs from 1 to 4 workers, close to Redis's 12.
- Workers read every ready connection before sending any reply, the order
  Redis uses. Sending each reply right away cost about 50% more kernel
  time per request than Redis for the same syscalls. Single-worker
  pipelined `GET` went from about 441k to 567k req/s, and server CPU from
  2.1 to 1.7 µs/req (Redis: about 1.8). With 4 workers, the
  median of three runs is about 15% less CPU per pipelined `GET` and 7%
  less per `SET`, with no change on `LRANGE`.
- The background expiry sweep skips shards that hold no TTLs. Before,
  a server with a million keys and no TTLs spent about 5% of a core
  walking them, even when idle.

Reproduce with:

```sh
brew install redis   # or apt install redis-server redis-tools
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
python3 scripts/bench.py                                   # small requests
python3 scripts/bench.py --tests lrange_100,lrange_300 --pipelines 4 \
    --requests 500000 --configs redis,redis-io4,dinodb-1,dinodb-2,dinodb-4,dinodb-8
```

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
