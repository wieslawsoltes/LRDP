#pragma once
#include "lrdp/desktop.hpp"
#include "lrdp/reconnect.hpp"
namespace lrdp {
// Does not contact the broker or open X11 until Client Info has been validated.
std::unique_ptr<Desktop> make_persistent_desktop(std::string broker_socket, std::string authenticated_principal);
}
