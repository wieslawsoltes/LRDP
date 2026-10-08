#pragma once
#include "lrdp/clipboard/file_store.hpp"

namespace lrdp {
// Explicit, existing, owner-controlled root. Export resolution rejects symlinks
// and non-regular objects; received files remain mode 0600 in private staging.
std::shared_ptr<ClipboardFileStore> make_clipboard_file_store(const std::string& root,
                                                            FileClipboardLimits limits = {});
}
