#include "server_options.hpp"
#include "lrdp/session.hpp"
#include "lrdp/transport.hpp"
#ifdef LRDP_HAVE_PRINTING
#include "lrdp/printing/native.hpp"
#endif
#include "lrdp/platform/clipboard_file_store.hpp"
#ifdef LRDP_HAVE_FUSE
#include "lrdp/platform/drive_mount.hpp"
#endif
#ifdef LRDP_HAVE_HEADLESS
#include "lrdp/platform/headless_desktop.hpp"
#include "lrdp/platform/persistent_desktop.hpp"
#endif
#ifdef LRDP_HAVE_GSSAPI
#include "lrdp/security/nla_transport.hpp"
#endif
#ifdef LRDP_HAVE_PORTAL
#include "lrdp/platform/portal_desktop.hpp"
#endif
#include <arpa/inet.h>
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
using namespace lrdp::server;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
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
        std::string principal = "lrdp:loopback-laboratory";
#ifdef LRDP_HAVE_GSSAPI
        if (c.nla) {
            principal = authenticate_nla(stream.native_tls(), fd, {c.service, c.allow_ntlm},
                [&](const std::string& identity) { return c.principals.contains(identity); });
            std::cout << "NLA principal authorized: " << principal << '\n' << std::flush;
        }
#endif
        std::unique_ptr<Desktop> desktop;
#ifdef LRDP_HAVE_X11
        if (c.backend == "x11") desktop = make_x11_desktop(c.display);
#endif
#ifdef LRDP_HAVE_PORTAL
        if (c.backend == "portal") desktop = make_portal_desktop();
#endif
#ifdef LRDP_HAVE_HEADLESS
        if (c.backend == "headless") desktop = c.broker_socket.empty() ? make_headless_desktop(c.headless)
            : make_persistent_desktop(c.broker_socket, c.laboratory ? principal : "nla:" + principal);
#endif
        if (c.backend == "demo") desktop = make_demo_desktop();
        require(desktop != nullptr, "selected desktop backend unavailable");
        Session session(std::move(desktop), negotiation.protocols, protocol, video, c.graphics);
        if (c.video.backend == "lossless") session.configure_lossless_graphics();
#ifdef LRDP_HAVE_FUSE
        std::unique_ptr<DriveMount> drive_mount;
        if (!c.drive_root.empty()) {
            drive::Limits limits; limits.writable = c.drives_writable;
            auto bridge = std::make_shared<drive::Bridge>(limits);
            session.configure_drives(bridge);
            drive_mount = std::make_unique<DriveMount>(bridge, c.drive_root);
            std::cout << "Drives mounted: " << drive_mount->path() << (c.drives_writable ? " (writable)" : " (readonly)") << '\n' << std::flush;
        }
#endif
#ifdef LRDP_HAVE_PRINTING
        if (!c.printer_root.empty()) {
            auto endpoint = std::make_unique<printing::NativeEndpoint>(c.printer_root);
            std::cout << "Printers endpoint: " << endpoint->path() << '\n' << std::flush;
            session.configure_printers(std::move(endpoint));
        }
#endif
        if (c.network_metrics) {
            session.configure_network_metrics();
            stream.observe_transmissions([&session](View packet, std::uint64_t bytes, Clock::time_point now) {
                session.network_transmitted(packet, bytes, now);
            });
        }
        if (c.clipboard_rich) session.configure_rich_clipboard();
        if (!c.clipboard_root.empty()) session.configure_file_clipboard(make_clipboard_file_store(c.clipboard_root, c.clipboard_limits), c.clipboard_limits);
#ifdef LRDP_HAVE_AUDIO
        if (c.audio.playback || c.audio.microphone) {
            auto devices = make_pipewire_audio(c.audio);
            std::cout << "Audio: " << devices->description() << '\n' << std::flush;
            session.configure_audio(std::move(devices));
        }
