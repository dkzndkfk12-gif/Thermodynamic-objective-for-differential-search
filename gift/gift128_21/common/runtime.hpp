#pragma once
#include "exact_tools.hpp"
#include <condition_variable>
#include <future>
#include <queue>
#include <deque>
#include <sqlite3.h>
#ifdef __linux__
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char **environ;
#endif
namespace thermo {
struct TaskState {
    std::atomic<int> phase{0}, pid{0};
    std::atomic<bool> queued_ram_wait{false};
    std::atomic<std::uint64_t> reserved{0}, table{0};
    std::atomic<double> phase_started{0}, worker_started{0};
    void set(int p) {
        phase = p;
        phase_started = now();
    }
};
inline const char *phase_name(int p) {
    switch (p) {
    case 0:
        return "queued";
    case 1:
        return "preparing";
    case 2:
        return "native_computation";
    case 3:
        return "verifying";
    case 4:
        return "complete";
    case 5:
        return "waiting_for_RAM";
    }
    return "unknown";
}
#include "../../shared/work_pool.hpp"
class Database {
    sqlite3 *db = nullptr;

  public:
    explicit Database(const fs::path &path, bool ro = false) {
        const bool existing = fs::exists(path) && fs::file_size(path) > 0;
        int flags = ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        int r = sqlite3_open_v2(path.string().c_str(), &db, flags, nullptr);
        if (r != SQLITE_OK) {
            std::string e = db ? sqlite3_errmsg(db) : "open failed";
            if (db)
                sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error(e);
        }
        sqlite3_busy_timeout(db, 5000);
        try {

            if (existing) {
                sqlite3_stmt *stmt = nullptr;
                bool ok = sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &stmt, nullptr) == SQLITE_OK;
                std::string error = sqlite3_errmsg(db);
                if (ok) {
                    const int row = sqlite3_step(stmt);
                    const auto *value = row == SQLITE_ROW ? sqlite3_column_text(stmt, 0) : nullptr;
                    ok = value && std::string(reinterpret_cast<const char *>(value)) == "ok";
                    error = value ? reinterpret_cast<const char *>(value) : sqlite3_errmsg(db);
                    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
                }
                if (stmt) sqlite3_finalize(stmt);
                require(ok, "SQLite ledger integrity check failed: " + path.string() + ": " + error +
                    ". Keep this file; resume from an intact server ledger or a consistent SQLite backup. No automatic repair performed.");
            }
            if (!ro) {
                exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
                for (auto t : {"patterns", "cases", "meta", "jobs", "meta_parts"})
                    exec(std::string("CREATE TABLE IF NOT EXISTS ") + t +
                         "(id TEXT PRIMARY KEY, data TEXT NOT NULL)");
            }
        } catch (...) {
            sqlite3_close(db); db = nullptr;
            throw;
        }
    }
    ~Database() {
        if (db)
            sqlite3_close(db);
    }
    void exec(const std::string &s) {
        if(s.rfind("ROLLBACK",0)==0)meta_parts_valid=false;
        char *error = nullptr;
        if (sqlite3_exec(db, s.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            std::string msg = error ? error : sqlite3_errmsg(db);
            sqlite3_free(error);
            throw std::runtime_error("SQLite: " + msg);
        }
    }
    void put_raw(const std::string &table, const std::string &id, const J &v) {
        sqlite3_stmt *s = nullptr;
        std::string sql = "INSERT OR REPLACE INTO " + table + "(id,data) VALUES(?,?)";
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK, "SQLite prepare");
        auto data = v.dump();
        if(table=="meta_parts")meta_bytes_written+=data.size();
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, data.c_str(), int(data.size()), SQLITE_TRANSIENT);
        int r = sqlite3_step(s);
        sqlite3_finalize(s);
        require(r == SQLITE_DONE, "SQLite write");
    }
    void erase(const std::string &table, const std::string &id) {
        sqlite3_stmt *s = nullptr;
        auto sql = "DELETE FROM " + table + " WHERE id=?";
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK, "SQLite delete prepare");
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        int r = sqlite3_step(s);
        sqlite3_finalize(s);
        require(r == SQLITE_DONE, "SQLite delete");
    }
    std::map<std::string, J> load_raw(const std::string &table) {
        std::map<std::string, J> out;
        sqlite3_stmt *s = nullptr;
        auto sql = "SELECT id,data FROM " + table;
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK,
                "SQLite read prepare " + table);
        try {
            int r;
            while ((r = sqlite3_step(s)) == SQLITE_ROW) {
                auto id = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
                auto data = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
                out[id] = J::parse(data);
            }
            require(r == SQLITE_DONE, "SQLite read failed");
            sqlite3_finalize(s);
        } catch (...) {
            sqlite3_finalize(s);
            throw;
        }
        return out;
    }
