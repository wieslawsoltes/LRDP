#pragma once
#include "lrdp/video.hpp"
#include "lrdp/clipboard/file_store.hpp"
#include "lrdp/audio/devices.hpp"
#ifdef LRDP_HAVE_HEADLESS
#include "lrdp/platform/headless_server.hpp"
#endif
#include <set>

namespace lrdp::server {
struct Configuration {
    std::string certificate, key, backend = "demo", display, listen = "127.0.0.1", service;
    std::set<std::string> principals;
    unsigned port = 3389, fps = 30, max_sessions = 4;
    bool laboratory = false, nla = false, allow_ntlm = false, once = false, graphics = true;
    VideoOptions video;
    AudioOptions audio;
    std::string clipboard_root, drive_root, broker_socket, printer_root;
    bool drives_writable = false, network_metrics = false, check_config = false;
    bool clipboard_rich = false;
    FileClipboardLimits clipboard_limits;
#ifdef LRDP_HAVE_HEADLESS
    HeadlessOptions headless;
#endif
};
void usage();
Configuration parse(int argc, char** argv);
std::string capabilities();
std::string preflight(const Configuration& options);
} // namespace lrdp::server
