#include "lrdp/session.hpp"
#include "lrdp/transport.hpp"
#include <arpa/inet.h>
#include <charconv>
#include <csignal>
#include <iostream>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
unsigned number(std::string_view text, unsigned maximum) {
    unsigned value = 0; const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    lrdp::require(result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0 && value <= maximum, "invalid numeric argument"); return value;
}
}
int main(int argc, char** argv) {
    using namespace lrdp;
    try {
        std::string cert, key, backend = "demo", display;
        unsigned port = 3389, fps = 30; bool lab = false, once = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string { require(i + 1 < argc, "missing option value"); return argv[++i]; };
            if (arg == "--cert") cert = value(); else if (arg == "--key") key = value();
            else if (arg == "--port") port = number(value(), 65535); else if (arg == "--fps") fps = number(value(), 120);
            else if (arg == "--backend") backend = value(); else if (arg == "--display") display = value();
            else if (arg == "--lab-no-auth") lab = true; else if (arg == "--once") once = true;
            else if (arg == "--help") {
                std::cout << "LRDP experimental native server\n"
                          << "Usage: lrdpd --lab-no-auth --cert certificate.pem --key private-key.pem\n"
                          << "              [--port 3389] [--fps 30] [--backend demo|x11] [--display :0] [--once]\n"
                          << "Loopback-only TLS laboratory profile. NLA and production authentication are not implemented.\n"; return 0;
            } else throw ProtocolError("unknown command-line argument");
        }
        require(lab, "No authentication provider is enabled. Explicit --lab-no-auth is required for the loopback laboratory profile.");
        require(!cert.empty() && !key.empty(), "TLS certificate and key are required");
        require(backend == "demo" || backend == "x11", "unknown desktop backend");
#ifndef LRDP_HAVE_X11
        require(backend != "x11", "this binary was built without X11 support");
#endif
        std::signal(SIGPIPE, SIG_IGN); std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        TlsContext tls(cert, key);
        Socket listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); require(listener.get() >= 0, "cannot create listener");
        int yes = 1; setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(std::uint16_t(port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && listen(listener.get(), 8) == 0, "cannot bind loopback listener");
        std::cout << "LRDP listening on 127.0.0.1:" << port << " (TLS laboratory profile, backend=" << backend << ")\n" << std::flush;
        std::cerr << "WARNING: This profile has no user authentication; do not forward its port to untrusted peers.\n";
        do {
            pollfd incoming{listener.get(), POLLIN, 0};
            const auto rc = poll(&incoming, 1, 250); if (rc <= 0) continue;
            Socket peer(accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)); if (peer.get() < 0) continue;
            setsockopt(peer.get(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            try {
                const auto negotiation = parse_negotiation(read_negotiation(peer.get()));
                if (!(negotiation.protocols & 1) || (negotiation.flags & 3)) {
                    write_raw(peer.get(), negotiation_reply(1, true)); throw ProtocolError("TLS laboratory protocol selection rejected");
                }
                write_raw(peer.get(), negotiation_reply(1)); TlsStream stream(peer.get(), tls);
                std::unique_ptr<Desktop> desktop;
#ifdef LRDP_HAVE_X11
                if (backend == "x11") desktop = make_x11_desktop(display);
#endif
                if (!desktop) desktop = make_demo_desktop();
                Session session(std::move(desktop), negotiation.protocols);
                using Clock = std::chrono::steady_clock;
                const auto start = Clock::now(); auto next_frame = start, last_receive = start, last_progress = start;
                auto previous_queued = stream.queued(); bool announced = false;
                while (running && session.phase() != SessionPhase::closed) {
                    stream.pump(10);
                    while (auto packet = stream.packet()) {
                        // Client Info may contain passwords even in the laboratory profile.
                        struct Wipe { Bytes& bytes; ~Wipe() { OPENSSL_cleanse(bytes.data(), bytes.size()); } } wipe{*packet};
                        session.receive(*packet);
                        last_receive = Clock::now(); stream.enqueue(session.drain());
                    }
                    const auto now = Clock::now();
                    if (!announced && session.active()) { std::cout << "Session active\n" << std::flush; announced = true; }
                    require(announced || now - start < std::chrono::seconds(20), "session activation deadline exceeded");
                    require(now - last_receive < std::chrono::minutes(30), "idle session deadline exceeded");
                    if (!stream.queued() || stream.queued() < previous_queued) last_progress = now;
                    require(now - last_progress < std::chrono::seconds(15), "client is not draining its output queue"); previous_queued = stream.queued();
                    if (now >= next_frame) {
                        session.tick(stream.queued() == 0); stream.enqueue(session.drain());
                        next_frame = now + std::chrono::microseconds(1000000 / fps);
                    }
                }
            } catch (const std::exception& error) { std::cerr << "Session ended: " << error.what() << '\n'; }
            if (once) break;
        } while (running);
        return 0;
    } catch (const std::exception& error) { std::cerr << "LRDP: " << error.what() << '\n'; return 1; }
}
