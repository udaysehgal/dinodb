# Building DinoDB: a sharded, multi-threaded key-value store in C++

*~20 minute read · systems, C++, concurrency, networking*

---

## Why build another Redis

Redis's core trick is deceptively simple: run every command on a single
thread. No locks, no races, no torn reads — the entire data structure
engine gets to pretend concurrency doesn't exist, because from its point
of view it doesn't. That buys enormous simplicity, and for years it was
also fast enough, because a single core can push millions of simple
operations per second and the network stack is usually the real
bottleneck anyway.

But "fast enough" has a ceiling, and the ceiling is one core. If your
workload is CPU-bound — big `ZRANGE` scans, large hash operations, many
concurrent clients each doing real work — a single-threaded engine simply
cannot use the seven other cores sitting idle next to it. Redis's own
answer, over the years, has been to lean on clustering (shard across
*processes*, not threads) rather than take on internal concurrency.

DinoDB asks a narrower question: what does an in-memory KV store look
like if you *do* let multiple threads execute commands at once, and pay
for that with real synchronization instead of getting it for free? Not
lock-free, epoch-reclaimed, seqlock-validated concurrency — that's a
different, much harder project — but the simplest thing that can
plausibly work: **partition the keyspace into shards, put a
reader-writer lock on each one, and let unrelated keys never see each
other's locks.**

This post walks through how that's built, from the wire protocol up to
the on-disk snapshot format.

## Design goals

1. **Speak RESP** — `redis-cli` and any standard Redis client library
   should be able to talk to it with zero adaptation.
2. **Real parallelism, not the illusion of it** — two clients touching
   two different keys should never wait on each other, on any code path.
3. **A codebase one person can hold in their head** — every subsystem
   (parser, store, dispatcher, reactor, persistence) is small, has one
   job, and is unit-testable in isolation.
4. **Boring, portable primitives** — `std::shared_mutex`, `poll()`,
   plain TCP sockets. No lock-free tricks, no platform-specific event
   APIs, no external dependencies beyond the C++ standard library.

## Architecture overview

Four layers, each one only aware of the layer directly below it:

```mermaid
flowchart TB
    subgraph net["Networking"]
        A["Acceptor thread\naccept() loop"]
        W1["Worker 0\npoll() loop"]
        W2["Worker 1\npoll() loop"]
        WN["Worker N-1\npoll() loop"]
    end
    subgraph proto["Protocol"]
        P["RESP streaming parser\n+ reply encoder"]
    end
    subgraph exec["Command dispatch"]
        D["dispatch(cmd, args)\n~40 command handlers"]
    end
    subgraph data["Sharded store"]
        S0["Shard 0\nshared_mutex + hashmap"]
        S1["Shard 1\nshared_mutex + hashmap"]
        SN["Shard 31\nshared_mutex + hashmap"]
    end

    A -- "round-robin new fd" --> W1
    A --> W2
    A --> WN
    W1 --> P
    W2 --> P
    WN --> P
    P --> D
    D --> S0
    D --> S1
    D --> SN
```

- **Networking** owns file descriptors and nothing else. One acceptor
  thread blocks on `accept()`; every new connection is handed to one of
  N worker threads round-robin. Each worker runs its own `poll()` loop
  over the connections it owns — no connection is ever touched by more
  than one thread, so the networking layer itself needs zero locks.
- **Protocol** turns bytes into requests and replies back into bytes.
  It knows nothing about what a `SET` does.
- **Dispatch** knows what every command does, but not how bytes got to
  it or where the store's locks live.
- **The sharded store** is the only place a mutex is ever taken.

Each layer being ignorant of the ones above it is what makes the whole
thing unit-testable without spinning up a socket — the test suite calls
`dispatch()` directly against a `Store` and never touches the network at
all.

## The store: sharding instead of one big lock

The naive version of a thread-safe hash map is `std::mutex` plus
`std::unordered_map`. It works, and it also means every single command —
`GET`, `SET`, `TTL`, doesn't matter — serializes behind the same lock. At
that point the extra worker threads are pure overhead: you've built a
single-threaded database with more context-switching.

