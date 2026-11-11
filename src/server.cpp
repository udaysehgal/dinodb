#include "dinodb/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dinodb/commands.hpp"
#include "dinodb/persistence.hpp"
#include "poller.hpp"

namespace dinodb {

namespace {

// Per-connection buffering state, owned by exactly one worker thread.
struct ConnState {
    resp::Parser parser;
    resp::Request req;  // reused across requests to avoid reallocating
    std::string outbuf;
    size_t outbuf_sent = 0;
    bool close_after_flush = false;  // QUIT or protocol error
    bool want_write = false;         // write interest registered with the poller
    bool closing = false;            // queued for close this iteration
};

// Work handed from the acceptor to a worker: a raw fd waiting to be
// registered with that worker's poll() set. The acceptor writes a byte to
// wake_w after queueing so the worker notices immediately instead of on its
// next poll() timeout.
struct WorkerState {
    std::mutex mtx;
    std::deque<int> pending;
    int wake_r = -1;
    int wake_w = -1;
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_conn(int fd, std::unordered_map<int, ConnState>& conns, Server& server) {
    conns.erase(fd);
    ::close(fd);
    server.connected_clients--;
}

bool is_quit(const resp::Request& req) {
    return req[0].size() == 4 && strncasecmp(req[0].c_str(), "quit", 4) == 0;
}

// Reads whatever is available and runs every complete request. Returns
// false if the connection should be closed right away.
bool handle_readable(int fd, ConnState& cs, Server& server, char* buf, size_t buf_size) {
    while (true) {
        ssize_t r = ::recv(fd, buf, buf_size, 0);
        if (r > 0) {
            cs.parser.feed(buf, static_cast<size_t>(r));
            if (static_cast<size_t>(r) < buf_size) break;
        } else if (r == 0) {
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
    }

    while (!cs.close_after_flush && cs.parser.next(cs.req)) {
        if (cs.req.empty()) continue;
        cs.outbuf += dispatch(server.store(), server, cs.req);
        server.commands_processed++;
        if (is_quit(cs.req)) cs.close_after_flush = true;
    }
    if (cs.parser.failed() && !cs.close_after_flush) {
        cs.outbuf += resp::error(cs.parser.error());
        cs.close_after_flush = true;
    }
    return true;
}

// Writes as much of outbuf as the socket accepts. Returns false on error.
bool flush_output(int fd, ConnState& cs) {
    while (cs.outbuf_sent < cs.outbuf.size()) {
        ssize_t w = ::send(fd, cs.outbuf.data() + cs.outbuf_sent, cs.outbuf.size() - cs.outbuf_sent, 0);
        if (w > 0) {
            cs.outbuf_sent += static_cast<size_t>(w);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
    }
    if (cs.outbuf_sent == cs.outbuf.size()) {
        cs.outbuf.clear();
        cs.outbuf_sent = 0;
    }
    return true;
}

void worker_loop(WorkerState& ws, std::atomic<bool>& running, Server& server) {
    std::unordered_map<int, ConnState> conns;
    std::vector<Poller::Event> events;
    std::vector<int> to_close;
    std::vector<int> to_flush;
    char buf[65536];

    Poller poller;
    if (!poller.ok() || !poller.add(ws.wake_r)) {
        std::cerr << "dinodb: failed to create poller: " << strerror(errno) << "\n";
        std::abort();
    }

    while (running.load()) {
        // The timeout only bounds how long shutdown takes to notice running_.
        if (poller.wait(events, 200) <= 0) continue;

        to_close.clear();
        to_flush.clear();
        for (const Poller::Event& ev : events) {
            if (ev.fd == ws.wake_r) {
                char drain[64];
                while (::read(ws.wake_r, drain, sizeof(drain)) > 0) {
                }
                std::lock_guard<std::mutex> lock(ws.mtx);
                for (int fd : ws.pending) {
                    if (poller.add(fd)) {
                        conns.emplace(fd, ConnState{});
                    } else {
                        ::close(fd);
                        server.connected_clients--;
                    }
                }
                ws.pending.clear();
                continue;
            }

            auto it = conns.find(ev.fd);
            if (it == conns.end() || it->second.closing) continue;
            ConnState& cs = it->second;

            // Read before honoring a hangup so a client that sends a command
            // and immediately half-closes still gets it executed; the EOF
            // itself shows up as recv() returning 0.
            bool ok = !ev.error;
            if (ok && ev.readable) ok = handle_readable(ev.fd, cs, server, buf, sizeof(buf));
            if (!ok) {
                cs.closing = true;
                to_close.push_back(ev.fd);
                continue;
            }
            if (!cs.outbuf.empty() || cs.close_after_flush) to_flush.push_back(ev.fd);
        }
        for (int fd : to_flush) {
            auto it = conns.find(fd);
            if (it == conns.end() || it->second.closing) continue;
            ConnState& cs = it->second;
            bool ok = cs.outbuf.empty() || flush_output(fd, cs);
            if (cs.close_after_flush && cs.outbuf.empty()) ok = false;
            if (!ok) {
                cs.closing = true;
                to_close.push_back(fd);
                continue;
            }
            bool want_write = !cs.outbuf.empty();
            if (want_write != cs.want_write) {
                poller.set_write(fd, want_write);
                cs.want_write = want_write;
            }
        }
        for (int fd : to_close) close_conn(fd, conns, server);
    }

    for (auto& [fd, cs] : conns) ::close(fd);
}

}  // namespace

Server::Server(ServerConfig cfg) : cfg_(std::move(cfg)), store_(32) {
    if (cfg_.worker_threads <= 0) {
        unsigned hc = std::thread::hardware_concurrency();
        cfg_.worker_threads = hc > 0 ? static_cast<int>(hc) : 4;
    }
    std::signal(SIGPIPE, SIG_IGN);
}

Server::~Server() { stop(); }

bool Server::save_snapshot() {
    std::lock_guard<std::mutex> lock(save_mtx_);
    return persistence::save(store_, cfg_.rdb_path);
}

int64_t Server::uptime_seconds() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_).count();
}

void Server::cron_loop() {
    using namespace std::chrono;
    constexpr auto kTick = milliseconds(100);
    auto next_save = steady_clock::now() + seconds(cfg_.autosave_interval_sec);
    while (running_.load()) {
        std::this_thread::sleep_for(kTick);
        // One shard per tick: a full pass over 32 shards takes ~3s and never
        // holds more than one shard's lock at a time.
        expired_keys += store_.sweep_expired_step();
        if (cfg_.autosave_interval_sec > 0 && steady_clock::now() >= next_save) {
            if (!save_snapshot()) std::cerr << "dinodb: autosave to " << cfg_.rdb_path << " failed\n";
            next_save = steady_clock::now() + seconds(cfg_.autosave_interval_sec);
        }
    }
}

int Server::run() {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        std::cerr << "dinodb: socket() failed: " << strerror(errno) << "\n";
        return 1;
    }

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "dinodb: invalid bind address '" << cfg_.bind_addr << "'\n";
        ::close(lfd);
        return 1;
    }

    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "dinodb: bind() failed on port " << cfg_.port << ": " << strerror(errno) << "\n";
        ::close(lfd);
        return 1;
    }

    if (listen(lfd, 512) < 0) {
        std::cerr << "dinodb: listen() failed: " << strerror(errno) << "\n";
        ::close(lfd);
        return 1;
    }

    set_nonblocking(lfd);
    running_ = true;

    std::vector<std::unique_ptr<WorkerState>> worker_states;
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < cfg_.worker_threads; ++i) {
        auto ws = std::make_unique<WorkerState>();
        int fds[2];
        if (pipe(fds) != 0) {
            std::cerr << "dinodb: pipe() failed: " << strerror(errno) << "\n";
            return 1;
        }
        set_nonblocking(fds[0]);
        set_nonblocking(fds[1]);
        ws->wake_r = fds[0];
        ws->wake_w = fds[1];
        worker_states.push_back(std::move(ws));
    }
    for (int i = 0; i < cfg_.worker_threads; ++i) {
        worker_threads.emplace_back(worker_loop, std::ref(*worker_states[i]), std::ref(running_), std::ref(*this));
    }

    cron_thread_ = std::thread(&Server::cron_loop, this);

    std::cout << "dinodb listening on " << cfg_.bind_addr << ":" << cfg_.port
              << " with " << cfg_.worker_threads << " worker threads" << std::endl;

    size_t next_worker = 0;
    while (running_.load()) {
        // poll() with a timeout rather than a blocking accept(): shutdown()
        // doesn't reliably wake accept() on every platform (e.g. macOS).
        pollfd lp{lfd, POLLIN, 0};
        if (poll(&lp, 1, 200) <= 0) continue;
        int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) continue;
            if (!running_.load()) break;
            std::cerr << "dinodb: accept() failed: " << strerror(errno) << "\n";
            continue;  // e.g. EMFILE: keep serving existing clients
        }

        set_nonblocking(fd);
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        WorkerState& ws = *worker_states[next_worker];
        next_worker = (next_worker + 1) % worker_states.size();
        connected_clients++;
        {
            std::lock_guard<std::mutex> lock(ws.mtx);
            ws.pending.push_back(fd);
        }
        char wake = 1;
        if (::write(ws.wake_w, &wake, 1) < 0) {
            // EAGAIN: the pipe is full, so a wake-up is already pending.
        }
    }

    running_ = false;
    for (auto& t : worker_threads) t.join();
    if (cron_thread_.joinable()) cron_thread_.join();
    for (auto& ws : worker_states) {
        ::close(ws->wake_r);
        ::close(ws->wake_w);
        for (int fd : ws->pending) ::close(fd);
    }
    ::close(lfd);

    return 0;
}

// Async-signal-safe (only flips an atomic), so it can be called straight
// from a SIGINT/SIGTERM handler. run() notices within one poll timeout.
void Server::stop() { running_ = false; }

}  // namespace dinodb
