#include "lrdp/session.hpp"
#include "lrdp/transport.hpp"
#ifdef LRDP_HAVE_GSSAPI
#include "lrdp/security/nla_transport.hpp"
#endif
#ifdef LRDP_HAVE_PORTAL
#include "lrdp/platform/portal_desktop.hpp"
#endif
#include <arpa/inet.h>
#include <charconv>
#include <csignal>
#include <iostream>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace lrdp;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
unsigned number(std::string_view text, unsigned maximum) {
    unsigned value = 0; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0 && value <= maximum, "invalid numeric argument"); return value;
}
struct Configuration {
    std::string certificate, key, backend = "demo", display, listen = "127.0.0.1", service;
    std::set<std::string> principals;
    unsigned port = 3389, fps = 30, max_sessions = 4;
    bool laboratory = false, nla = false, allow_ntlm = false, once = false, graphics = true;
    VideoOptions video;
};
void usage() {
    std::cout << "LRDP native Linux remote desktop server\n"
              << "  --cert FILE --key FILE\n"
              << "  --auth nla --service TERMSRV@host.example.org --allow-principal user@REALM\n"
              << "  [--allow-ntlm] [--listen 127.0.0.1] [--port 3389] [--max-sessions 4]\n"
              << "  [--backend demo|x11|portal] [--display :0] [--fps 30] [--once]\n"
              << "  [--gfx auto|off] [--encoder auto|software|vaapi|nvenc|raw] [--device /dev/dri/renderD128]\n"
              << "  --allow-principal can be repeated; matching is exact and case-sensitive.\n"
              << "  Alternatively: --lab-no-auth (loopback-only, no user authentication).\n"
              << "  NLA uses system GSS credentials; desktop access runs as the server's Unix user.\n";
}
Configuration parse(int argc, char** argv) {
    Configuration c;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        auto value = [&]() -> std::string { require(i + 1 < argc, "missing option value"); return argv[++i]; };
        if (option == "--cert") c.certificate = value(); else if (option == "--key") c.key = value();
        else if (option == "--backend") c.backend = value(); else if (option == "--display") c.display = value();
        else if (option == "--listen") c.listen = value(); else if (option == "--port") c.port = number(value(), 65535);
        else if (option == "--fps") c.fps = number(value(), 120);
        else if (option == "--max-sessions") c.max_sessions = number(value(), 64);
        else if (option == "--encoder") c.video.backend = value(); else if (option == "--device") c.video.device = value();
        else if (option == "--gfx") { const auto mode = value(); require(mode == "auto" || mode == "off", "invalid graphics mode"); c.graphics = mode == "auto"; }
        else if (option == "--auth") { require(value() == "nla", "only --auth nla is supported"); c.nla = true; }
        else if (option == "--service") c.service = value();
        else if (option == "--allow-principal") { const auto name = value(); require(!name.empty() && name.size() <= 1024, "invalid principal policy"); c.principals.insert(name); }
        else if (option == "--allow-ntlm") c.allow_ntlm = true;
        else if (option == "--lab-no-auth") c.laboratory = true; else if (option == "--once") c.once = true;
        else throw ProtocolError("unknown command-line option: " + option);
    }
    require(c.nla != c.laboratory, "select either authenticated --auth nla or explicit loopback --lab-no-auth");
    require(!c.certificate.empty() && !c.key.empty(), "TLS certificate and key are required");
    require(!c.laboratory || c.listen == "127.0.0.1" || c.listen == "::1", "unauthenticated laboratory access must remain on loopback");
    require(!c.nla || (!c.service.empty() && !c.principals.empty()), "NLA requires a service identity and at least one allowed principal");
    require(c.nla || (!c.allow_ntlm && c.service.empty() && c.principals.empty()), "authentication options cannot be used in laboratory mode");
    require(c.backend == "demo" || c.backend == "x11" || c.backend == "portal", "unknown desktop backend");
#ifndef LRDP_HAVE_GSSAPI
    require(!c.nla, "this build has no system GSSAPI support");
#endif
#ifndef LRDP_HAVE_X11
    require(c.backend != "x11", "this build has no X11 support");
#endif
#ifndef LRDP_HAVE_PORTAL
    require(c.backend != "portal", "this build has no portal/PipeWire support");
#endif
    require(c.video.backend == "auto" || c.video.backend == "software" || c.video.backend == "vaapi" ||
            c.video.backend == "nvenc" || c.video.backend == "raw", "invalid video encoder");
#ifndef LRDP_HAVE_FFMPEG
    require(c.video.backend == "auto" || c.video.backend == "raw", "this build has no FFmpeg video support");
