#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinodb::resp {

// A parsed client request: an array of bulk strings, e.g. ["SET", "k", "v"].
using Request = std::vector<std::string>;

// Streaming RESP parser. Feed it raw bytes as they arrive off the socket;
// it extracts complete requests and leaves partial data buffered for the
// next call. Supports the RESP multibulk array format used by real Redis
// clients, plus plain inline commands (space-separated, newline terminated)
// for quick manual testing with `nc` or `telnet`.
//
// Malformed or oversized input puts the parser into a sticky error state
// (see error()); the server replies with the message and drops the client.
class Parser {
public:
    // Same limits Redis applies by default.
    static constexpr long kMaxArgs = 1024 * 1024;
    static constexpr long kMaxBulkLen = 512L * 1024 * 1024;
    static constexpr size_t kMaxInlineLen = 64 * 1024;

    void feed(const char* data, size_t len);
    // Pops the next complete request into `out` (reusing its storage) and
    // returns true, or returns false if none is complete yet or the parser
    // has failed.
    bool next(Request& out);
    // Convenience form that allocates a fresh Request.
    std::optional<Request> next();

    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }

private:
    std::string buf_;
    size_t pos_ = 0;
    std::string error_;

    bool fail(const std::string& msg);

    bool try_parse_multibulk(Request& out);
    bool try_parse_inline(Request& out);
    void compact();
};

// Reply builders.
std::string simple_string(const std::string& s);
std::string error(const std::string& msg);
std::string integer(int64_t v);
std::string bulk_string(const std::string& s);
std::string nil_bulk();
std::string array(const std::vector<std::string>& already_encoded_items);
std::string nil_array();

// Appends one bulk string to `out` in place.
void append_bulk(std::string& out, const std::string& s);

// Builds an array reply directly into one buffer instead of encoding every
// element as its own string and concatenating them afterwards.
class ArrayWriter {
public:
    void bulk(const std::string& s) {
        append_bulk(body_, s);
        ++count_;
    }
    std::string finish() const;

private:
    std::string body_;
    size_t count_ = 0;
};
std::string empty_array();

}  // namespace dinodb::resp
