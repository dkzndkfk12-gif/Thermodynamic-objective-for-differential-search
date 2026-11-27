#pragma once
#include "vendor/json.hpp"
#include <gmpxx.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <ctime>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace thermo {
using J = nlohmann::ordered_json;
using Z = mpz_class;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t GiB = 1ULL << 30, MiB = 1ULL << 20;
inline std::atomic<bool> stopping{false};
inline double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
inline double thread_cpu() {
#ifdef __linux__
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        throw std::runtime_error("thread CPU clock failed");
    return t.tv_sec + t.tv_nsec * 1e-9;
#else
    return 0;
#endif
}
struct Cancelled : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Deadline {
    double end = std::numeric_limits<double>::infinity();
    void check() const {
        if (std::isfinite(end) && stopping.load())
            throw Cancelled("USER_STOP");
        if (now() >= end)
            throw Cancelled("TASK_TIME_LIMIT");
    }
    double remaining() const { return std::max(0.0, end - now()); }
};
inline void require(bool ok, const std::string &s) {
    if (!ok)
        throw std::runtime_error(s);
}
inline std::string read_text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    require(bool(f), "Cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
inline J read_json(const fs::path &p) { return J::parse(read_text(p)); }
inline void write_atomic(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        require(bool(f), "Cannot write " + tmp.string());
        f.write(s.data(), s.size());
        f.flush();
        require(bool(f), "Write failed " + tmp.string());
    }
#ifdef __linux__
    int fd = open(tmp.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("open for fsync failed");
    int rc = fsync(fd);
    close(fd);
    require(rc == 0, "fsync failed");
#endif
    fs::rename(tmp, p);
#ifdef __linux__
    int dir = open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
#endif
}
inline void save_json(const fs::path &p, const J &j) { write_atomic(p, nlohmann::json(j).dump(2) + '\n'); }
inline std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> v;
    std::stringstream f(s);
    std::string x;
    while (std::getline(f, x, c))
        v.push_back(x);
    if (!s.empty() && s.back() == c)
        v.push_back("");
    return v;
}
inline std::string upper(std::string s) {
    for (char &c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
inline std::string str(const J &j) {
    if (j.is_string())
        return j.get<std::string>();
    if (j.is_number_integer() || j.is_number_unsigned())
        return j.dump();
    throw std::runtime_error("Expected string/integer");
}
inline Z integer(const J &j) { return Z(str(j), 10); }
inline Z pow2(unsigned long n) {
    Z x = 1;
    mpz_mul_2exp(x.get_mpz_t(), x.get_mpz_t(), n);
    return x;
}
inline Z power(Z x, unsigned long n) {
    Z y;
    mpz_pow_ui(y.get_mpz_t(), x.get_mpz_t(), n);
    return y;
}
inline Z product(const std::vector<std::vector<int>> &ds) {
    Z n = 1;
    for (const auto &d : ds)
        n *= d.size();
    return n;
}
inline std::uint64_t checked_u64(const Z &z) {
    require(z >= 0 && mpz_sizeinbase(z.get_mpz_t(), 2) <= 64, "Integer does not fit uint64");
    return std::stoull(z.get_str());
}
inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
inline std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) {
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}
inline double log2z(const Z &z) {
    require(z > 0, "log2 of nonpositive integer");
    long e = 0;
    double m = mpz_get_d_2exp(&e, z.get_mpz_t());
    return std::log2(m) + e;
}
inline J dyadic(Z n, unsigned e) {
    require(n >= 0 && n <= pow2(e), "Exact probability outside [0,1]");
    if (n == 0)
        return J{{"numerator", "0"}, {"denominator_power", 0}, {"F", nullptr}};
    auto shift = std::min<unsigned long>(e, mpz_scan1(n.get_mpz_t(), 0));
    n >>= shift;
    e -= unsigned(shift);
    return J{{"numerator", n.get_str()}, {"denominator_power", e}, {"F", double(e) - log2z(n)}};
}
inline int compare_mass(const J &a, const J &b) {
    unsigned ea = a.at("denominator_power"), eb = b.at("denominator_power"), e = std::max(ea, eb);
    Z x = integer(a.at("numerator")) << (e - ea), y = integer(b.at("numerator")) << (e - eb);
    return mpz_cmp(x.get_mpz_t(), y.get_mpz_t());
}
inline J add_mass(const std::vector<J> &v) {
    unsigned e = 0;
    for (const auto &r : v)
        e = std::max(e, r.at("denominator_power").get<unsigned>());
    Z n = 0;
    for (const auto &r : v)
        n += integer(r.at("numerator")) << (e - r.at("denominator_power").get<unsigned>());
    return dyadic(n, e);
}
inline J scale_mass(const J &m, std::uint64_t n) {
    return dyadic(integer(m.at("numerator")) * Z(std::to_string(n)), m.at("denominator_power"));
}

inline std::string show_mass(const J &m) {
    if (m.is_null() || m.value("numerator", std::string("0")) == "0")
        return "미산정";
    std::ostringstream s;
    s << "2^-" << std::fixed << std::setprecision(9) << m.at("F").get<double>();
    return s.str();
}

inline std::string sha256(const std::string &input) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<unsigned char> d(input.begin(), input.end());
    std::uint64_t bits = std::uint64_t(d.size()) * 8;
    d.push_back(0x80);
    while (d.size() % 64 != 56)
        d.push_back(0);
    for (int j = 7; j >= 0; --j)
        d.push_back(static_cast<unsigned char>(bits >> (8 * j)));
    auto rr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t off = 0; off < d.size(); off += 64) {
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 4; ++j)
                w[i] = (w[i] << 8) | d[off + 4 * i + j];
        for (int i = 16; i < 64; ++i) {
            auto a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + (rr(a, 7) ^ rr(a, 18) ^ (a >> 3)) + w[i - 7] +
                   (rr(b, 17) ^ rr(b, 19) ^ (b >> 10));
        }
        auto a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            auto t = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            auto u = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = dd + t;
            dd = c;
            c = b;
            b = a;
            a = t + u;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += dd;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::ostringstream s;
    for (auto x : h)
        s << std::hex << std::setw(8) << std::setfill('0') << x;
    return s.str();
}
inline std::string digest(const J &j) { return sha256(nlohmann::json(j).dump(-1, ' ', true)); }
inline J read_table(const fs::path &p) {
    std::ifstream f(p);
    require(bool(f), "Missing native table " + p.string());
    std::string line;
    std::getline(f, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    auto keys = split(line, '\t');
    J rows = J::array();
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        auto vals = split(line, '\t');
        require(vals.size() == keys.size(), "Malformed native table row");
        J row = J::object();
        for (std::size_t i = 0; i < keys.size(); ++i)
            row[keys[i]] = vals[i];
        rows.push_back(row);
    }
    return rows;
}
inline std::uint64_t available_memory() {
#ifdef __linux__
    std::ifstream f("/proc/meminfo");
    std::string key, unit;
    std::uint64_t n = 0, available = 0;
    while (f >> key >> n) {
        std::getline(f, unit);
        if (key == "MemAvailable:") {
            available = n * 1024;
            break;
        }
    }
    require(available > 0, "Cannot detect MemAvailable");
    for (auto pair :
         {std::pair<const char *, const char *>{"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory.current"},
          {"/sys/fs/cgroup/memory/memory.limit_in_bytes", "/sys/fs/cgroup/memory/memory.usage_in_bytes"}}) {
        if (fs::exists(pair.first) && fs::exists(pair.second)) {
            auto cap = read_text(pair.first);
            if (cap.find("max") == std::string::npos) {
                auto a = std::stoull(cap), b = std::stoull(read_text(pair.second));
                available = std::min<std::uint64_t>(available, a > b ? a - b : 0);
            }
        }
    }
    return available;
#else
    throw std::runtime_error("Linux/WSL required");
#endif
}
inline std::uint64_t rss(int pid) {
#ifdef __linux__
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream s(line.substr(6));
            std::uint64_t k = 0;
            s >> k;
            return k * 1024;
        }
#endif
    return 0;
}
}
