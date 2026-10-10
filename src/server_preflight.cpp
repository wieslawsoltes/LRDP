#include "server_options.hpp"
#include "lrdp/transport.hpp"
#include "lrdp/platform/unique_fd.hpp"
#include <filesystem>
#include <netdb.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/un.h>

namespace lrdp::server {
namespace {
struct Build {
    bool x11 = false, portal = false, headless = false, gss = false;
    bool ffmpeg = false, audio = false, fuse = false, printing = false;
    Build() {
#ifdef LRDP_HAVE_X11
        x11 = true;
#endif
#ifdef LRDP_HAVE_PORTAL
        portal = true;
#endif
#ifdef LRDP_HAVE_HEADLESS
        headless = true;
#endif
#ifdef LRDP_HAVE_GSSAPI
        gss = true;
#endif
#ifdef LRDP_HAVE_FFMPEG
        ffmpeg = true;
#endif
#ifdef LRDP_HAVE_AUDIO
        audio = true;
#endif
#ifdef LRDP_HAVE_FUSE
        fuse = true;
#endif
#ifdef LRDP_HAVE_PRINTING
        printing = true;
#endif
    }
};
// Pin every directory component. No directory creation, socket connection,
// native display access, subprocess or hardware probe occurs in preflight.
UniqueFd directory(const std::string& input, bool private_mode, bool absolute) {
    require(!input.empty() && input.find('\0') == std::string::npos && input.size() < 3072,
            "invalid preflight directory path");
    require(!absolute || input.front() == '/', "native endpoint directory must be absolute");
    if (absolute) {
        // Match native control endpoints: do not normalize away a component
        // which their descriptor-relative resolver would reject at runtime.
        for (std::size_t start = 1; start < input.size();) {
            auto end = input.find('/', start);
            if (end == std::string::npos) end = input.size();
            const auto part = input.substr(start, end - start);
            require(!part.empty() && part != "." && part != "..", "invalid native endpoint directory component");
            start = end + 1;
        }
    }
    const auto path = std::filesystem::absolute(input).lexically_normal().string();
    require(path != "/", "native endpoint requires a dedicated directory");
    UniqueFd fd(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(bool(fd), "cannot inspect filesystem root");
    for (std::size_t start = 1; start < path.size();) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const auto part = path.substr(start, end - start);
        if (part.empty()) break; // lexically-normal trailing separator
        UniqueFd next(openat(fd.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        require(bool(next), "cannot inspect endpoint directory without symlinks");
        fd = std::move(next); start = end + 1;
    }
    struct stat st{};
    require(fstat(fd.get(), &st) == 0 && st.st_uid == geteuid() &&
            (private_mode ? (st.st_mode & 0777) == 0700 : !(st.st_mode & 0022)),
            "unsafe endpoint directory ownership or permissions");
    require(faccessat(fd.get(), ".", W_OK | X_OK, AT_EACCESS) == 0,
            "endpoint directory is not writable/searchable by this user");
    return fd;
}
#ifdef LRDP_HAVE_HEADLESS
void executable(const std::string& path) {
    struct stat st{};
    require(!path.empty() && path.front() == '/' && stat(path.c_str(), &st) == 0 &&
            S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0,
            "headless executable is missing, not absolute or not executable");
}
#endif
void broker(const std::string& path) {
    require(!path.empty() && path.front() == '/' && path.size() < sizeof(sockaddr_un::sun_path),
            "invalid session-broker socket path");
    const auto slash = path.find_last_of('/');
    auto parent = directory(path.substr(0, slash), true, true);
    const auto leaf = path.substr(slash + 1); struct stat st{};
    require(!leaf.empty() && fstatat(parent.get(), leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0600,
            "session-broker socket is missing or has unsafe ownership/permissions");
}
}
std::string capabilities() {
    const Build b;
    std::ostringstream out;
    out << std::boolalpha << "{\"schema\":1,\"version\":\"" << LRDP_VERSION
        << "\",\"kind\":\"compiled-capabilities\",\"hardware_probe_performed\":false,"
        << "\"backends\":{\"demo\":true,\"x11\":" << b.x11
        << ",\"portal\":" << b.portal << ",\"headless\":" << b.headless << "},"
        << "\"integrations\":{\"gssapi_nla\":" << b.gss << ",\"ffmpeg\":" << b.ffmpeg
        << ",\"pipewire_audio\":" << b.audio << ",\"fuse_drives\":" << b.fuse
        << ",\"raw_printers\":" << b.printing << ",\"headless_broker\":" << b.headless
        << ",\"lossless_graphics\":true,\"network_metrics\":true,\"zero_copy_capture\":false,\"pam_login_broker\":false}}";
    return out.str();
}
std::string preflight(const Configuration& c) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV; addrinfo* raw = nullptr;
    require(getaddrinfo(c.listen.c_str(), std::to_string(c.port).c_str(), &hints, &raw) == 0,
            "--listen requires a numeric IPv4 or IPv6 address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address(raw, freeaddrinfo);
    TlsContext tls(c.certificate, c.key);
    const auto* certificate = SSL_CTX_get0_certificate(tls.get());
    require(certificate && X509_cmp_current_time(X509_get0_notBefore(certificate)) == -1 &&
            X509_cmp_current_time(X509_get0_notAfter(certificate)) == 1,
            "TLS certificate is expired, not yet valid or has malformed validity times");
    if (c.backend == "headless" || !c.printer_root.empty() || !c.drive_root.empty())
        require(getuid() != 0 && geteuid() == getuid(), "native private endpoints require an ordinary non-setuid user");
    if (!c.clipboard_root.empty()) (void)directory(c.clipboard_root, false, false);
    if (!c.drive_root.empty()) (void)directory(c.drive_root, true, true);
    if (!c.printer_root.empty()) {
        (void)directory(c.printer_root, true, true);
        require(c.printer_root.size() + (c.printer_root.back() == '/' ? 41U : 42U) < sizeof(sockaddr_un::sun_path), "printer socket path would exceed Unix limits");
    }
    if (!c.broker_socket.empty()) broker(c.broker_socket);
#ifdef LRDP_HAVE_HEADLESS
    if (c.backend == "headless" && c.broker_socket.empty()) {
        executable(c.headless.xorg);
        require(!c.headless.command.empty(), "missing headless application");
        executable(c.headless.command.front());
    }
#endif
    // Do not serialize paths, service credentials, principal names or key bytes.
    std::ostringstream out;
    out << "{\"schema\":1,\"kind\":\"configuration-preflight\",\"valid\":true,"
        << "\"listener_opened\":false,\"desktop_opened\":false,\"security\":\""
        << (c.nla ? "nla" : "loopback-laboratory") << "\",\"authorized_principal_count\":" << c.principals.size()
        << ",\"backend\":\"" << c.backend << "\",\"encoder\":\"" << c.video.backend
        << "\",\"capabilities\":" << capabilities()
        << ",\"unchecked\":[\"certificate trust chain and remote hostname\",\"NLA credential/backend validity\","
        << "\"listener address availability\",\"native desktop, compositor consent and broker responsiveness\","
        << "\"GPU encoders, physical devices and live audio/drive endpoints\"]}";
    return out.str();
}
} // namespace lrdp::server
