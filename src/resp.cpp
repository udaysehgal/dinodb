#include "dinodb/resp.hpp"

#include <algorithm>
#include <charconv>

namespace dinodb::resp {

void Parser::feed(const char* data, size_t len) {
    buf_.append(data, len);
}

void Parser::compact() {
    if (pos_ > 0 && pos_ >= buf_.size() / 2 && pos_ > 4096) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
}

// Finds "\r\n" starting at `from`; returns npos if not found.
static size_t find_crlf(const std::string& s, size_t from) {
    size_t r = s.find('\r', from);
    if (r == std::string::npos || r + 1 >= s.size()) return std::string::npos;
    if (s[r + 1] != '\n') return std::string::npos;
    return r;
}

// Parses the decimal number in buf_[from, to); false if it isn't one.
static bool parse_len(const std::string& s, size_t from, size_t to, long& out) {
    auto [end, ec] = std::from_chars(s.data() + from, s.data() + to, out);
    return ec == std::errc{} && end == s.data() + to;
}

bool Parser::fail(const std::string& msg) {
    error_ = "ERR Protocol error: " + msg;
    return false;
}

// Fills `out` in place, reusing its strings' storage across requests: the
// per-request allocation of a fresh vector showed up in profiles.
bool Parser::try_parse_multibulk(Request& out) {
    size_t p = pos_;
    if (p >= buf_.size() || buf_[p] != '*') return false;

    size_t line_end = find_crlf(buf_, p);
    if (line_end == std::string::npos) {
        if (buf_.size() - p > 32) return fail("invalid multibulk length");
        return false;
    }
    long count = 0;
    if (!parse_len(buf_, p + 1, line_end, count) || count > kMaxArgs) {
        return fail("invalid multibulk length");
    }
    size_t cursor = line_end + 2;

    if (count < 0) {
        pos_ = cursor;
        out.clear();
        return true;
    }

    // Never size an allocation from the client's count; out grows as
    // arguments actually arrive.
    size_t n = 0;
    for (long i = 0; i < count; ++i) {
        if (cursor >= buf_.size()) return false;
        if (buf_[cursor] != '$') return fail("expected '$', got '" + std::string(1, buf_[cursor]) + "'");
        size_t len_line_end = find_crlf(buf_, cursor);
        if (len_line_end == std::string::npos) {
            if (buf_.size() - cursor > 32) return fail("invalid bulk length");
            return false;
        }
        long blen = 0;
        if (!parse_len(buf_, cursor + 1, len_line_end, blen) || blen < 0 || blen > kMaxBulkLen) {
            return fail("invalid bulk length");
        }
        cursor = len_line_end + 2;
        if (cursor + static_cast<size_t>(blen) + 2 > buf_.size()) return false;
        if (buf_[cursor + blen] != '\r' || buf_[cursor + blen + 1] != '\n') {
            return fail("bulk string not terminated by CRLF");
        }
        if (n < out.size()) out[n].assign(buf_, cursor, static_cast<size_t>(blen));
        else out.emplace_back(buf_, cursor, static_cast<size_t>(blen));
        ++n;
        cursor += static_cast<size_t>(blen) + 2;
    }

    out.resize(n);
    pos_ = cursor;
    return true;
}

bool Parser::try_parse_inline(Request& out) {
    size_t p = pos_;
    size_t nl = buf_.find('\n', p);
    if (nl == std::string::npos) {
        if (buf_.size() - p > kMaxInlineLen) return fail("too big inline request");
        return false;
    }

    size_t end = nl;
    if (end > p && buf_[end - 1] == '\r') --end;

    Request& req = out;
    req.clear();
    size_t i = p;
    while (i < end) {
        while (i < end && buf_[i] == ' ') ++i;
        if (i >= end) break;
        size_t start = i;
        while (i < end && buf_[i] != ' ') ++i;
        req.emplace_back(buf_, start, i - start);
    }

    pos_ = nl + 1;
    return true;
}

bool Parser::next(Request& out) {
    if (failed()) return false;
    if (pos_ >= buf_.size()) {
        buf_.clear();
        pos_ = 0;
        return false;
    }

    bool ok = buf_[pos_] == '*' ? try_parse_multibulk(out) : try_parse_inline(out);
    if (ok) compact();
    return ok;
}

std::optional<Request> Parser::next() {
    Request req;
    if (!next(req)) return std::nullopt;
    return req;
}

// Reply builders append into one pre-sized string rather than chaining
// operator+, which built a temporary per piece.
std::string simple_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 3);
    out += '+';
    out += s;
    out += "\r\n";
    return out;
}
std::string error(const std::string& msg) { return "-" + msg + "\r\n"; }
std::string integer(int64_t v) { return ":" + std::to_string(v) + "\r\n"; }

void append_bulk(std::string& out, const std::string& s) {
    char len[24];
    auto [end, ec] = std::to_chars(len, len + sizeof(len), s.size());
    out += '$';
    out.append(len, end);
    out += "\r\n";
    out += s;
    out += "\r\n";
}

std::string bulk_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    append_bulk(out, s);
    return out;
}

std::string ArrayWriter::finish() const {
    std::string out;
    out.reserve(body_.size() + 16);
    out += '*';
    out += std::to_string(count_);
    out += "\r\n";
    out += body_;
    return out;
}

std::string nil_bulk() { return "$-1\r\n"; }
std::string nil_array() { return "*-1\r\n"; }
std::string empty_array() { return "*0\r\n"; }

std::string array(const std::vector<std::string>& already_encoded_items) {
    std::string out = "*" + std::to_string(already_encoded_items.size()) + "\r\n";
    for (auto& item : already_encoded_items) out += item;
    return out;
}

}  // namespace dinodb::resp
