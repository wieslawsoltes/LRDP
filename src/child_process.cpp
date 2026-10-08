#include "lrdp/platform/child_process.hpp"
#include "lrdp/platform/unique_fd.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <limits>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace lrdp {
ChildProcess::~ChildProcess() { stop(); }
bool ChildProcess::alive() const {
    if (pid_ < 0) return false;
    siginfo_t info{};
    int rc; do { rc = waitid(P_PID,id_t(pid_),&info,WEXITED|WNOHANG|WNOWAIT); } while (rc < 0 && errno == EINTR);
    return rc == 0 && info.si_pid == 0; // Leave zombies unreaped until stop(), preventing PID reuse.
}
void ChildProcess::start(const std::vector<std::string>& arguments, const std::map<std::string,std::string>& changes,
                         const std::vector<std::string>& remove, int log_fd, int display_fd) {
    require(pid_ < 0 && !arguments.empty() && arguments[0].starts_with('/'),"child requires an absolute executable path");
    std::vector<char*> argv; argv.reserve(arguments.size()+1);
    for (const auto& arg : arguments) { require(arg.find('\0') == std::string::npos,"embedded NUL in process argument"); argv.push_back(const_cast<char*>(arg.c_str())); }
    argv.push_back(nullptr);
    std::map<std::string,std::string> env;
    for (auto p = environ; p && *p; ++p) {
        const std::string value(*p); const auto equal = value.find('=');
        if (equal != std::string::npos) env[value.substr(0,equal)] = value.substr(equal+1);
    }
    for (const auto& key : remove) env.erase(key);
    for (const auto& [key,value] : changes) env[key] = value;
    std::vector<std::string> storage; storage.reserve(env.size());
    for (const auto& [key,value] : env) storage.push_back(key+'='+value);
    std::vector<char*> envp; envp.reserve(storage.size()+1);
    for (auto& value : storage) envp.push_back(value.data());
    envp.push_back(nullptr);
    UniqueFd null(open("/dev/null",O_RDONLY|O_CLOEXEC)); require(bool(null) && log_fd >= 0,"missing child I/O descriptors");
    // All memory and signal structures are prepared before fork, including when
    // this facility is called in a process that has already used a GSS provider.
    struct sigaction action{}; action.sa_handler = SIG_DFL; sigemptyset(&action.sa_mask);
    sigset_t mask; sigemptyset(&mask); const auto parent = getpid();
    const auto child = fork(); require(child >= 0,"cannot launch native desktop process");
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG,SIGTERM) != 0 || getppid() != parent || prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) != 0) _exit(125);
        for (int signal : {SIGTERM,SIGINT,SIGHUP,SIGPIPE,SIGCHLD}) sigaction(signal,&action,nullptr);
        sigprocmask(SIG_SETMASK,&mask,nullptr);
        if (setsid() < 0 || dup2(null.get(),0) < 0 || dup2(log_fd,1) < 0 || dup2(log_fd,2) < 0) _exit(125);
        if (display_fd >= 0 && (dup2(display_fd,3) < 0 || fcntl(3,F_SETFD,0) < 0)) _exit(125);
        if (syscall(SYS_close_range,display_fd >= 0 ? 4U : 3U,~0U,0) < 0) _exit(125);
        execve(argv[0],argv.data(),envp.data()); _exit(127);
    }
    pid_ = child;
}
void ChildProcess::stop() noexcept {
    if (pid_ < 0) return;
    kill(-pid_,SIGTERM); kill(pid_,SIGTERM);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (alive() && std::chrono::steady_clock::now() < deadline) poll(nullptr,0,10);
    // Kill any remaining members, even if the group leader has already exited.
    kill(-pid_,SIGKILL); kill(pid_,SIGKILL);
    while (waitpid(pid_,nullptr,0) < 0 && errno == EINTR) {}
    pid_ = -1;
}