DinoDB instead fixes the shard count at 32 and hashes every key into
exactly one of them:

```cpp
class Shard {
public:
    std::shared_mutex mtx;
    std::unordered_map<std::string, Entry> map;
};

Shard& Store::shard_for(const std::string& key) {
    size_t h = hasher_(key);
    return *shards_[h % shards_.size()];
}
```

Every read (`GET`, `HGET`, `TTL`, ...) takes a `shared_lock` on exactly
one shard; every write (`SET`, `DEL`, `LPUSH`, ...) takes a
`unique_lock` on exactly one shard. Two clients hitting different shards
never block each other — including two writers. Two clients hitting the
*same* key still serialize, same as Redis's single global thread would,
but now that's the exception, not the rule:

```mermaid
sequenceDiagram
    participant C1 as Client A: SET foo 1
    participant C2 as Client B: SET bar 2
    participant SF as Shard(foo)
    participant SB as Shard(bar)
    C1->>SF: unique_lock
    C2->>SB: unique_lock
    Note over SF,SB: Different shards -> no contention, run in parallel
    SF-->>C1: OK
    SB-->>C2: OK
```

With 32 shards and a reasonable hash distribution, two random keys land
in the same shard about 1-in-32 of the time — contention drops roughly
linearly with shard count, at the cost of 32 mutexes' worth of memory
and a hash computation per lookup. That's the whole trade: DinoDB is
choosing *coarse, cheap, well-understood* parallelism over *fine-grained,
expensive, hard-to-verify* parallelism.

### What actually lives in a shard

Rather than a tagged union of heap-allocated containers, every key maps
to one fixed-layout `Value` struct big enough to hold any of the five
supported types:

```cpp
struct Value {
    ValueType type;                              // which field is "live"
    std::string str;                              // String
    std::unordered_map<std::string, std::string> hash;  // Hash
    std::deque<std::string> list;                  // List
    std::unordered_set<std::string> set;            // Set
    std::unordered_map<std::string, double> zscores; // ZSet: member -> score
    std::set<std::pair<double, std::string>> zorder; // ZSet: (score, member)
};

struct Entry {
    Value value;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};
```

This is a deliberate simplification versus e.g. Redis's `listpack`/
`intset`/`embstr` family of size-tuned encodings, or an epoch-reclaimed
structure that hands back memory to other threads lazily. Every key
here costs roughly `sizeof(Value)` regardless of whether it holds a
3-byte string or a 10,000-entry hash — noticeably more memory overhead
per key, in exchange for an ownership model simple enough that there is
no reclamation problem to solve at all: a `std::unique_lock` is held,
the struct is mutated in place, the lock is released. No epochs, no
hazard pointers, no deferred frees.

Sorted sets get two structures instead of one specifically so that
`ZRANGE` (score-ordered) doesn't have to sort on every call: `zscores`
answers "what's this member's score" in O(1), and `zorder` — a
`std::set<std::pair<double, std::string>>` — keeps the score-sorted view
maintained incrementally on every insert/update/remove. Ordering by the
`(score, member)` pair, rather than a `multimap` keyed on score alone,
buys two things: Redis's tie-break rule (members with equal scores sort
lexicographically) falls out for free, and removing a member's stale
entry is a single O(log n) `erase` instead of a scan over every member
sharing that score:

```cpp
void zset_put(const std::string& member, double score) {
    auto it = zscores.find(member);
    if (it != zscores.end()) {
        zorder.erase({it->second, member});  // drop the stale (old_score, member)
        it->second = score;
    } else {
        zscores.emplace(member, score);
    }
    zorder.emplace(score, member);
}
```

### Holding the lock for the whole command

Taking "one lock per command" literally matters. An early version of
the `Store` API looked like this:

```cpp
Entry* Store::get_mut(const std::string& key) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    ...
    return &it->second;   // lock is released here...
}
// ...so the caller mutates the entry with no lock held at all.
```

