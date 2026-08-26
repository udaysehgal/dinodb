#include "dinodb/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dinodb/commands.hpp"
#include "dinodb/persistence.hpp"

namespace dinodb {

namespace {

// Per-connection buffering state, owned by exactly one worker thread.
struct ConnState {
    resp::Parser parser;
    std::string outbuf;
    size_t outbuf_sent = 0;
};

// Work handed from the acceptor to a worker: a raw fd waiting to be
// registered with that worker's poll() set.
struct WorkerState {
    std::mutex mtx;
    std::deque<int> pending;
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_conn(int fd, std::unordered_map<int, ConnState>& conns, Server& server) {
    conns.erase(fd);
    ::close(fd);
    if (server.connected_clients.load() > 0) server.connected_clients--;
}

void worker_loop(WorkerState& ws, std::atomic<bool>& running, Server& server) {
    std::unordered_map<int, ConnState> conns;
    std::vector<pollfd> pfds;
    char buf[65536];

    while (running.load()) {
        {
            std::lock_guard<std::mutex> lock(ws.mtx);
            while (!ws.pending.empty()) {
                int fd = ws.pending.front();
                ws.pending.pop_front();
                conns.emplace(fd, ConnState{});
            }
        }

        pfds.clear();
        pfds.reserve(conns.size());
        for (auto& [fd, cs] : conns) {
            short events = POLLIN;
            if (!cs.outbuf.empty()) events |= POLLOUT;
            pfds.push_back(pollfd{fd, events, 0});
        }

        int n = poll(pfds.empty() ? nullptr : pfds.data(), pfds.size(), 200);
        if (n <= 0) continue;

        for (auto& pfd : pfds) {
            if (pfd.revents == 0) continue;
            auto it = conns.find(pfd.fd);
            if (it == conns.end()) continue;
            ConnState& cs = it->second;
            bool should_close = false;

            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                should_close = true;
            }

            if (!should_close && (pfd.revents & POLLIN)) {
                while (true) {
                    ssize_t r = ::recv(pfd.fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        cs.parser.feed(buf, static_cast<size_t>(r));
                        if (static_cast<size_t>(r) < sizeof(buf)) break;
                    } else if (r == 0) {
                        should_close = true;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        should_close = true;
                        break;
                    }
                }

                if (!should_close) {
                    while (auto req = cs.parser.next()) {
                        if (req->empty()) continue;
                        cs.outbuf += dispatch(server.store(), server, *req);
                        server.commands_processed++;
                    }
                }
            }

            if (!should_close && !cs.outbuf.empty()) {
                while (cs.outbuf_sent < cs.outbuf.size()) {
                    ssize_t w = ::send(pfd.fd, cs.outbuf.data() + cs.outbuf_sent,
                                        cs.outbuf.size() - cs.outbuf_sent, 0);
                    if (w > 0) {
                        cs.outbuf_sent += static_cast<size_t>(w);
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        should_close = true;
                        break;
                    }
                }
                if (cs.outbuf_sent == cs.outbuf.size()) {
                    cs.outbuf.clear();
                    cs.outbuf_sent = 0;
                }
            }

            if (should_close) close_conn(pfd.fd, conns, server);
        }
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

bool Server::save_snapshot() { return persistence::save(store_, cfg_.rdb_path); }
bool Server::load_snapshot() { return persistence::load(store_, cfg_.rdb_path); }

void Server::autosave_loop() {
    while (running_.load()) {
        for (int i = 0; i < cfg_.autosave_interval_sec && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (running_.load()) save_snapshot();
    }
}

int Server::run() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "dinodb: socket() failed: " << strerror(errno) << "\n";
        return 1;
    }

    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (cfg_.bind_addr == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, cfg_.bind_addr.c_str(), &addr.sin_addr);
    }

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "dinodb: bind() failed on port " << cfg_.port << ": " << strerror(errno) << "\n";
        ::close(listen_fd_);
        return 1;
    }

    if (listen(listen_fd_, 512) < 0) {
        std::cerr << "dinodb: listen() failed: " << strerror(errno) << "\n";
        ::close(listen_fd_);
        return 1;
    }

    running_ = true;

    std::vector<std::unique_ptr<WorkerState>> worker_states;
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < cfg_.worker_threads; ++i) {
        worker_states.push_back(std::make_unique<WorkerState>());
    }
    for (int i = 0; i < cfg_.worker_threads; ++i) {
        worker_threads.emplace_back(worker_loop, std::ref(*worker_states[i]), std::ref(running_), std::ref(*this));
    }

    if (cfg_.autosave_interval_sec > 0) {
        autosave_thread_ = std::thread(&Server::autosave_loop, this);
    }

    std::cout << "dinodb listening on " << cfg_.bind_addr << ":" << cfg_.port
              << " with " << cfg_.worker_threads << " worker threads\n";

    size_t next_worker = 0;
    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  // listen_fd_ was closed by stop(), or a fatal error occurred
        }

        set_nonblocking(fd);
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        WorkerState& ws = *worker_states[next_worker];
        next_worker = (next_worker + 1) % worker_states.size();
        {
            std::lock_guard<std::mutex> lock(ws.mtx);
            ws.pending.push_back(fd);
        }
        connected_clients++;
    }

    running_ = false;
    for (auto& t : worker_threads) t.join();
    if (autosave_thread_.joinable()) autosave_thread_.join();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    return 0;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

}  // namespace dinodb
