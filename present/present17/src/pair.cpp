#include "rounds.hpp"
#include "pair_transfer.hpp"
#include <iostream>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/file.h>
#endif

int main(int argc,char** argv){
    using namespace pair_transfer;
    try{
        Options o;std::map<std::string,std::string> args;
        for(int i=1;i<argc;i+=2){
            if(i+1==argc||!args.emplace(argv[i],argv[i+1]).second)throw std::runtime_error("missing/repeated argument");
        }
        const std::array<std::string,10> allowed={"--input","--output","--cap","--threads","--seconds","--nodes","--memory","--token","--work","--out"};
        for(const auto& x:args)if(std::find(allowed.begin(),allowed.end(),x.first)==allowed.end())throw std::runtime_error("unknown argument: "+x.first);
        for(const auto& k:allowed)if(!args.count(k))throw std::runtime_error("missing argument: "+k);
        auto number=[&](const std::string& k){const auto& s=args.at(k);
            if(s.empty()||s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid integer: "+k);
            return std::stoull(s);};
        o.a=word(args.at("--input"));o.b=word(args.at("--output"));
        const auto cap=number("--cap"),threads=number("--threads");
        o.nodes=number("--nodes");o.memory=number("--memory");o.token=number("--token");
        std::size_t used=0;o.seconds=std::stod(args.at("--seconds"),&used);
        if(used!=args.at("--seconds").size()||!std::isfinite(o.seconds)||o.seconds<=0||
           !o.a||!o.b||cap<1||cap>16||threads<1||threads>100||o.nodes<128)
            throw std::runtime_error("invalid exact pair configuration");
        o.cap=int(cap);o.threads=int(threads);o.work=args.at("--work");o.out=args.at("--out");
        omp_set_dynamic(0);
#ifdef __linux__
        if(o.memory){rlimit previous{};
            if(getrlimit(RLIMIT_AS,&previous))throw std::runtime_error("cannot inspect RAM limit");
            rlimit limit{std::min(static_cast<rlim_t>(o.memory),previous.rlim_cur),previous.rlim_max};
            if(setrlimit(RLIMIT_AS,&limit))throw std::runtime_error("cannot set RAM limit");
        }
#endif
        std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
#ifdef __linux__
        const pid_t parent=getppid();
        if(prctl(PR_SET_PDEATHSIG,SIGTERM))throw std::runtime_error("cannot install parent-death signal");
        if(getppid()!=parent||parent==1)interrupted=1;
        fs::create_directories(o.work);
        if(fs::is_symlink(o.work))throw std::runtime_error("scratch task directory must not be a symlink");
        struct TaskLock{int fd=-1;~TaskLock(){if(fd>=0)::close(fd);}} lock;
        lock.fd=::open((o.work/"worker.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(lock.fd<0)throw std::runtime_error("cannot open exact task lock");
        if(flock(lock.fd,LOCK_EX|LOCK_NB))throw std::runtime_error("WORK_DIRECTORY_BUSY");
#endif
        Timer timer(o.seconds);const int bits=3*activity(o.a)+3*o.cap*(ROUNDS-1)+1;
        if(bits<=256){Engine<corridor_integer::Integer<8>,ROUNDS> engine(o,timer);engine.run();}
        else if(bits<=512){Engine<corridor_integer::Integer<16>,ROUNDS> engine(o,timer);engine.run();}
        else{Engine<corridor_integer::Integer<32>,ROUNDS> engine(o,timer);engine.run();}
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
