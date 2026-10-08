#pragma once
#include "lrdp/desktop.hpp"
#include "lrdp/platform/headless_server.hpp"
namespace lrdp {
std::unique_ptr<Desktop> make_headless_desktop(const HeadlessOptions& options = {});
}
