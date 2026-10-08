#include "lrdp/platform/headless_server.hpp"
#include "lrdp/platform/unique_fd.hpp"
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
namespace lrdp {
namespace {
constexpr auto configuration = R"(Section "ServerFlags"
 Option "AutoAddDevices" "false"
 Option "AutoEnableDevices" "false"
 Option "AutoAddGPU" "false"
 Option "DontVTSwitch" "true"
 Option "DontZap" "true"
 Option "DontZoom" "true"
EndSection
Section "Device"
 Identifier "LRDP"
 Driver "dummy"
 VideoRam 262144
EndSection
Section "Monitor"
 Identifier "LRDPMonitor"
 HorizSync 1.0-1000.0
 VertRefresh 1.0-200.0
 Modeline "1280x720" 74.5 1280 1344 1472 1664 720 723 728 748
EndSection
Section "Screen"
 Identifier "LRDPScreen"
 Device "LRDP"
 Monitor "LRDPMonitor"
 DefaultDepth 24
 SubSection "Display"
  Depth 24
  Modes "1280x720"
 EndSubSection
EndSection
)";
void write_file(const std::string& path, View bytes) {
    UniqueFd fd(open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    require(bool(fd),"cannot create private desktop file");
    while (!bytes.empty()) {
        const auto n = write(fd.get(),bytes.data(),bytes.size());
        if (n < 0 && errno == EINTR) continue;
        require(n > 0,"cannot write private desktop file"); bytes = bytes.subspan(std::size_t(n));
    }
}
void authority_file(const std::string& path, std::string_view number, View cookie) {
    Writer out(256); out.be16(65535).be16(0).be16(unsigned(number.size()));
    out.raw(View(reinterpret_cast<const std::uint8_t*>(number.data()),number.size()));
    constexpr std::string_view name = "MIT-MAGIC-COOKIE-1";
    out.be16(unsigned(name.size())).raw(View(reinterpret_cast<const std::uint8_t*>(name.data()),name.size()));
    out.be16(unsigned(cookie.size())).raw(cookie); write_file(path,out.bytes());
}
}
struct HeadlessServer::Impl {
    std::string directory, display_name, auth;
    // Explicit cleanup order also applies when initialization throws.
    ChildProcess server, application;
    ~Impl() {
        application.stop(); server.stop();
        if (!directory.empty()) { std::error_code error; std::filesystem::remove_all(directory,error); }
    }
};
HeadlessServer::HeadlessServer(const HeadlessOptions& options) : impl_(std::make_unique<Impl>()) {
    require(getuid() != 0 && geteuid() == getuid(),"headless desktop must run as a non-root Unix user without setuid");
    require(options.xorg.starts_with('/') && !options.command.empty() && options.command[0].starts_with('/'),"headless executables must use absolute paths");
    char pattern[] = "/tmp/lrdp-session-XXXXXX"; char* created = mkdtemp(pattern);
    require(created != nullptr,"cannot create private headless runtime directory"); impl_->directory = created;
    const auto config = impl_->directory+"/xorg.conf", server_auth = impl_->directory+"/server.auth";
    write_file(config,View(reinterpret_cast<const std::uint8_t*>(configuration),std::char_traits<char>::length(configuration)));
    require(mkdir((impl_->directory+"/empty").c_str(),0700) == 0,"cannot create private Xorg configuration directory");
    std::array<std::uint8_t,16> cookie{};
    struct Wipe { std::array<std::uint8_t,16>& bytes; ~Wipe() { OPENSSL_cleanse(bytes.data(),bytes.size()); } } wipe{cookie};
    require(RAND_bytes(cookie.data(),int(cookie.size())) == 1,"cannot generate X11 authentication cookie");
    authority_file(server_auth,"",cookie);
    int descriptors[2]; require(pipe2(descriptors,O_CLOEXEC|O_NONBLOCK) == 0,"cannot create Xorg readiness pipe");
    UniqueFd read_end(descriptors[0]), write_end(descriptors[1]);
    // The writer is blocking: Xorg writes only a few bytes during startup.
    const auto flags = fcntl(write_end.get(),F_GETFL); require(flags >= 0 && fcntl(write_end.get(),F_SETFL,flags&~O_NONBLOCK) == 0,"cannot configure readiness pipe");
    UniqueFd log(open((impl_->directory+"/process.log").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600));
    require(bool(log),"cannot create headless diagnostics");
    impl_->server.start({options.xorg,"-displayfd","3","-config",config,"-configdir",impl_->directory+"/empty",
        "-auth",server_auth,"-nolisten","tcp","-noreset","-novtswitch","-sharevts","-logfile",impl_->directory+"/Xorg.log"},
        {{"HOME",impl_->directory}}, {"LD_PRELOAD","LD_LIBRARY_PATH"},log.get(),write_end.get());
    write_end.reset(); std::string number;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (number.empty() || number.back() != '\n') {
        require(impl_->server.alive(),"private Xorg exited during startup; install the dummy DDX and run without root");
        require(std::chrono::steady_clock::now() < deadline,"private Xorg startup timed out");
        pollfd ready{read_end.get(),POLLIN,0}; const auto rc = poll(&ready,1,50);
        if (rc < 0 && errno == EINTR) continue;
        require(rc >= 0,"Xorg readiness poll failed"); if (!rc) continue;
        char bytes[16]; const auto count = read(read_end.get(),bytes,sizeof(bytes));
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        require(count > 0 && number.size()+std::size_t(count) <= 8,"invalid Xorg display response"); number.append(bytes,std::size_t(count));
    }
    number.pop_back(); unsigned value = 0; const auto parsed = std::from_chars(number.data(),number.data()+number.size(),value);
    require(!number.empty() && parsed.ec == std::errc{} && parsed.ptr == number.data()+number.size() && value <= 65535,"invalid Xorg display number");
    impl_->display_name = ':'+number; impl_->auth = impl_->directory+"/client.auth";
    authority_file(impl_->auth,number,cookie);
}
HeadlessServer::~HeadlessServer() = default;
const std::string& HeadlessServer::display() const { return impl_->display_name; }
const std::string& HeadlessServer::authority() const { return impl_->auth; }
void HeadlessServer::start_desktop(const HeadlessOptions& options) {
    UniqueFd log(open((impl_->directory+"/desktop.log").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600));
    require(bool(log),"cannot create desktop diagnostics");
    impl_->application.start(options.command,{{"DISPLAY",display()},{"XAUTHORITY",authority()},{"XDG_SESSION_TYPE","x11"}},
        {"WAYLAND_DISPLAY","SESSION_MANAGER","DBUS_SESSION_BUS_ADDRESS","LD_PRELOAD","LD_LIBRARY_PATH"},log.get());
}
void HeadlessServer::check() const {
    require(impl_->server.alive(),"private Xorg display exited");
    if (impl_->application.pid() > 0) require(impl_->application.alive(),"configured desktop process exited");
}
}
