#include "dinodb/resp.hpp"

#include <cstdlib>

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

std::optional<Request> Parser::try_parse_multibulk() {
    size_t p = pos_;
    if (p >= buf_.size() || buf_[p] != '*') return std::nullopt;

    size_t line_end = find_crlf(buf_, p);
    if (line_end == std::string::npos) return std::nullopt;
    long count = std::strtol(buf_.c_str() + p + 1, nullptr, 10);
    size_t cursor = line_end + 2;

    if (count < 0) {
        pos_ = cursor;
        return Request{};
    }

    Request req;
    req.reserve(static_cast<size_t>(count));

    for (long i = 0; i < count; ++i) {
        if (cursor >= buf_.size() || buf_[cursor] != '$') return std::nullopt;
        size_t len_line_end = find_crlf(buf_, cursor);
        if (len_line_end == std::string::npos) return std::nullopt;
        long blen = std::strtol(buf_.c_str() + cursor + 1, nullptr, 10);
        cursor = len_line_end + 2;
        if (blen < 0) {
            req.emplace_back();
            continue;
        }
        if (cursor + static_cast<size_t>(blen) + 2 > buf_.size()) return std::nullopt;
        req.emplace_back(buf_, cursor, static_cast<size_t>(blen));
        cursor += static_cast<size_t>(blen) + 2;
    }

    pos_ = cursor;
    return req;
}

std::optional<Request> Parser::try_parse_inline() {
    size_t p = pos_;
    size_t nl = buf_.find('\n', p);
    if (nl == std::string::npos) return std::nullopt;

    size_t end = nl;
    if (end > p && buf_[end - 1] == '\r') --end;

    Request req;
    size_t i = p;
    while (i < end) {
        while (i < end && buf_[i] == ' ') ++i;
        if (i >= end) break;
        size_t start = i;
        while (i < end && buf_[i] != ' ') ++i;
        req.emplace_back(buf_, start, i - start);
    }

    pos_ = nl + 1;
    return req;
}

std::optional<Request> Parser::next() {
    if (pos_ >= buf_.size()) {
        buf_.clear();
        pos_ = 0;
        return std::nullopt;
    }

    std::optional<Request> result;
    if (buf_[pos_] == '*') {
        result = try_parse_multibulk();
    } else {
        result = try_parse_inline();
    }

    if (result.has_value()) {
        compact();
    }
    return result;
}

std::string simple_string(const std::string& s) { return "+" + s + "\r\n"; }
std::string error(const std::string& msg) { return "-" + msg + "\r\n"; }
std::string integer(int64_t v) { return ":" + std::to_string(v) + "\r\n"; }

std::string bulk_string(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
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
