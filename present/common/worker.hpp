#pragma once
#include "runtime.hpp"
namespace present_native {
inline J tsv(const fs::path &path) {
    std::ifstream f(path);
    require(bool(f), "Missing native receipt");
    std::string line;
    require(bool(std::getline(f, line)), "Missing TSV header");
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    auto header = split(line, '\t');
    J rows = J::array();
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        auto cells = split(line, '\t');
        require(cells.size() == header.size(), "Malformed native TSV receipt");
        J row = J::object();
        for (std::size_t i = 0; i < header.size(); ++i)
            row[header[i]] = cells[i];
        rows.push_back(row);
    }
    return rows;
}
inline double real(const J &x) {
    auto s = str(x);
    if (s == "-inf")
        return -INFINITY;
    return std::stod(s);
}
inline bool close(double a, double b) {
    return a == b || std::abs(a - b) <= std::max(1e-8, 1e-12 * std::max(std::abs(a), std::abs(b)));
}
inline J execute(Model &model, const fs::path &root, const J &job) {
    double start = now(), cpu_start = thread_cpu();
    std::string kind = job.at("kind");
    J result = {{"kind", kind}, {"status", "INCOMPLETE"}, {"cpu_seconds", 0}};
    std::optional<Process> receipt;
    try {
        TempDir tmp(fs::temp_directory_path());
        auto temp = tmp.path;
        double seconds = num(job, "seconds"), deadline = num(job, "deadline_monotonic", INFINITY);
        auto run = [&](std::vector<std::string> cmd, double grace = 10) {
            receipt = invoke(std::move(cmd), seconds, deadline, temp, grace);
            return *receipt;
        };
        auto arg = [](const J &v) { return str(v); };
        if (kind == "pair") {
            auto ep = split(job.at("endpoint"), '_');
            auto output = temp / "pair.json";
            std::vector<std::string> cmd = {(root / "bin/pair").string(),
                                            "--input",
                                            ep[0],
                                            "--output",
                                            ep[1],
                                            "--cap",
                                            arg(job["cap"]),
                                            "--threads",
                                            arg(job["threads"]),
                                            "--seconds",
                                            arg(job["seconds"]),
                                            "--nodes",
                                            arg(job["max_nodes"]),
                                            "--memory",
                                            arg(job["address_bytes"]),
                                            "--token",
                                            arg(job["job_index"]),
                                            "--work",
                                            job["work"],
                                            "--out",
                                            output.string()};
            auto p = run(cmd, 120);
            auto progress =
                fs::exists(output) ? output : fs::path(job["work"].get<std::string>()) / "progress.json";
            J d = fs::exists(progress) ? read_json(progress) : J::object();
            bool resource = false;
            for (auto prefix : {"SAVED_FRONTIER_EXCEEDS_RAM_BUDGET", "JOIN_RAM_LIMIT", "std::bad_alloc",
                                "MERGE_ABORT_RELOAD_CHECKPOINT", "WORK_DIRECTORY_BUSY"})
                if (p.err.rfind(prefix, 0) == 0)
                    resource = true;
            if (!d.empty() && get(d, "token") == job["job_index"]) {
                require(d["input"] == ep[0] && d["output"] == ep[1] && d["cap"] == job["cap"] &&
                            iv(d, "denominator_power") ==
                                3 * activity(ep[0]) + 3 * iv(job, "cap") * (model.R - 1),
                        "Stale/mismatched endpoint exact progress");
                result.update(
                    J{{"status", d["status"]}, {"pair_progress", d}, {"cpu_seconds", get(d, "cpu_seconds")}});
                if (truth(get(d, "probability_complete"))) {
                    auto w = model.encode_trail(model.catalog(J::array({model.pattern(job["states"])})),
                                                job["states"]);
                    J row = {{"input", ep[0]},
                             {"output", ep[1]},
                             {"cap", job["cap"]},
                             {"states", job["states"]},
                             {"numerator", d["numerator"]},
                             {"denominator_power", d["denominator_power"]},
                             {"trail_weight", w["witness_weight"]},
                             {"first_seconds", now() - num(job, "submitted_monotonic")}};
                    result["corridor_records"] = J::array({model.corridor_record(row)});
                }
                result["cpu_time_is_lower_bound"] = !p.known || !fs::exists(output);
                if (p.known && p.code > 0 && !resource)
                    throw std::runtime_error("Endpoint exact engine: " + p.err);
                if (p.known && p.code != 0 && !truth(get(d, "probability_complete")))
                    result.update(J{{"reason", p.err}, {"status", "INCOMPLETE"}});
            } else if (p.known && p.code == 0)
                throw std::runtime_error("Exact pair returned without this job's progress");
            else if (!p.known || p.code < 0 || resource)
                result["reason"] = p.err.empty() ? "PAIR_RELOAD_DURABLE_CHECKPOINT" : p.err;
            else
                throw std::runtime_error("Endpoint exact engine: " + p.err);
        } else if (kind == "exact" || kind == "plan") {
            J compiled = model.compile(job["states"]);
            J quick = kind == "plan" ? model.singleton(compiled) : J();
            if (!quick.is_null()) {
                result.update(quick);
                result["job_wall_seconds"] = now() - start;
                result["python_thread_cpu_seconds"] = thread_cpu() - cpu_start;
                return result;
            }
            J parent = job.value("box", J::array());
            auto file = temp / "exact.txt", checkpoint = temp / "checkpoint.json";
            write_atomic(file, model.adaptive_text(compiled, J::array({parent})));
            std::vector<std::string> cmd = {(root / "bin/exact").string(),
                                            kind == "plan" ? "plan" : "eval",
                                            file.string(),
                                            checkpoint.string(),
                                            arg(job["address_bytes"]),
                                            arg(job["table_bytes"]),
                                            arg(job["leaf_operations"]),
                                            arg(job["seconds"]),
                                            arg(job["max_frontier"]),
                                            "2"};
            auto p = run(cmd);
            if (p.known && p.code == 0)
                result.update(J::parse(p.out));
            else if (kind == "exact" && fs::exists(checkpoint) && (!p.known || p.code < 0)) {
                result.update(read_json(checkpoint));
                result["reason"] = p.err.empty() ? "RECOVERED_CHECKPOINT" : p.err;
            } else if (!p.known || (kind == "plan" && (p.err.find("CHUNK_TIME_LIMIT") != std::string::npos ||
                                                       p.err.find("std::bad_alloc") != std::string::npos)))
                result["reason"] = p.err;
            else
                throw std::runtime_error("Exact engine: " + p.err);
            if (result["status"] == "EXACT_PROGRESS" || result["status"] == "EXACT_DONE") {
                Model::verify(compiled, parent, result);
                require(result["solved_nodes"].is_number_integer() &&
                            result["zero_nodes"].is_number_integer() && iv(result, "zero_nodes") >= 0 &&
                            iv(result, "zero_nodes") <= iv(result, "solved_nodes"),
                        "Invalid completed subproblem count");
            }
            result["witness_weight"] = compiled["witness_weight"];
        } else {
            auto out = temp / "result.tsv";
            std::vector<std::string> cmd = {(root / "bin/search").string(),
                                            kind == "screen" ? "ais" : kind,
                                            "--out",
                                            out.string(),
                                            "--seconds",
                                            arg(job["seconds"]),
                                            "--seed",
                                            arg(job["seed"]),
                                            "--limit",
                                            arg(job.value("keep", J(16)))};
            J cat, witness;
            auto add = [&](std::initializer_list<std::string> values) { cmd.insert(cmd.end(), values); };
            if (kind == "corridor") {
                add({"--center", arg(job["cap"]), "--width", arg(job["input_active"]), "--copies",
                     arg(job["copies"]), "--corridor-nodes", arg(job["max_nodes"]), "--sweeps",
                     arg(job["max_edges"]), "--start-active", arg(job.value("start_active", J(0))),
                     "--memory-bytes", arg(job["address_bytes"])});
                if (truth(get(job, "initial")))
                    add({"--initial", job["initial"]});
            } else if (kind == "seed" || kind == "pattern")
                add({"--active", arg(job["active"]), "--sweeps", "1000000000"});
            else if (kind == "ensemble") {
                const auto &seeds = job["initial_trails"];
                require(seeds.size() >= 1 && seeds.size() <= 16, "Invalid joint seed count");
                std::ostringstream text;
                text << "PRESENT_ENSEMBLE_V1 " << model.R << ' ' << seeds.size() << '\n';
                for (const auto &states : seeds) {
                    require(int(states.size()) == model.R + 1, "Wrong-round ensemble seed");
                    for (const auto &s : states)
                        text << s.get<std::string>() << '\n';
                }
                auto file = temp / "ensemble.txt";
                write_atomic(file, text.str());
                add({"--bridge", file.string(), "--copies", arg(job["copies"]), "--active",
                     arg(job["active"]), "--sweeps", "1000000000"});
            } else if (kind == "search" || kind == "screen") {
                cat = model.catalog(J::array({job["pattern"]}));
                auto file = temp / "model.txt";
                write_atomic(file, model.native_text(cat));
                add({"--model", file.string()});
                if (truth(get(job, "states")))
                    witness = model.encode_trail(cat, job["states"]);
                if (kind == "search") {
                    add({"--copies", arg(job["copies"]), "--sweeps", "1000000000"});
                    if (!witness.is_null())
                        add({"--initial", witness["coordinates"].get<std::string>() +
                                              witness["witness_internal"].get<std::string>()});
                } else {
                    Deadline dl{deadline};
                    J ds = model.reduction(cat, witness.at("coordinates"), dl);
                    require(!truth(get(ds, "proven_zero")), "Witnessed family cannot have zero mass");
                    file = temp / "domains.txt";
                    std::ostringstream text;
                    text << "PRESENT_DOMAINS_V2 0 " << cat["patterns"][0]["d"] << '\n';
                    for (int v : cat["patterns"][0]["used"]) {
                        const auto &d = ds["domains"][std::to_string(v)];
                        text << v << ' ' << d.size();
                        for (int x : d)
                            text << ' ' << x;
                        text << '\n';
                    }
                    write_atomic(file, text.str());
                    add({"--endpoint", witness["coordinates"], "--pattern", "0", "--domains", file.string(),
                         "--paths", arg(job["paths"]), "--steps", arg(job["steps"])});
                }
            } else if (kind == "expand") {
                auto file = temp / "bridge.txt";
                std::ostringstream text;
                text << "PRESENT_BRIDGE" << model.R << "_V2\n";
                for (const auto &s : job["states"])
                    text << s.get<std::string>() << '\n';
                write_atomic(file, text.str());
                add({"--bridge", file.string(), "--center", arg(job["center"]), "--width", "8", "--sweeps",
                     "1000000000"});
            } else
                throw std::runtime_error("Unknown job kind");
            double offset = now() - num(job, "submitted_monotonic");
            auto p = run(cmd);
            if (kind == "corridor" && (!p.known || p.code < 0) && fs::exists(out.string() + ".checkpoint")) {
                out = out.string() + ".checkpoint";
                p.code = 0;
                p.known = true;
                result["reason"] = "RECOVERED_COMPLETED_ROW_ARCHIVE";
                result["cpu_time_is_lower_bound"] = true;
            }
            if (fs::exists(out.string() + ".metrics.json"))
                result.update(read_json(out.string() + ".metrics.json"));
            if (p.known && p.code == 1)
                throw std::runtime_error("Search engine: " + p.err);
            if (!p.known || p.code != 0 || !fs::exists(out))
                result["reason"] = p.err.empty() ? "native incomplete" : p.err;
            else {
                auto rows = tsv(out);
                result["status"] = "COMPLETE";
                if (kind == "corridor") {
                    J records = J::array();
                    for (const auto &row : rows) {
                        J r = model.corridor_record(row);
                        require(r["cap"] == job["cap"] && activity(r["input"]) <= iv(job, "input_active"),
                                "Native transfer differs from dispatched domain");
                        r["first_seconds"] = num(r, "first_seconds") + offset;
                        records.push_back(r);
                    }
                    result["corridor_records"] = records;
                } else if (kind == "pattern") {
                    J ps = J::array();
                    for (const auto &row : rows)
                        ps.push_back(split(row["pattern"], ','));
                    result["patterns"] = ps;
                } else if (kind == "search") {
                    J hits = J::array();
                    for (const auto &row : rows) {
                        auto t = model.decode(cat, row["endpoint"], row["internal"]);
                        require(close(num(t, "weight"), real(row["trail_weight"])),
                                "Search witness weight mismatch");
                        hits.push_back(J{{"states", t["states"]},
                                         {"hard", std::stoll(str(row["hard_visits"]))},
                                         {"soft", std::stoll(str(row["soft_visits"]))},
                                         {"first_seconds", offset + real(row["first_seconds"])}});
                    }
                    result["hits"] = hits;
                } else if (kind == "expand" || kind == "seed" || kind == "ensemble") {
                    J hits = J::array();
                    for (const auto &row : rows)
                        hits.push_back(J{{"states", split(row["states"], ',')},
                                         {"first_seconds", offset + real(row["first_seconds"])}});
                    result["hits"] = hits;
                } else {
                    require(rows.size() == std::size_t(iv(job, "paths")), "Incomplete AIS batch");
                    std::vector<double> logs;
                    for (std::size_t i = 0; i < rows.size(); ++i) {
                        auto &row = rows[i];
                        double w = real(row["log_weight"]);
                        J tw = model.terminal_weight(cat, row["endpoint"], row["internal"]);
                        require(std::stoull(str(row["path_id"])) == i &&
                                    row["endpoint"] == witness["coordinates"] && str(row["pattern"]) == "0" &&
                                    close(real(row["trail_weight"]), tw.is_null() ? -1 : tw.get<double>()) &&
                                    (tw.is_null() == (w == -INFINITY)) && !std::isnan(w) && w != INFINITY,
                                "Invalid AIS path");
                        logs.push_back(w);
                    }
                    double top = -INFINITY;
                    for (auto w : logs)
                        if (std::isfinite(w))
                            top = std::max(top, w);
                    if (std::isfinite(top)) {
                        long double total = 0, squares = 0, maximum = 0;
                        int positive = 0;
                        for (auto w : logs) {
                            double x = std::isfinite(w) ? std::exp(w - top) : 0;
                            total += x;
                            squares += x * x;
                            maximum = std::max(maximum, static_cast<long double>(x));
                            positive += std::isfinite(w);
                        }
                        result.update(J{{"F", -(top + std::log(double(total / logs.size()))) / std::log(2.)},
                                        {"ess", double(total * total / squares)},
                                        {"positive", positive},
                                        {"max_weight_fraction", double(maximum / total)},
                                        {"paths", logs.size()}});
                    } else
                        result.update(
                            J{{"F", nullptr}, {"ess", 0}, {"positive", 0}, {"max_weight_fraction", 0}});
                    result["paths"] = logs.size();
                    result["sampling_is_only_screening"] = true;
                }
                std::string suffix = kind == "corridor"   ? ".corridor.json"
                                     : kind == "ensemble" ? ".ensemble.json"
                                     : kind == "search"   ? ".search.json"
                                                          : ".expand.json";
                if (fs::exists(out.string() + suffix))
                    result["diagnostics"] = read_json(out.string() + suffix);
                else if (kind == "corridor")
                    result["diagnostics"] = J{{"stop", "HARD_TIMEOUT_ARCHIVE_RECOVERED"}};
            }
        }
    } catch (const Cancelled &) {
        result.update(J{{"status", kind == "exact" ? "RESOURCE_DEFERRED" : "INCOMPLETE"},
                        {"reason", "CAMPAIGN_TIME_LIMIT"}});
    } catch (const std::exception &e) {
        result.update(J{{"status", "ERROR"}, {"reason", e.what()}});
    }
    result["job_wall_seconds"] = now() - start;
    result["python_thread_cpu_seconds"] = thread_cpu() - cpu_start;
    if (receipt) {
        result["cpu_seconds"] = receipt->cpu;
        result["peak_rss_kib"] = receipt->rss;
        result["cpu_time_is_lower_bound"] = false;
        result["cpu_accounting"] = "wait4 per child, including all OpenMP threads";
    }
    return result;
}
}