#include "../../shared/meta_parts.inc"
};
struct RunLock {
    int fd = -1;
    explicit RunLock(const fs::path &p) {
#ifdef __linux__
        fd = open(p.c_str(), O_CREAT | O_RDWR, 0600);
        require(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0, "Output directory is already in use");
#else
        (void)p;
#endif
    }
    ~RunLock() {
#ifdef __linux__
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            close(fd);
        }
#endif
    }
};
struct ProcessResult {
    int code = 0;
    double cpu = 0, wall = 0;
    long peak_rss = 0;
    bool forced = false;
    std::string out, err;
};
inline ProcessResult invoke(std::vector<std::string> args, const fs::path &dir, const Deadline &dl,
                            const std::shared_ptr<TaskState> &state, double grace = 10) {
    ProcessResult result;
#ifdef __linux__
    dl.check();
    fs::create_directories(dir);
    auto op = dir / "stdout.txt", ep = dir / "stderr.txt";
    std::vector<char *> av;
    for (auto &s : args)
        av.push_back(s.data());
    av.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "spawn actions");
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, op.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, ep.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = 0;
    auto start = now();
    int rc = posix_spawnp(&pid, av[0], &actions, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    require(rc == 0, "Cannot start native worker: " + std::string(std::strerror(rc)));
    state->pid = pid;
    state->set(2);
    bool sent = false;
    double signal_time = 0;
    int status = 0;
    rusage usage{};
    while (true) {
        pid_t p = wait4(pid, &status, WNOHANG, &usage);
        if (p == pid)
            break;
        if (p < 0 && errno != EINTR) {
            state->pid = 0;
            throw std::runtime_error("wait4 failed");
        }
        if (!sent && (stopping.load() || now() >= dl.end)) {
            kill(pid, SIGTERM);
            sent = true;
            signal_time = now();
        }
        if (sent && now() - signal_time >= grace) {
            kill(pid, SIGKILL);
            result.forced = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    state->pid = 0;
    result.wall = now() - start;
    result.cpu = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
                 (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
    result.peak_rss = usage.ru_maxrss;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    result.out = read_text(op);
    result.err = read_text(ep);
#else
    (void)args;
    (void)dir;
    (void)dl;
    (void)state;
    (void)grace;
    throw std::runtime_error("Linux/WSL required");
#endif
    return result;
}

class Resources {
    std::mutex mu;
    std::map<std::uint64_t, std::weak_ptr<TaskState>> states;
    std::uint64_t initial, headroom, last_budget;
    std::uint64_t version = 0;
    std::uint64_t shared_preparation_reserve = 0;
    int workers;
    double observed = -1;
    static std::uint64_t unshared_rss(int pid) {
        if(!mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0))return rss(pid);
        std::ifstream f("/proc/"+std::to_string(pid)+"/status");std::string line;
        std::uint64_t resident=0,shared=0;
        while(std::getline(f,line)) {
            std::istringstream in(line);std::string key;std::uint64_t kb;
            if(in>>key>>kb){if(key=="VmRSS:")resident=kb*1024;else if(key=="RssShmem:")shared=kb*1024;}
        }
        return resident>=shared?resident-shared:0;
    }
    static std::uint64_t committed_cache() {
#ifdef __linux__
        const auto path=std::getenv("THERMO_MESSAGE_CACHE_PATH");struct stat st{};
        if(path&&!stat(path,&st))return std::min(mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0),std::uint64_t(st.st_blocks)*512);
#endif
        return 0;
    }
    std::uint64_t live_locked() {
        if (now() - observed < 1)
            return last_budget;
        std::uint64_t own = 0;
#ifdef __linux__
        own = sat_add(unshared_rss(int(getpid())),committed_cache());
#endif
        for (auto i = states.begin(); i != states.end();) {
            auto s = i->second.lock();
            if (!s)
                i = states.erase(i);
            else {
                int pid = s->pid.load();
                if (pid)
                    own = sat_add(own, unshared_rss(pid));
                ++i;
            }
        }
        auto total = sat_add(available_memory(), own);
        auto budget = std::min(initial, total > headroom ? total - headroom : 0);
        if (budget != last_budget)
            ++version;
        last_budget = budget;
        observed = now();
        return last_budget;
    }
    std::uint64_t used_locked() {
        std::uint64_t n = 0;
        for (auto &i : states)
            if (auto s = i.second.lock())
                n = sat_add(n, s->reserved.load());
        return n;
    }
    std::uint64_t controller_locked() const {
#ifdef __linux__
        return sat_add(std::max(sat_add(controller,shared_preparation_reserve), rss(int(getpid()))),mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0));
#else
        return sat_add(controller,shared_preparation_reserve);
#endif
    }

  public:
    static constexpr std::uint64_t overhead = 256 * MiB, controller = 512 * MiB, minimum = 16 * MiB;
    Resources(std::uint64_t available, std::uint64_t budget, int n)
        : initial(budget), headroom(available > budget ? available - budget : 0), last_budget(budget),
          workers(n) {}
    void reserve_shared_preparation(std::uint64_t bytes) {
        std::lock_guard<std::mutex> l(mu);shared_preparation_reserve=bytes;++version;
    }
    bool attach(std::uint64_t id, const std::shared_ptr<TaskState> &s) {
        std::lock_guard<std::mutex> l(mu);
        auto budget = live_locked(), used = used_locked();
        if (sat_add(sat_add(used, controller_locked()), overhead) > budget)
            return false;
        s->reserved = overhead;
        states[id] = s;
        return true;
    }
    bool native(const std::shared_ptr<TaskState> &s, std::uint64_t desired, std::uint64_t required,
                std::uint64_t cap, std::uint64_t &table) {
        std::lock_guard<std::mutex> l(mu);
        auto budget = live_locked(), used = used_locked();
        auto own = s->reserved.load();
        auto commitments = sat_add(controller_locked(), used >= own ? used - own : 0);
        auto free = budget > commitments ? budget - commitments : 0;
        if (free <= overhead)
            return false;
        auto allowed = free - overhead;
        if (cap)
            allowed = std::min(allowed, cap);
        if (required > allowed || allowed < minimum)
            return false;
        table = std::min(allowed, std::max({minimum, required, desired}));
        s->reserved = table + overhead;
        s->table = table;
        return true;
    }
    void release(const std::shared_ptr<TaskState> &s) {
        std::lock_guard<std::mutex> l(mu);

        if (s->reserved.load() && s->phase.load() != 0)
            ++version;
        s->reserved = 0;
        s->table = 0;
    }
    std::uint64_t admission_version() {
        std::lock_guard<std::mutex> l(mu);
        live_locked();
        return version;
    }
    std::uint64_t base_table(int evalslots) {
        std::lock_guard<std::mutex> l(mu);
        auto b = live_locked(), r = controller_locked() + overhead * workers;
        return b > r ? std::max(minimum, (b - r) / std::uint64_t(std::max(1, evalslots))) : minimum;
    }
    std::uint64_t maximum_table(unsigned future_workers, std::uint64_t cap = 0) {
        std::lock_guard<std::mutex> l(mu);
        auto b = live_locked();
        auto reserve = sat_add(controller_locked(), sat_mul(overhead, std::uint64_t(future_workers) + 1));
        auto maximum = b > reserve ? b - reserve : 0;
        return cap ? std::min(maximum, cap) : maximum;
    }
    J status() {
        std::lock_guard<std::mutex> l(mu);
        auto b = live_locked();
        std::uint64_t lo = UINT64_MAX, hi = 0;
        std::uint64_t parent_rss = 0, child_rss = 0;
#ifdef __linux__
        parent_rss = rss(int(getpid()));
#endif
        for (auto &i : states)
            if (auto s = i.second.lock()) {
                if (s->pid.load())
                    child_rss = sat_add(child_rss, rss(s->pid.load()));
                auto n = s->table.load();
                if (n) {
                    lo = std::min(lo, n);
                    hi = std::max(hi, n);
                }
            }
        return J{{"policy", "coherent_CPP_RAM_admission_v1"},
                 {"active_reserved_bytes", used_locked()},
                 {"current_admission_budget_bytes", b},
                 {"initial_budget_bytes", initial},
                 {"active_table_bytes_min", lo == UINT64_MAX ? 0 : lo},
                 {"active_table_bytes_max", hi},
                 {"controller_actual_rss_bytes", parent_rss},
                 {"native_children_actual_rss_bytes", child_rss},
                 {"actual_rss_sum_bytes", sat_add(parent_rss, child_rss)},
                 {"reservations_are_actual_RSS", false}};
    }
};
template <class T> class Cache {
    struct Entry {
        std::shared_ptr<T> value;
        std::mutex mu;
        std::uint64_t tick = 0;
    };
    std::mutex guard;
    std::map<std::string, std::shared_ptr<Entry>> entries;
    std::uint64_t tick = 0;
    std::size_t limit;

  public:
    explicit Cache(std::size_t max) : limit(max) {}
    template <class F> std::shared_ptr<T> get(const std::string &key, const Deadline &dl, F create) {
        std::shared_ptr<Entry> e;
        {
            std::lock_guard<std::mutex> l(guard);
            auto &item = entries[key];
            if (!item)
                item = std::make_shared<Entry>();
            e = item;
            e->tick = ++tick;
            while (entries.size() > limit) {
                auto oldest = entries.end();
                for (auto it = entries.begin(); it != entries.end(); ++it)
                    if (it->second.use_count() == 1 &&
                        (oldest == entries.end() || it->second->tick < oldest->second->tick))
                        oldest = it;
                if (oldest == entries.end())
                    break;
                entries.erase(oldest);
            }
        }
        std::unique_lock<std::mutex> l(e->mu, std::defer_lock);
        while (!l.try_lock()) {
            dl.check();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!e->value) {
            dl.check();
            e->value = std::make_shared<T>(create());
        }
        return e->value;
    }
};
}