It returned a pointer into the shard *after* the lock had gone out of
scope, so every command handler read and mutated values completely
unsynchronized. Under load that showed up as lost `INCR`s (31,992 out of
32,000 in a 16-client test) and, under ThreadSanitizer, as a crash
inside `std::deque` while two `LPUSH`es raced on the same list.

The fix is to never hand out a pointer that can outlive its lock. The
`Store` now takes the work to the data instead: callers pass a lambda,
and it runs while the shard lock is held:

```cpp
// Shared lock; e is nullptr if the key is missing or expired.
c.store.read(key, [](const Entry* e) { ... });

// Exclusive lock; the whole read-modify-write is atomic.
c.store.write(key, [&](Slot& slot) {
    Entry* e = slot.get();               // nullptr if absent
    ...
    slot.get_or_create(ValueType::String).value.str = std::to_string(cur);
});
```

`Slot` is a small handle to one key's position in its shard. It also
takes care of a Redis rule that is easy to forget: when its destructor
runs (still under the lock), a list, set, hash or sorted set that a
command just emptied is deleted, so `LPOP`ing the last element really
does make the key disappear.

### Expiry: lazy, plus a background sweep

Every access checks `expires_at` against `now()` under the shard lock it
already holds and treats a stale entry as missing (write paths erase it
on the spot). On its own that means a key nobody ever reads again would
outlive its TTL in memory forever, so a housekeeping thread also sweeps
one shard every 100ms, erasing whatever has expired. A full pass over all
32 shards takes about three seconds, and the sweep never holds more than
one shard's lock at a time.

## The wire: a streaming RESP parser

Real Redis clients send **RESP multibulk arrays** — `*3\r\n$3\r\nSET\r\n...`
— but the parser also accepts plain space-separated inline commands
(`PING\r\n`), which makes manual testing with `nc` painless. TCP gives
you a byte stream, not message boundaries, so the parser has to cope
with a request arriving in pieces across multiple `recv()` calls, or
multiple requests arriving in a single `recv()`:

```cpp
class Parser {
public:
    void feed(const char* data, size_t len);       // append raw bytes
    std::optional<Request> next();                  // pop one complete request, if any
private:
    std::string buf_;
    size_t pos_ = 0;   // parse cursor; buf_ is compacted periodically, not on every call
};
```

`next()` is the whole contract: call it in a loop after every `feed()`.
If a complete request is sitting in the buffer it comes back immediately;
if the buffer ends mid-length-prefix or mid-bulk-string, it returns
`nullopt` and leaves `pos_` untouched, so the next `feed()` picks up
exactly where parsing left off.

Because the parser buffers whatever the client claims is coming, it also
enforces Redis's default limits (at most 1M arguments per request, 512MB
per bulk string, 64KB per inline command) and never sizes an allocation
from a client-supplied count. Anything malformed or over the limits puts
the parser into a sticky error state; the server replies with
`-ERR Protocol error: ...` and closes the connection. The buffer is only compacted (bytes
before `pos_` erased) once it's grown past a threshold, so a stream of
small pipelined requests doesn't pay a `memmove` per request.

```mermaid
sequenceDiagram
    participant Sock as socket recv()
    participant Buf as Parser buffer
    participant Disp as dispatch()
    Sock->>Buf: feed(bytes) [partial: "*3\r\n$3\r\nSET\r\n$1"]
    Buf-->>Sock: next() -> nullopt (incomplete)
    Sock->>Buf: feed(bytes) [rest: "\r\nk\r\n$1\r\nv\r\n"]
    Buf-->>Disp: next() -> ["SET", "k", "v"]
    Disp-->>Buf: encoded reply appended to outbuf
    Buf-->>Sock: next() -> nullopt (buffer drained)
```

## The reactor: one acceptor, N pollers, zero shared state per connection

