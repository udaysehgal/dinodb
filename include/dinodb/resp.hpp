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
class Parser {
public:
    void feed(const char* data, size_t len);
    // Pops and returns the next complete request, if any.
    std::optional<Request> next();

private:
    std::string buf_;
    size_t pos_ = 0;

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
