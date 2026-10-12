#include "dinodb/persistence.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <utility>
#include <vector>

namespace dinodb::persistence {

namespace {

constexpr char kMagic[4] = {'D', 'I', 'N', 'O'};
constexpr uint8_t kVersion = 1;

void write_u32(std::ostream& os, uint32_t v) { os.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void write_u8(std::ostream& os, uint8_t v) { os.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void write_i64(std::ostream& os, int64_t v) { os.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void write_double(std::ostream& os, double v) { os.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void write_str(std::ostream& os, const std::string& s) {
    write_u32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool read_u32(std::istream& is, uint32_t& v) { return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(v))); }
bool read_u8(std::istream& is, uint8_t& v) { return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(v))); }
bool read_i64(std::istream& is, int64_t& v) { return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(v))); }
bool read_double(std::istream& is, double& v) { return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(v))); }
// Guards against a corrupt length field turning into a multi-GB allocation.
constexpr uint32_t kMaxStrLen = 512u * 1024 * 1024;

bool read_str(std::istream& is, std::string& s) {
    uint32_t len = 0;
    if (!read_u32(is, len) || len > kMaxStrLen) return false;
    s.resize(len);
    if (len == 0) return true;
    return static_cast<bool>(is.read(s.data(), len));
}

// ofstream can't fsync, so reopen the finished file just to flush it to
// disk before it replaces the previous snapshot.
bool fsync_path(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

}  // namespace

bool save(Store& store, const std::string& path) {
    std::string tmp_path = path + ".tmp";
    std::ofstream os(tmp_path, std::ios::binary | std::ios::trunc);
    if (!os) return false;

    os.write(kMagic, sizeof(kMagic));
    write_u8(os, kVersion);

    store.for_each([&](const std::string& key, const Entry& e) {
        write_u8(os, static_cast<uint8_t>(e.value.type));
        write_str(os, key);
        if (e.expires_at.has_value()) {
            write_u8(os, 1);
            int64_t millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  *e.expires_at - std::chrono::system_clock::time_point{})
                                  .count();
            write_i64(os, millis);
        } else {
            write_u8(os, 0);
        }

        switch (e.value.type) {
            case ValueType::String:
                write_str(os, e.value.str);
                break;
            case ValueType::Hash:
                write_u32(os, static_cast<uint32_t>(e.value.hash.size()));
                for (auto& [k, v] : e.value.hash) {
                    write_str(os, k);
                    write_str(os, v);
                }
                break;
            case ValueType::List:
                write_u32(os, static_cast<uint32_t>(e.value.list.size()));
                for (auto& v : e.value.list) write_str(os, v);
                break;
            case ValueType::Set:
                write_u32(os, static_cast<uint32_t>(e.value.set.size()));
                for (auto& v : e.value.set) write_str(os, v);
                break;
            case ValueType::ZSet:
                write_u32(os, static_cast<uint32_t>(e.value.zscores.size()));
                for (auto& [member, score] : e.value.zscores) {
                    write_str(os, member);
                    write_double(os, score);
                }
                break;
        }
    });

    os.flush();
    bool ok = os.good();
    os.close();
    if (!ok || os.fail() || !fsync_path(tmp_path)) {
        std::remove(tmp_path.c_str());
        return false;
    }

    // rename() is atomic: readers see either the old snapshot or the new
    // one, never a partial file.
    return std::rename(tmp_path.c_str(), path.c_str()) == 0;
}

namespace {

bool load_entries(std::istream& is, std::vector<std::pair<std::string, Entry>>& out) {
    char magic[4];
    if (!is.read(magic, sizeof(magic))) return false;
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;
    uint8_t version = 0;
    if (!read_u8(is, version) || version != kVersion) return false;

    while (true) {
        uint8_t type_byte;
        if (!read_u8(is, type_byte)) break;  // clean EOF
        if (type_byte > static_cast<uint8_t>(ValueType::ZSet)) return false;

        std::string key;
        if (!read_str(is, key)) return false;

        uint8_t has_expiry = 0;
        if (!read_u8(is, has_expiry)) return false;
        std::optional<TimePoint> expires_at;
        if (has_expiry) {
            int64_t millis = 0;
            if (!read_i64(is, millis)) return false;
            expires_at = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(millis);
        }

        Entry entry;
        entry.value.type = static_cast<ValueType>(type_byte);
        entry.expires_at = expires_at;

        switch (entry.value.type) {
            case ValueType::String: {
                if (!read_str(is, entry.value.str)) return false;
                break;
            }
            case ValueType::Hash: {
                uint32_t n = 0;
                if (!read_u32(is, n)) return false;
                for (uint32_t i = 0; i < n; ++i) {
                    std::string k, v;
                    if (!read_str(is, k) || !read_str(is, v)) return false;
                    entry.value.hash.emplace(std::move(k), std::move(v));
                }
                break;
            }
            case ValueType::List: {
                uint32_t n = 0;
                if (!read_u32(is, n)) return false;
                for (uint32_t i = 0; i < n; ++i) {
                    std::string v;
                    if (!read_str(is, v)) return false;
                    entry.value.list.push_back(std::move(v));
                }
                break;
            }
            case ValueType::Set: {
                uint32_t n = 0;
                if (!read_u32(is, n)) return false;
                for (uint32_t i = 0; i < n; ++i) {
                    std::string v;
                    if (!read_str(is, v)) return false;
                    entry.value.set.insert(std::move(v));
                }
                break;
            }
            case ValueType::ZSet: {
                uint32_t n = 0;
                if (!read_u32(is, n)) return false;
                for (uint32_t i = 0; i < n; ++i) {
                    std::string member;
                    double score = 0;
                    if (!read_str(is, member) || !read_double(is, score)) return false;
                    entry.value.zset_put(member, score);
                }
                break;
            }
        }

        out.emplace_back(std::move(key), std::move(entry));
    }

    return is.eof();
}

}  // namespace

LoadResult load(Store& store, const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return LoadResult::Missing;

    std::vector<std::pair<std::string, Entry>> entries;
    if (!load_entries(is, entries)) return LoadResult::Corrupt;

    auto t = now();
    for (auto& [key, entry] : entries) {
        if (entry.expired(t) || entry.value.empty_container()) continue;
        store.restore(key, std::move(entry));
    }
    return LoadResult::Ok;
}

}  // namespace dinodb::persistence
