#include "server_options.hpp"
#include <charconv>
#include <iostream>

namespace lrdp::server {
unsigned number(std::string_view text, unsigned maximum) {
    unsigned value = 0; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0 && value <= maximum, "invalid numeric argument"); return value;
}

void usage() {
    std::cout << "LRDP native Linux remote desktop server\n"
              << "  --capabilities | --version (no configuration or native devices opened)\n"
              << "  --check-config [normal options] (read-only preflight; no listener/desktop)\n"
              << "  --cert FILE --key FILE\n"
              << "  --auth nla --service TERMSRV@host.example.org --allow-principal user@REALM\n"
              << "  [--allow-ntlm] [--listen 127.0.0.1] [--port 3389] [--max-sessions 4]\n"
              << "  [--printers-directory /private/0700/directory]  Native raw printer submission.\n"
              << "  [--drives-directory /private/0700/directory] [--drives-writable]\n"
              << "  [--backend demo|x11|portal|headless] [--display :0] [--fps 30] [--once]\n"
              << "  [--gfx auto|off] [--encoder auto|software|vaapi|nvenc|raw|lossless] [--device /dev/dri/renderD128]\n"
              << "  [--desktop-command /absolute/executable] [--desktop-arg ARG] [--xorg-executable /absolute/Xorg]\n"
              << "  [--network-metrics]  Opt-in continuous RTT and passive receive-throughput diagnostics.\n"
              << "  [--session-broker /private/directory/broker.sock] (headless only; applications survive disconnect)\n"
              << "  [--audio] [--microphone]  Publish per-session virtual PipeWire devices.\n"
              << "  [--clipboard-rich] (HTML and images; x11/headless/portal)\n"
              << "  [--clipboard-files DIRECTORY] [--clipboard-max-mib 256] (x11/headless/portal; private staging)\n"
              << "  --allow-principal can be repeated; matching is exact and case-sensitive.\n"
              << "  Alternatively: --lab-no-auth (loopback-only, no user authentication).\n"
              << "  NLA uses system GSS credentials; desktop access runs as the server's Unix user.\n";
}
Configuration parse(int argc, char** argv) {
    Configuration c;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        auto value = [&]() -> std::string { require(i + 1 < argc, "missing option value"); return argv[++i]; };
        if (option == "--check-config") c.check_config = true;
        else if (option == "--cert") c.certificate = value(); else if (option == "--key") c.key = value();
        else if (option == "--backend") c.backend = value(); else if (option == "--display") c.display = value();
#ifdef LRDP_HAVE_HEADLESS
        else if (option == "--desktop-command") c.headless.command = {value()};
        else if (option == "--desktop-arg") c.headless.command.push_back(value());
        else if (option == "--xorg-executable") c.headless.xorg = value();
#endif
        else if (option == "--session-broker") c.broker_socket = value();
        else if (option == "--listen") c.listen = value(); else if (option == "--port") c.port = number(value(), 65535);
        else if (option == "--fps") c.fps = number(value(), 120);
        else if (option == "--max-sessions") c.max_sessions = number(value(), 64);
        else if (option == "--encoder") c.video.backend = value(); else if (option == "--device") c.video.device = value();
        else if (option == "--gfx") { const auto mode = value(); require(mode == "auto" || mode == "off", "invalid graphics mode"); c.graphics = mode == "auto"; }
        else if (option == "--auth") { require(value() == "nla", "only --auth nla is supported"); c.nla = true; }
        else if (option == "--service") c.service = value();
        else if (option == "--allow-principal") { const auto name = value(); require(!name.empty() && name.size() <= 1024, "invalid principal policy"); c.principals.insert(name); }
        else if (option == "--allow-ntlm") c.allow_ntlm = true;
        else if (option == "--printers-directory") c.printer_root = value();
        else if (option == "--drives-directory") c.drive_root = value();
        else if (option == "--drives-writable") c.drives_writable = true;
        else if (option == "--clipboard-rich") c.clipboard_rich = true;
        else if (option == "--clipboard-files") c.clipboard_root = value();
        else if (option == "--clipboard-max-mib") c.clipboard_limits.bytes = std::uint64_t(number(value(), 1024))*1024*1024;
        else if (option == "--network-metrics") c.network_metrics = true;
        else if (option == "--audio") c.audio.playback = true;
        else if (option == "--microphone") c.audio.microphone = true;
        else if (option == "--lab-no-auth") c.laboratory = true; else if (option == "--once") c.once = true;
        else throw ProtocolError("unknown command-line option: " + option);
    }
    require(c.nla != c.laboratory, "select authenticated --auth nla or explicit loopback --lab-no-auth");
    require(!c.certificate.empty() && !c.key.empty(), "TLS certificate and key are required");
    require(!c.laboratory || c.listen == "127.0.0.1" || c.listen == "::1", "unauthenticated laboratory access must remain on loopback");
    require(!c.nla || (!c.service.empty() && !c.principals.empty()), "NLA requires a service identity and allowed principal");
    require(c.nla || (!c.allow_ntlm && c.service.empty() && c.principals.empty()), "authentication options are invalid in laboratory mode");
    require(c.backend == "demo" || c.backend == "x11" || c.backend == "portal" || c.backend == "headless", "unknown desktop backend");
#ifndef LRDP_HAVE_HEADLESS
    require(c.backend != "headless", "this build has no headless Xorg support");
#endif
#ifndef LRDP_HAVE_GSSAPI
    require(!c.nla, "this build has no system GSSAPI support");
#endif
#ifndef LRDP_HAVE_X11
    require(c.backend != "x11", "this build has no X11 support");
#endif
#ifndef LRDP_HAVE_PORTAL
    require(c.backend != "portal", "this build has no portal/PipeWire support");
#endif
#ifndef LRDP_HAVE_AUDIO
    require(!c.audio.playback && !c.audio.microphone, "this build has no PipeWire audio support");
#endif
    require(c.video.backend == "auto" || c.video.backend == "software" || c.video.backend == "vaapi" ||
            c.video.backend == "nvenc" || c.video.backend == "raw" || c.video.backend == "lossless", "invalid video encoder");
#ifndef LRDP_HAVE_FFMPEG
    require(c.video.backend == "auto" || c.video.backend == "raw" || c.video.backend == "lossless", "this build has no FFmpeg support");
#endif
    require(!c.clipboard_rich || c.backend == "x11" || c.backend == "headless" || c.backend == "portal", "rich clipboard requires a native desktop backend");
    require(c.clipboard_root.empty() || c.backend == "x11" || c.backend == "headless" || c.backend == "portal", "file clipboard requires a native desktop backend");
    require(c.broker_socket.empty() || c.backend == "headless", "persistent sessions require --backend headless");
    require(!c.drives_writable || !c.drive_root.empty(), "--drives-writable requires --drives-directory");
#ifndef LRDP_HAVE_FUSE
    require(c.drive_root.empty(), "this build has no libfuse3 drive mounting support");
#endif
#ifndef LRDP_HAVE_PRINTING
    require(c.printer_root.empty(), "this build has no native printer submission support");
#endif
    require(c.video.backend != "lossless" || c.graphics, "lossless graphics requires --gfx auto");
    c.video.fps = c.fps; return c;
}
} // namespace lrdp::server
