#pragma once

// Thin readiness-notification wrapper: epoll on Linux, kqueue on macOS/BSD.
//
// Unlike poll(), interest is registered once per fd instead of being passed
// (and re-scanned by the kernel) on every call, and wait() only returns the
// fds that are actually ready. Both backends are level-triggered, so the
// worker loop keeps poll()'s simple semantics: anything left unread or
// unsent is reported again on the next wait().

#include <unistd.h>

#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace dinodb {

class Poller {
public:
    struct Event {
        int fd;
        bool readable;  // includes EOF/hangup: the next recv() reports it
        bool writable;
        bool error;
    };

    Poller() {
#if defined(__linux__)
        fd_ = epoll_create1(EPOLL_CLOEXEC);
#else
        fd_ = kqueue();
#endif
    }
    ~Poller() {
        if (fd_ >= 0) ::close(fd_);
    }
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    bool ok() const { return fd_ >= 0; }

    // Registers read interest in fd.
    bool add(int fd) {
#if defined(__linux__)
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = fd;
        return epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
#else
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        return kevent(fd_, &ev, 1, nullptr, 0, nullptr) == 0;
#endif
    }

    // Turns write interest on or off (on only while output is pending).
    void set_write(int fd, bool on) {
#if defined(__linux__)
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP | (on ? EPOLLOUT : 0u);
        ev.data.fd = fd;
        epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev);
#else
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_WRITE, on ? EV_ADD : EV_DELETE, 0, 0, nullptr);
        kevent(fd_, &ev, 1, nullptr, 0, nullptr);
#endif
    }

    // Closing an fd removes it from both epoll and kqueue automatically, so
    // there is no remove(); callers just close().

    // Waits up to timeout_ms and fills `out` with ready fds. An fd can
    // appear twice on kqueue (separate read and write events).
    int wait(std::vector<Event>& out, int timeout_ms) {
        out.clear();
#if defined(__linux__)
        epoll_event evs[kMaxEvents];
        int n = epoll_wait(fd_, evs, kMaxEvents, timeout_ms);
        for (int i = 0; i < n; ++i) {
            uint32_t e = evs[i].events;
            out.push_back({evs[i].data.fd, (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0,
                           (e & EPOLLOUT) != 0, (e & EPOLLERR) != 0});
        }
#else
        struct kevent evs[kMaxEvents];
        timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        int n = kevent(fd_, nullptr, 0, evs, kMaxEvents, &ts);
        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(evs[i].ident);
            bool error = (evs[i].flags & EV_ERROR) != 0;
            out.push_back({fd, evs[i].filter == EVFILT_READ, evs[i].filter == EVFILT_WRITE, error});
        }
#endif
        return n;
    }

private:
    static constexpr int kMaxEvents = 256;
    int fd_ = -1;
};

}  // namespace dinodb