DinoDB deliberately does **not** use `epoll`/`kqueue`/`SO_REUSEPORT`
per-core fan-out the way high-end event-driven servers usually do.
`poll()` is portable POSIX, present and identical on Linux and macOS,
and simple enough to get right without conditionally-compiled platform
code. The trade is a linear rescan of each worker's fd set every
iteration instead of O(1) readiness notification — a real cost at very
high connection counts, and a non-issue at the scale this project
targets.

```mermaid
flowchart LR
    Client1((client)) -->|connect| Acceptor
    Client2((client)) -->|connect| Acceptor
    Client3((client)) -->|connect| Acceptor
    Acceptor["Acceptor thread\npoll() + accept()"]
    Acceptor -->|fd, round-robin| Q0["Worker 0 pending queue"]
    Acceptor -->|fd, round-robin| Q1["Worker 1 pending queue"]
    Q0 --> P0["Worker 0: poll() over its own fds"]
    Q1 --> P1["Worker 1: poll() over its own fds"]
```

Ownership is the key invariant: once the acceptor hands a file
descriptor to worker *i*'s pending queue, only worker *i* ever calls
`recv()`, `send()`, or `close()` on it. There is exactly one `std::mutex`
in the entire networking layer — guarding each worker's pending-fd
queue — and it's held only for the microseconds it takes to push or
drain a `std::deque<int>`. After queueing an fd, the acceptor writes one
byte to that worker's wake-up pipe, which sits in the worker's `poll()`
set, so the worker picks the connection up immediately. No connection state is ever shared between
threads, so the per-connection buffers (`resp::Parser`, the pending
output bytes) don't need any synchronization at all.

Each worker's loop, per iteration:

1. Drain its pending queue into its local connection map (new fds only).
2. Build a `pollfd` list: `POLLIN` always, plus `POLLOUT` if that
   connection has unsent reply bytes buffered.
3. `poll()` over those fds plus the wake-up pipe. The 200ms timeout
   only bounds how quickly shutdown is noticed. An earlier version had no
   wake-up pipe, so a new connection sat unnoticed until the timeout
   expired and every client's first reply took ~200ms. With the pipe it
   takes ~0.1ms.
4. For each ready fd: drain `recv()` until `EAGAIN`, feed every byte to
   that connection's parser, `dispatch()` every complete request that
   falls out, append the encoded reply to the connection's outbound
   buffer, then try to flush that buffer with `send()` (partial sends
   are just remembered and retried next iteration via `POLLOUT`).

## Command dispatch

`dispatch()` is a case-insensitive hash-table lookup over ~60 commands
covering five data types plus key/server management. No command object
graph, no visitor pattern — just a function that takes a `Store&` and a
parsed request and returns an already-RESP-encoded reply string:

| Category | Commands |
|---|---|
| Generic | `DEL` `EXISTS` `TYPE` `EXPIRE` `PEXPIRE` `TTL` `PTTL` `PERSIST` `KEYS` `FLUSHALL` `DBSIZE` |
| Strings | `SET` (`EX`/`PX`/`NX`/`XX`) `GET` `GETSET` `APPEND` `STRLEN` `MSET` `MGET` `INCR` `DECR` `INCRBY` `DECRBY` |
| Hashes | `HSET` `HGET` `HDEL` `HEXISTS` `HGETALL` `HKEYS` `HVALS` `HLEN` `HINCRBY` |
| Lists | `LPUSH` `RPUSH` `LPOP` `RPOP` `LLEN` `LRANGE` `LINDEX` |
| Sets | `SADD` `SREM` `SISMEMBER` `SMEMBERS` `SCARD` |
| Sorted sets | `ZADD` `ZSCORE` `ZREM` `ZCARD` `ZINCRBY` `ZRANGE` |
| Server | `PING` `ECHO` `INFO` `SAVE`/`BGSAVE` `SELECT` `CONFIG` (stub) |