#endif
        const auto start = Clock::now(); auto next_frame = start, last_receive = start, last_progress = start;
        std::uint64_t previous_written = 0; bool announced = false; std::string graphics_status, clipboard_status;
        auto next_network_log = start;
        auto drain = [&] {
            auto packets = session.drain(); auto media = session.drain_media();
            const bool data_pending = !packets.empty() || !media.empty();
            auto probes = session.poll_network(Clock::now(), stream.queued() == 0, data_pending);
            stream.enqueue(std::move(probes.before_data));
            stream.enqueue(std::move(packets)); stream.enqueue_media(std::move(media));
            stream.enqueue(std::move(probes.after_data));
        };
        while (running && session.phase() != SessionPhase::closed) {
            stream.pump(10);
            while (auto packet = stream.packet()) {
                struct Wipe { Bytes& bytes; ~Wipe() { OPENSSL_cleanse(bytes.data(), bytes.size()); } } wipe{*packet};
                session.receive(*packet);
                // Diagnostic replies must not silently defeat the existing idle policy.
                if (!session.last_packet_was_network()) last_receive = Clock::now();
                drain();
            }
            const auto now = Clock::now();
            if (!announced && session.active()) { std::cout << "Session active\n" << std::flush; announced = true; }
            require(announced || now - start < std::chrono::seconds(20), "session activation deadline exceeded");
            require(now - last_receive < std::chrono::minutes(30), "idle session deadline exceeded");
            if (!stream.queued() || stream.bytes_written() != previous_written) last_progress = now;
            require(now - last_progress < std::chrono::seconds(15), "peer is not draining output"); previous_written = stream.bytes_written();
            const bool due = now >= next_frame;
            session.tick(stream.normal_queued() == 0, due); drain();
            if (due) next_frame = now + std::chrono::microseconds(1000000 / c.fps);
            if (const auto* detector = session.network_metrics(); detector && now >= next_network_log) {
                const auto& m = detector->metrics();
                auto number_or_unknown = [](const auto& value) { return value ? std::to_string(*value) : std::string("unknown"); };
                std::cout << "Network: rtt_us=" << number_or_unknown(m.rtt_us)
                          << " smoothed_rtt_us=" << number_or_unknown(m.smoothed_rtt_us)
                          << " minimum_rtt_us=" << number_or_unknown(m.minimum_rtt_us)
                          << " jitter_us=" << number_or_unknown(m.jitter_us)
                          << " rtt_age_ms=" << number_or_unknown(detector->rtt_age_ms(now))
                          << " peer_receive_kbps=" << number_or_unknown(m.peer_kbps)
                          << " bandwidth_age_ms=" << number_or_unknown(detector->bandwidth_age_ms(now))
                          << " rtt_samples=" << m.rtt_samples << " bandwidth_samples=" << m.bandwidth_samples
                          << " ignored=" << m.ignored_responses << " invalid=" << m.invalid_measurements << " timeouts=" << m.timeouts
                          << " rtt_disabled=" << m.rtt_disabled << " bandwidth_disabled=" << m.bandwidth_disabled
                          << " sequence_exhausted=" << m.sequence_exhausted << '\n' << std::flush;
                next_network_log = now + std::chrono::seconds(5);
            }
            if (clipboard_status != session.clipboard_status()) {
                clipboard_status = session.clipboard_status(); std::cout << "Clipboard: " << clipboard_status << '\n' << std::flush;
            }
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
        if (argc == 2 && std::string_view(argv[1]) == "--capabilities") { std::cout << capabilities() << '\n'; return 0; }
        if (argc == 2 && std::string_view(argv[1]) == "--version") { std::cout << "LRDP " << LRDP_VERSION << '\n'; return 0; }
        const auto c = parse(argc, argv);
        if (c.check_config) { std::cout << preflight(c) << '\n'; return 0; }
        std::signal(SIGPIPE, SIG_IGN); std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        TlsContext context(c.certificate, c.key); Socket listener(bind_listener(c)); VideoFactory video;
#ifdef LRDP_HAVE_FFMPEG
        if (c.video.backend != "raw" && c.video.backend != "lossless") video = ffmpeg_video_factory(c.video);
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
