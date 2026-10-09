#pragma once
#include "lrdp/printing/native.hpp"
#include <sys/socket.h>

namespace lrdp::printing::local {
constexpr unsigned list = 1, submit = 2, response = 0x80000000U;
constexpr std::size_t packet_limit = 32768;
struct Message { Bytes bytes; std::vector<UniqueFd> descriptors; };
// nullopt = EAGAIN; an empty message denotes orderly shutdown.
std::optional<Message> receive(int fd);
bool send(int fd, View packet, int descriptor = -1);
void same_user(int fd);
UniqueFd directory(const std::string& absolute, bool private_leaf);
std::string pinned(int directory, std::string_view leaf);
UniqueFd connect(const std::string& socket_path);
void wait(int fd, short events, drive::Clock::time_point deadline);
Bytes request(unsigned command, View payload = {});
Bytes reply(unsigned command, unsigned status, View payload = {});
Bytes exchange(int fd, unsigned command, View payload, int descriptor, std::chrono::seconds timeout);
void text(Writer& out, std::string_view value);
std::string text(Reader& in);
} // namespace lrdp::printing::local