Each table entry carries a Redis-style arity (`3` means exactly three
arguments including the command name, `-3` means at least three), so
argument-count errors are produced in one place with Redis's exact
wording. Every handler then follows the same shape: parse and validate
arguments up front (so a bad `ZADD` score can't leave a half-applied
update), then do all its work on a key inside a single `Store::read` or
`Store::write` call and encode the reply. Multi-key commands (`MSET`,
`MGET`, `DEL`) take one shard lock per key in turn, so each key's update
is atomic but, unlike in Redis, the command as a whole is not.

### Asymptotic cost per operation

| Command | Cost | Why |
|---|---|---|
| `GET` / `SET` / `HGET` / `SADD` | O(1) amortized | hash lookup in one shard |
| `LPUSH` / `RPOP` | O(1) | `std::deque` push/pop at either end |
| `LRANGE start stop` | O(k) | k = size of the returned slice |
| `ZADD` / `ZINCRBY` | O(log n) | `std::set` erase + insert, n = zset size |
| `ZRANGE start stop` | O(start + k) | walks `zorder` from the front to `start`, then k results |
| `KEYS pattern` | O(total keys) | full scan across all 32 shards |

`ZRANGE`'s linear walk (rather than O(log n + k) via an order-statistics
tree) and `KEYS`'s full scan are the two honest weak points in the
current design — both are correct and simple, neither is what a
production KV store would ship.

## Persistence: a snapshot format, not a log

DinoDB does not implement an append-only file. It has one persistence
primitive — a full point-in-time snapshot — triggered by `SAVE`, a
periodic autosave timer, and a save-on-clean-shutdown hook for
`SIGINT`/`SIGTERM`. The format is a small custom length-prefixed binary
layout, unrelated to Redis's RDB format:

```mermaid
flowchart TB
    H["Header\n'DINO' magic + version byte"]
    H --> E1["Entry: type byte, key, expiry flag [+ millis], payload"]
    E1 --> E2["Entry: type byte, key, expiry flag [+ millis], payload"]
    E2 --> Edots["... one entry per live key, across all 32 shards ..."]
    Edots --> EOF["EOF = end of entries"]
```

Each entry's payload shape depends on its type byte — a length-prefixed
string for `String`, a count followed by key/value pairs for `Hash`,
and so on for `List`/`Set`/`ZSet`. Writing is a single pass over
`Store::for_each`, which holds one shard's shared lock at a time while
that shard's live (non-expired) entries are serialized. Reads proceed
during a `SAVE`, and a write only waits if it targets the shard currently
being written out. A mutex in `Server` also keeps a client `SAVE` and
the autosave timer from writing the snapshot at the same time.

Durability is via the standard write-temp-file, `fsync`, `rename`
pattern: `SAVE` always writes to `dinodb.rdb.tmp` first and atomically
renames it over `dinodb.rdb` only after every byte is flushed — a
crash mid-write leaves the previous snapshot untouched rather than a
half-written file.

Loading is all-or-nothing. The whole file is parsed into a staging
buffer first, and the store is only populated if every record checks
out (with length fields sanity-capped so a corrupt one can't trigger a
multi-gigabyte allocation). If the file is damaged, the server refuses
to start, as Redis does. The alternative, starting empty, would let the
next autosave quietly overwrite whatever was still recoverable.

## Concurrency safety invariants

The whole locking story rests on a short list of invariants, each one
enforced structurally rather than by convention:

- **A key belongs to exactly one shard, always**, because
  `shard_for(key)` is a pure function of the key's hash — there is no
  code path where a key's shard changes over its lifetime.
- **No pointer into a shard outlives its lock.** Handlers only see
  entries inside `Store::read`/`Store::write` callbacks, which run with
  the shard lock held; command handlers never reach into a `Shard`
  directly. The test suite runs under ThreadSanitizer in CI to keep it
  that way.
- **A connection's buffers are touched by exactly one thread.** Once
  the acceptor enqueues an fd to worker *i*, no other worker ever sees
  that fd again, so `resp::Parser` and the per-connection output buffer
  need no synchronization of their own.
