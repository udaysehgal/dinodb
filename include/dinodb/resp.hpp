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
    // Pops and returns the next complete request, if any. Returns nullopt
    // once the parser has failed.
    std::optional<Request> next();

    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }

private:
    std::string buf_;
    size_t pos_ = 0;
    std::string error_;

    std::optional<Request> fail(const std::string& msg);

    std::optional<Request> try_parse_multibulk();
    std::optional<Request> try_parse_inline();
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
std::string empty_array();

}  // namespace dinodb::resp
