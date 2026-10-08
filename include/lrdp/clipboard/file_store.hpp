#pragma once
#include "lrdp/wire.hpp"
#include <memory>
#include <optional>

namespace lrdp {
struct ClipboardFile {
    std::string name; // Relative slash-separated UTF-8 path, never a source path.
    bool directory = false;
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> modified; // Windows FILETIME, if supplied.
};
struct FileClipboardLimits {
    std::size_t entries = 128;
    std::uint64_t bytes = 256ULL * 1024 * 1024;
    std::uint32_t chunk = 64 * 1024;
    unsigned window = 4;
};
std::string validate_clipboard_path(std::string path);
std::uint64_t validate_clipboard_files(const std::vector<ClipboardFile>& files,
                                     FileClipboardLimits limits = {}, bool sizes_required = false);
Bytes encode_clipboard_files(const std::vector<ClipboardFile>& files, FileClipboardLimits limits = {});
std::vector<ClipboardFile> decode_clipboard_files(View payload, FileClipboardLimits limits = {});

class ClipboardFileSource {
public:
    virtual ~ClipboardFileSource() = default;
    virtual const std::vector<ClipboardFile>& files() const = 0;
    virtual Bytes read(std::size_t index, std::uint64_t offset, std::uint32_t count) const = 0;
};
class ClipboardFileSink {
public:
    virtual ~ClipboardFileSink() = default; // Aborts unpublished data.
    virtual void write(std::size_t index, std::uint64_t offset, View bytes) = 0;
    virtual std::vector<std::string> finish() = 0; // Publish only a complete transfer.
};
class ClipboardFileStore {
public:
    virtual ~ClipboardFileStore() = default;
    virtual std::shared_ptr<const ClipboardFileSource> offer(const std::vector<std::string>& paths) = 0;
    virtual std::unique_ptr<ClipboardFileSink> receive(const std::vector<ClipboardFile>& files) = 0;
};
} // namespace lrdp
