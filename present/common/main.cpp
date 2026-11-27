#include "controller.hpp"
int main(int argc, char **argv) {
    using namespace present_native;
    try {
        fs::path variant, out = "runs/run";
        J options = J::object();
        bool positional = false;
        for (int i = 1; i < argc; ++i) {
            std::string s = argv[i];
            if (s == "--help" || s == "-h") {
                std::cout << "bash run.sh OUTPUT_DIR [--cores N --hours H --seed N]\n";
                return 0;
            }
            if (s == "--build-seconds") {
                require(++i < argc, "Missing build time");
                double b = std::stod(argv[i]);
                require(std::isfinite(b) && b >= 0, "Invalid build time");
                options["build_seconds"] = b;
            } else if (s == "--variant") {
                require(++i < argc, "Missing variant");
                variant = argv[i];
            } else if (s == "--cores") {
                require(++i < argc, "Missing cores");
                std::string v = argv[i];
                std::size_t n;
                auto cores = std::stoll(v, &n);
                require(n == v.size() && cores >= 0 && cores <= INT_MAX, "Invalid cores");
                options["cores"] = cores;
            } else if (s == "--hours" || s == "--wall-hours") {
                require(++i < argc, "Missing hours");
                std::string v = argv[i];
                std::size_t n;
                double h = std::stod(v, &n);
                require(n == v.size() && std::isfinite(h) && h > 0, "Invalid hours");
                options["wall_hours"] = h;
            } else if (s == "--seed") {
                require(++i < argc, "Missing seed");
                std::string v = argv[i];
                require(!v.empty() && v.find_first_not_of("0123456789") == std::string::npos, "Invalid seed");
                options["seed"] = std::stoull(v);
            } else if (!s.empty() && s[0] != '-' && !positional) {
                out = s;
                positional = true;
            } else
                throw std::runtime_error("Unknown argument: " + s);
        }
        require(!variant.empty(), "Missing variant directory");
        std::signal(SIGINT, [](int) { stopping = true; });
        std::signal(SIGTERM, [](int) { stopping = true; });
        setenv("OMP_NUM_THREADS", "1", 1);
        setenv("OPENBLAS_NUM_THREADS", "1", 1);
        setenv("MKL_NUM_THREADS", "1", 1);
        Campaign campaign(variant, out, options);
        return campaign.execute();
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
