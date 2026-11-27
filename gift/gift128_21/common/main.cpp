#include "report.hpp"
using namespace thermo;
static void stop_handler(int) {
    stopping.store(true);
}
int main(int argc, char **argv) {
    try {
        Options options(argc, argv);
        if (options.help) {
            Options::usage();
            return 0;
        }
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);
        auto set=[](const char*k,std::uint64_t v){auto s=std::to_string(v);setenv(k,s.c_str(),1);};
        set("THERMO_COLLAPSED_EVERY",options.collapsed_every);set("THERMO_COLLAPSED_WORK",options.collapsed_work);
        set("THERMO_BRANCH_WORK",options.branch_work);set("THERMO_FINE_PLAN_BITS",options.fine_plan_bits);
        set("THERMO_LOCAL_BOUND_WORK",options.local_bound_work);
        const auto memory=Campaign::memory_budget(options,available_memory());
        auto cache_mib=options.message_cache_mib==UINT64_MAX
            ? std::min<std::uint64_t>({16384,64ULL*std::min(options.cores,available_cpus()),memory/(64*MiB)})
            : std::min<std::uint64_t>(options.message_cache_mib,memory/(8*MiB));
        mass_messages::Arena cache(cache_mib);
        set("THERMO_MESSAGE_CACHE_BYTES",cache.bytes);
        Campaign campaign(std::move(options));
        return campaign.execute();
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