- **`for_each` (used by `KEYS`, `SAVE`, `DBSIZE`) takes a shared lock
  per shard, one shard at a time** — never all 32 at once — so a full
  scan can't deadlock against, or starve, a concurrent single-key write
  on some other shard.

## A concrete trace

Two clients, one `SET`, one `GET`, same key, arriving on different
worker threads at roughly the same time:

```mermaid
sequenceDiagram
    participant CA as Client A (worker 0): SET user:1 alice
    participant CB as Client B (worker 3): GET user:1
    participant Sh as Shard(hash("user:1") % 32)

    CA->>Sh: unique_lock (write)
    CB->>Sh: shared_lock (write) -- blocks, waits for CA
    Sh-->>CA: entry created/updated, str="alice"
    CA->>Sh: unlock
    Sh-->>CB: lock acquired
    Sh-->>CB: read str="alice"
    CB->>Sh: unlock
```

Nothing here is DinoDB-specific — it's exactly what a `std::shared_mutex`
guarantees. The interesting part is what *doesn't* happen: a third
client doing `SET other:key 1` on a different shard runs concurrently
with both of these, untouched, on a third worker thread, the whole
time.

## Does the trade actually pay off?

The point of all that locking is multi-core throughput, so I measured it
(`scripts/bench.py`, Apple M4, against Redis 8.10; full tables in the
README). Two things stood out.

First, an honest benchmark on one laptop is harder than it looks. For
small `GET`/`SET` requests, every server, Redis and DinoDB alike, landed
at about 145k req/s without pipelining and about 2.2–2.4M with it. That is
the ceiling of `redis-benchmark` and loopback TCP, not of either server.
`redis-benchmark`'s own throughput numbers were also quantized for short
runs (results snapped to exactly 1,000,000 or 2,000,000), so the script
computes throughput from wall-clock time and records server CPU time per
request, which doesn't depend on who the bottleneck is.

Second, once the server *is* the bottleneck, the design delivers. With
`LRANGE` returning 100–300 elements from one shared list, every request
takes a `shared_lock` on the same shard and they all proceed in parallel:

| | Redis | Redis, 4 I/O threads | DinoDB, 1 worker | DinoDB, 4 workers |
|---|---:|---:|---:|---:|
| `LRANGE 0 99` | 296k req/s | 333k | 250k | **399k** |
| `LRANGE 0 299` | 121k req/s | 140k | 105k | **174k** |

That's about 1.4x Redis, with lower p99 latency. Redis's I/O threads
parallelize socket reads and writes, but command execution, including
encoding those 300-element replies, still happens on one thread.

The cost is just as clear. Per core, DinoDB is less efficient: a single
worker needs 25–30% more CPU per request than Redis and trails it on
every workload. Under light load, more workers means more CPU burned per
request, because each wake-up handles fewer requests. Sharded locks buy
parallelism; they don't buy efficiency.

## When this design is (and isn't) the right call

DinoDB's sharded-mutex model is a genuine trade, not a strictly better
version of anything:

| Use it when | Reach for something else when |
|---|---|
| Workload is CPU-bound across many independent keys | Workload is dominated by a handful of hot keys — sharding doesn't help those |
| You want to reason about correctness with plain `std::shared_mutex` semantics | You need to squeeze out the last 20% of throughput and can justify a lock-free design's audit cost |
| Memory headroom for a fixed-layout `Value` per key is fine | Key count is huge and per-key overhead has to be minimized |
| A snapshot-based durability model is acceptable | You need Redis's AOF-level durability guarantees |

## What's not here yet

Pub/Sub, `MULTI`/`EXEC` transactions, Lua scripting, `SCAN` cursors,
streams, bitmaps, HyperLogLog, geospatial commands, and any form of
replication or clustering are all absent. Each is a reasonably
self-contained addition given the existing layering — `SCAN`, for
instance, is mostly a cursor wrapped around the same `for_each` that
already powers `KEYS`.

Source: [github.com/udaysehgal/dinodb](https://github.com/udaysehgal/dinodb)