#endif
    c.video.fps = c.fps; return c;
}
int bind_listener(const Configuration& c) {
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* raw = nullptr;
    require(getaddrinfo(c.listen.c_str(), std::to_string(c.port).c_str(), &hints, &raw) == 0, "--listen requires a numeric IPv4 or IPv6 address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
    for (auto* a = raw; a; a = a->ai_next) {
        const int fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) continue;
        int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (a->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
        if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 16) == 0) return fd;
        close(fd);
    }
    throw ProtocolError("cannot bind RDP listening address");
}
int serve(int fd, TlsContext& context, const Configuration& c, const VideoFactory& video) {
    try {
        const auto negotiation = parse_negotiation(read_negotiation(fd));
        const std::uint32_t protocol = c.nla ? 2U : 1U;
        if (!(negotiation.protocols & protocol) || (negotiation.flags & 3)) {
            write_raw(fd, negotiation_reply(c.nla ? 5 : 1, true)); throw ProtocolError("required RDP security protocol was not offered");
        }
        write_raw(fd, negotiation_reply(protocol)); TlsStream stream(fd, context);
#ifdef LRDP_HAVE_GSSAPI
        if (c.nla) {
            const auto principal = authenticate_nla(stream.native_tls(), fd, {c.service, c.allow_ntlm},
                [&](const std::string& identity) { return c.principals.contains(identity); });
            std::cout << "NLA principal authorized: " << principal << '\n' << std::flush;
        }
#endif
        // Do not obtain desktop access or trigger user consent before authorization.
        std::unique_ptr<Desktop> desktop;
#ifdef LRDP_HAVE_X11
        if (c.backend == "x11") desktop = make_x11_desktop(c.display);
#endif
#ifdef LRDP_HAVE_PORTAL
        if (c.backend == "portal") desktop = make_portal_desktop();
#endif
        if (c.backend == "demo") desktop = make_demo_desktop();
        require(desktop != nullptr, "selected desktop backend unavailable");
        Session session(std::move(desktop), negotiation.protocols, protocol, video, c.graphics);
        const auto start = Clock::now(); auto next_frame = start, last_receive = start, last_progress = start;
        std::size_t previous_queued = 0; bool announced = false; std::string graphics_status;
        while (running && session.phase() != SessionPhase::closed) {
            stream.pump(10);
            while (auto packet = stream.packet()) {
                struct Wipe { Bytes& bytes; ~Wipe() { OPENSSL_cleanse(bytes.data(), bytes.size()); } } wipe{*packet};
                session.receive(*packet); last_receive = Clock::now(); stream.enqueue(session.drain());
            }
            const auto now = Clock::now();
            if (!announced && session.active()) { std::cout << "Session active\n" << std::flush; announced = true; }
            require(announced || now - start < std::chrono::seconds(20), "session activation deadline exceeded");
            require(now - last_receive < std::chrono::minutes(30), "idle session deadline exceeded");
            if (!stream.queued() || stream.queued() < previous_queued) last_progress = now;
            require(now - last_progress < std::chrono::seconds(15), "peer is not draining output"); previous_queued = stream.queued();
            const bool due = now >= next_frame;
            session.tick(stream.queued() == 0, due); stream.enqueue(session.drain());
            if (due) next_frame = now + std::chrono::microseconds(1000000 / c.fps);
            if (graphics_status != session.graphics_status()) {
                graphics_status = session.graphics_status(); std::cout << "Graphics: " << graphics_status << '\n' << std::flush;
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "Session ended: " << error.what() << '\n'; return 0; }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
        const auto c = parse(argc, argv);
        std::signal(SIGPIPE, SIG_IGN); std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        TlsContext context(c.certificate, c.key); Socket listener(bind_listener(c)); VideoFactory video;
#ifdef LRDP_HAVE_FFMPEG
        if (c.video.backend != "raw") video = ffmpeg_video_factory(c.video);
#endif
        std::cout << "LRDP listening on " << c.listen << ':' << c.port << " (" << (c.nla ? "NLA" : "TLS laboratory profile")
                  << ", backend=" << c.backend << ")\n" << std::flush;
        if (c.laboratory) std::cerr << "WARNING: No user authentication; do not forward this port to untrusted peers.\n";
        std::set<pid_t> children;
        while (running) {
            for (auto it = children.begin(); it != children.end();) {
                if (waitpid(*it, nullptr, WNOHANG) == *it) it = children.erase(it); else ++it;
            }
            pollfd ready{listener.get(), POLLIN, 0}; if (poll(&ready, 1, 250) <= 0) continue;
            Socket peer(accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)); if (peer.get() < 0) continue;
            if (children.size() >= c.max_sessions) continue;
            int yes = 1; setsockopt(peer.get(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            if (c.once) return serve(peer.get(), context, c, video);
            // Fork before opening GSS/desktop/encoder resources or creating any worker.
            const auto child = fork();
            if (child == 0) { close(listener.get()); const auto result = serve(peer.get(), context, c, video); std::cout.flush(); std::cerr.flush(); _exit(result); }
            require(child > 0, "cannot create isolated session process"); children.insert(child);
        }
        for (auto child : children) kill(child, SIGTERM);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!children.empty() && Clock::now() < deadline) {
            for (auto it = children.begin(); it != children.end();) {
                if (waitpid(*it, nullptr, WNOHANG) == *it) it = children.erase(it); else ++it;
            }
            if (!children.empty()) poll(nullptr, 0, 10);
        }
        for (auto child : children) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        return 0;
    } catch (const std::exception& error) { std::cerr << "LRDP: " << error.what() << '\n'; return 1; }
}
