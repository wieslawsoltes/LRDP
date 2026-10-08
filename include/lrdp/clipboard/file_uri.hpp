#pragma once
#include "lrdp/wire.hpp"

namespace lrdp {
std::vector<std::string> decode_file_uris(std::string_view text, bool gnome = false);
std::string encode_file_uris(const std::vector<std::string>& paths, bool gnome = false);
}
