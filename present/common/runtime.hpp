#pragma once
#include "model.hpp"
#include <condition_variable>
#include <future>
#include <queue>
#include <unordered_map>
#include <sqlite3.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <signal.h>
extern char **environ;
namespace present_native {
class Records {
    struct Entry {
        J value;
        std::list<std::string>::iterator order;
    };
    std::list<std::string> order;
    std::unordered_map<std::string, Entry> data;
    bool valid = true;

  public:
    Records() = default;
    Records(const J &j) { *this = j; }
    Records(const Records &other) { *this = other.json(); }
    Records(Records &&) = default;
    Records &operator=(Records &&) = default;
    Records &operator=(const Records &other) {
        if (this != &other)
            *this = other.json();
        return *this;
    }
    Records &operator=(const J &j) {
        data.clear();
        order.clear();
        valid = !j.is_null();
        if (!j.is_null()) {
            require(j.is_object(), "Expected record object");
            for (auto it = j.begin(); it != j.end(); ++it)
                (*this)[it.key()] = it.value();
        }
        return *this;
    }
    J &operator[](const std::string &key) {
        valid = true;
        auto it = data.find(key);
        if (it != data.end())
            return it->second.value;
        order.push_back(key);
        return data.emplace(key, Entry{J(), std::prev(order.end())}).first->second.value;
    }
    J &at(const std::string &key) { return data.at(key).value; }
    const J &at(const std::string &key) const { return data.at(key).value; }
    const J &operator[](const std::string &key) const { return at(key); }
    bool contains(const std::string &key) const { return data.find(key) != data.end(); }
    void erase(const std::string &key) {
        auto it = data.find(key);
        if (it != data.end()) {
            order.erase(it->second.order);
            data.erase(it);
        }
    }
    std::size_t size() const { return data.size(); }
    bool empty() const { return data.empty(); }
    bool is_null() const { return !valid; }
    void clear() {
        data.clear();
        order.clear();
        valid = false;
    }
    template <bool Const> struct Iter {
        using Store = std::conditional_t<Const, const Records, Records>;
        Store *owner;
        std::list<std::string>::const_iterator position;
        auto &operator*() const { return owner->at(*position); }
        auto *operator->() const { return &owner->at(*position); }
        Iter &operator++() {
            ++position;
            return *this;
        }
        bool operator!=(const Iter &o) const { return position != o.position; }
        bool operator==(const Iter &o) const { return position == o.position; }
        const std::string &key() const { return *position; }
        auto &value() const { return **this; }
    };
    Iter<false> begin() { return {this, order.begin()}; }
    Iter<false> end() { return {this, order.end()}; }
    Iter<true> begin() const { return {this, order.begin()}; }
    Iter<true> end() const { return {this, order.end()}; }
    J json() const {
        if (!valid)
            return nullptr;
        J out = J::object();
        auto &object = out.get_ref<J::object_t &>();
        object.reserve(data.size());
        for (const auto &key : order)
            object.emplace_back(key, at(key));
        return out;
    }
    friend void to_json(J &out, const Records &r) { out = r.json(); }
};
inline const J &get(const Records &r, std::string key) {
    static const J empty;
    if (!r.contains(key))
        return empty;
    return r.at(key);
}
inline J &def(Records &r, const std::string &key, const J &value) {
    if (!r.contains(key))
        r[key] = value;
    return r[key];
}
inline double num(const Records &r, const std::string &key, double fallback=0) {
    const auto& value=get(r,key);
    return value.is_null()?fallback:value.is_string()?std::stod(value.get<std::string>()):value.get<double>();
}
inline long long iv(const Records &r, const std::string &key, long long fallback=0) {
    const auto& value=get(r,key);
    return value.is_null()?fallback:value.is_string()?std::stoll(value.get<std::string>()):value.get<long long>();
}
inline void inc(Records &r,const std::string& key,double n=1){r[key]=num(r,key)+n;}
inline void inci(Records &r,const std::string& key,long long n=1){r[key]=iv(r,key)+n;}
struct Pool {
    std::mutex mu;
    std::condition_variable cv;
    std::queue<std::function<void()>> queue;
    std::vector<std::thread> threads;
    bool stopped = false;
    explicit Pool(int n) {
        for (int i = 0; i < n; ++i)
            threads.emplace_back([this] {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> l(mu);
                        cv.wait(l, [&] { return stopped || !queue.empty(); });
                        if (stopped && queue.empty())
                            return;
                        fn = std::move(queue.front());
                        queue.pop();
                    }
                    fn();
                }
            });
    }
    std::future<J> submit(std::function<J()> fn) {
        auto task = std::make_shared<std::packaged_task<J()>>(std::move(fn));
        auto f = task->get_future();
        {
            std::lock_guard<std::mutex> l(mu);
            require(!stopped, "Pool stopped");
            queue.push([task] { (*task)(); });
        }
        cv.notify_one();
        return f;
    }
    void join() {
        {
            std::lock_guard<std::mutex> l(mu);
            stopped = true;
        }
        cv.notify_all();
        for (auto &t : threads)
            if (t.joinable())
                t.join();
    }
    ~Pool() { join(); }
};
struct Database {
    sqlite3 *db = nullptr;
    explicit Database(const fs::path &path) {
        int rc = sqlite3_open(path.c_str(), &db);
        require(rc == SQLITE_OK, "Cannot open SQLite ledger");
        sqlite3_busy_timeout(db, 5000);
        exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        for (auto t : {"patterns", "cases", "meta"})
            exec(std::string("CREATE TABLE IF NOT EXISTS ") + t + "(id TEXT PRIMARY KEY,data TEXT NOT NULL)");
    }
    ~Database() {
        if (db)
            sqlite3_close(db);
    }
    void exec(const std::string &sql) {
        char *e = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &e) != SQLITE_OK) {
            std::string msg = e ? e : sqlite3_errmsg(db);
            sqlite3_free(e);
            throw std::runtime_error("SQLite: " + msg);
        }
    }
    std::vector<std::pair<std::string, J>> load(const std::string &table) {
        sqlite3_stmt *s = nullptr;
        require(sqlite3_prepare_v2(db, ("SELECT id,data FROM " + table).c_str(), -1, &s, nullptr) ==
                    SQLITE_OK,
                "SQLite read prepare");
        std::vector<std::pair<std::string, J>> out;
        int rc;
        try {
            while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
                auto id = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
                auto data = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
                out.push_back({id, J::parse(data)});
            }
            require(rc == SQLITE_DONE, "SQLite read failed");
        } catch (...) {
            sqlite3_finalize(s);
            throw;
        }
        sqlite3_finalize(s);
        return out;
    }
    void put(const std::string &table, const std::string &id, const J &value) {
        sqlite3_stmt *s = nullptr;
        require(sqlite3_prepare_v2(db, ("INSERT OR REPLACE INTO " + table + " VALUES (?,?)").c_str(), -1, &s,
                                   nullptr) == SQLITE_OK,
                "SQLite write prepare");
        std::string data = value.dump();
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, data.c_str(), int(data.size()), SQLITE_TRANSIENT);
        int rc = sqlite3_step(s);
        sqlite3_finalize(s);
        require(rc == SQLITE_DONE, "SQLite write failed");
    }
    void erase(const std::string &table, const std::string &id) {
        sqlite3_stmt *s = nullptr;
        require(sqlite3_prepare_v2(db, ("DELETE FROM " + table + " WHERE id=?").c_str(), -1, &s, nullptr) ==
                    SQLITE_OK,
                "SQLite erase prepare");
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(s);
        sqlite3_finalize(s);
        require(rc == SQLITE_DONE, "SQLite erase failed");
    }
};
struct RunLock {
    int fd = -1;
    explicit RunLock(const fs::path &path) {
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        require(fd >= 0, "Cannot open run lock");
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            fd = -1;
            throw std::runtime_error("This run is already active");
        }
    }
    ~RunLock() {
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }
};
inline std::set<fs::path> cgroups() {
    std::set<fs::path> out = {"/sys/fs/cgroup", "/sys/fs/cgroup/memory", "/sys/fs/cgroup/cpu"};
    std::ifstream f("/proc/self/cgroup");
    std::string line;
    while (std::getline(f, line)) {
        auto a = line.find(':'), b = line.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        auto cs = line.substr(a + 1, b - a - 1), rel = line.substr(b + 1);
        std::vector<std::string> controllers = cs.empty() ? std::vector<std::string>{""} : split(cs, ',');
        for (const auto &c : controllers) {
            fs::path root = "/sys/fs/cgroup";
            if (!c.empty())
                root /= c;
            auto path = (root / fs::path(rel).relative_path()).lexically_normal();
            for (;;) {
                if (path == root || path.string().rfind(root.string() + "/", 0) == 0)
                    out.insert(path);
                else
                    break;
                if (path == root)
                    break;
                path = path.parent_path();
            }
        }
    }
    return out;
}
inline std::uint64_t available_memory() {
    std::vector<std::uint64_t> values;
    std::ifstream f("/proc/meminfo");
    std::string label;
    std::uint64_t x;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        if (in >> label >> x && label == "MemAvailable:")
            values.push_back(x * 1024);
    }
    for (const auto &p : cgroups())
        for (auto pair : {std::make_pair("memory.max", "memory.current"),
                          std::make_pair("memory.limit_in_bytes", "memory.usage_in_bytes")})
            try {
                auto s = read_text(p / pair.first);
                if (s.find("max") == std::string::npos) {
                    Z cap(s), used(read_text(p / pair.second));
                    Z free = cap > used ? Z(cap - used) : Z(0);
                    values.push_back(free > pow2(64) - 1 ? UINT64_MAX : free.get_ui());
                }
            } catch (...) {
            }
    require(!values.empty(), "Available RAM cannot be detected; Linux/WSL required");
    return *std::min_element(values.begin(), values.end());
}
inline int available_cores() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    int n = sched_getaffinity(0, sizeof(mask), &mask) == 0 ? CPU_COUNT(&mask)
                                                           : int(std::thread::hardware_concurrency());
    n = std::max(1, n);
    for (const auto &p : cgroups()) {
        try {
            std::istringstream f(read_text(p / "cpu.max"));
            std::string q;
            long long period;
            f >> q >> period;
            if (q != "max" && period > 0)
                n = std::min(n, std::max(1, int(std::ceil(std::stold(q) / period))));
        } catch (...) {
        }
        try {
            long long q = std::stoll(read_text(p / "cpu.cfs_quota_us")),
                      period = std::stoll(read_text(p / "cpu.cfs_period_us"));
            if (q > 0 && period > 0)
                n = std::min(n, std::max(1, int(std::ceil(double(q) / period))));
        } catch (...) {
        }
    }
    return n;
}
inline J layout(const J &a, std::uint64_t avail = 0, int cores = 0) {
    if (!avail)
        avail = available_memory();
    if (!cores)
        cores = available_cores();
    int requested = std::min(iv(a, "cores") ? int(iv(a, "cores")) : cores, cores);
    require(requested >= 3, "At least 3 available CPU cores are required");
    bool automatic = num(a, "memory_gib") == 0;
    std::uint64_t memory =
        std::min(automatic ? avail * 85 / 100 : std::uint64_t(num(a, "memory_gib") * GiB), avail);
    std::uint64_t coordinator =
                      std::max<std::uint64_t>(256 * MiB, std::min<std::uint64_t>(4 * GiB, memory / 20)),
                  address = 0, table = 0;
    bool transfer = !a["corridor_caps"].empty();
    int workers = requested - 1, exact = 0, heavy = 0, light = 0;
    auto minimum = std::max<std::uint64_t>(
        16 * MiB, num(a, "exact_job_gib") ? std::min<std::uint64_t>(128 * MiB, num(a, "exact_job_gib") * GiB)
                                          : 128 * MiB);
    for (; workers >= 2; --workers) {
        exact = std::max(1, workers / 3);
        heavy = transfer ? workers : exact;
        light = workers - heavy;
        if (memory <= coordinator + std::uint64_t(light) * 128 * MiB)
            continue;
        address = (memory - coordinator - light * 128 * MiB) / heavy;
        table = address > 192 * MiB ? (address - 192 * MiB) * 85 / 100 : 0;
        if (num(a, "exact_job_gib"))
            table = std::min<std::uint64_t>(table, num(a, "exact_job_gib") * GiB);
        if (table >= minimum)
            break;
    }
    require(workers >= 2, "Available RAM is insufficient for two concurrent native jobs");
    address = std::min(address, (table * 100 + 84) / 85 + 192 * MiB);
    J r = {{"cores", workers + 1},
           {"workers", workers},
           {"exact_slots", exact},
           {"search_slots", workers - exact},
           {"table_bytes", table},
           {"address_bytes", address},
           {"native_budget_bytes", memory},
           {"coordinator_cores", 1},
           {"resource_policy", "auto_ram_v1"},
           {"detected_available_bytes", avail},
           {"available_cpu_cores", cores},
           {"requested_cpu_cores", requested},
           {"ram_limited_workers", workers < requested - 1},
           {"automatic_memory_fraction", automatic ? J(.85) : J()},
           {"coordinator_reserve_bytes", coordinator},
           {"heavy_job_slots", heavy},
           {"light_job_reserve_bytes", 128 * MiB},
           {"reserved_peak_bytes", coordinator + light * 128 * MiB + heavy * address},
           {"max_pending_boxes", a["max_pending_boxes"]}};
    if (transfer) {
        r["corridor_effective_max_nodes"] = iv(a, "corridor_max_nodes")
                                                ? std::uint64_t(iv(a, "corridor_max_nodes"))
                                                : std::max<std::uint64_t>(128, table / 256);
        r["corridor_effective_max_edges"] =
            iv(a, "corridor_max_edges") ? std::uint64_t(iv(a, "corridor_max_edges")) : UINT64_MAX;
        r["corridor_edges_follow_time_budget"] = !iv(a, "corridor_max_edges");
    }
    return r;
}
struct TempDir {
    fs::path path;
    explicit TempDir(const fs::path &base) {
        std::string s = (base / "present-native-XXXXXX").string();
        std::vector<char> b(s.begin(), s.end());
        b.push_back(0);
        auto p = mkdtemp(b.data());
        require(p, "Cannot allocate temporary work directory");
        path = p;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
struct Process {
    int code = 0;
    bool known = false;
    std::string out, err;
    double cpu = 0;
    long rss = 0;
};
inline std::mutex process_mutex;
inline std::set<pid_t> processes;
inline void stop_all() {
    stopping = true;
    std::lock_guard<std::mutex> l(process_mutex);
    for (auto p : processes)
        kill(p, SIGTERM);
}
inline Process invoke(std::vector<std::string> args, double seconds, double deadline, const fs::path &temp,
                      double grace = 10) {
    Process r;
    if (stopping) {
        r.err = "STOP";
        return r;
    }
    seconds = std::min(seconds, deadline - now());
    if (seconds <= 0) {
        r.err = "CAMPAIGN_TIME_LIMIT";
        return r;
    }
    auto pos = std::find(args.begin(), args.end(), "--seconds");
    if (pos != args.end())
        *(pos + 1) = std::to_string(seconds);
    else {
        require(args.size() > 7, "Malformed exact command");
        args[7] = std::to_string(seconds);
    }
    fs::path output = temp / "stdout.txt", error = temp / "stderr.txt";
    int outfd = open(output.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600),
        errfd = open(error.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
    if (outfd < 0 || errfd < 0) {
        if (outfd >= 0)
            close(outfd);
        if (errfd >= 0)
            close(errfd);
        throw std::runtime_error("Cannot create native receipt streams");
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outfd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errfd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, outfd);
    posix_spawn_file_actions_addclose(&actions, errfd);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    require(posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0,
            "Cannot isolate native file descriptors");
#endif
#endif
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outfd);
    close(errfd);
    require(rc == 0, "Cannot launch native executable: " + args[0]);
    {
        std::lock_guard<std::mutex> l(process_mutex);
        processes.insert(pid);
        if (stopping)
            kill(pid, SIGTERM);
    }
    double end = now() + seconds + grace;
    bool killed = false;
    int status;
    struct rusage usage{};
    for (;;) {
        pid_t got;
        int wait_error;
        {
            std::lock_guard<std::mutex> l(process_mutex);
            got = wait4(pid, &status, WNOHANG, &usage);
            wait_error = errno;
            if (got == pid || (got < 0 && wait_error != EINTR))
                processes.erase(pid);
        }
        if (got == pid)
            break;
        if (got < 0 && wait_error != EINTR) {
            throw std::runtime_error("wait4 failed");
        }
        if (now() >= end && !killed) {
            kill(pid, SIGKILL);
            killed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    r.out = read_text(output);
    r.err = read_text(error);
    if (r.err.size() > 2000)
        r.err.erase(0, r.err.size() - 2000);
    r.cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec * 1e-6 + usage.ru_stime.tv_sec +
            usage.ru_stime.tv_usec * 1e-6;
    r.rss = usage.ru_maxrss;
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    r.known = !killed && !(stopping && r.code != 0);
    if (killed)
        r.err = "NATIVE_TIMEOUT: " + r.err;
    if (stopping && !r.known)
        r.err = "STOP";
    return r;
}
}
