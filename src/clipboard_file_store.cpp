#include "lrdp/platform/clipboard_file_store.hpp"
#include "lrdp/platform/unique_fd.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <filesystem>
#include <map>
#include <sys/random.h>
#include <sys/stat.h>

namespace lrdp {
namespace {
struct DirectoryDelete { void operator()(DIR* p) const { if (p) closedir(p); } };
using Directory = std::unique_ptr<DIR, DirectoryDelete>;
UniqueFd duplicate(int fd) {
    UniqueFd result(fcntl(fd, F_DUPFD_CLOEXEC, 3)); require(bool(result), "cannot duplicate clipboard directory"); return result;
}
struct stat attributes(int fd) {
    struct stat value{}; require(fstat(fd, &value) == 0, "cannot inspect clipboard file"); return value;
}
bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
UniqueFd directory_at(int parent, const std::string& name) {
    UniqueFd value(openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    require(bool(value), "clipboard directory is unavailable or symbolic"); return value;
}
UniqueFd parent_of(int root, const std::string& path) {
    auto parent = duplicate(root);
    for (std::size_t start = 0;;) {
        const auto end = path.find('/', start); if (end == std::string::npos) return parent;
        parent = directory_at(parent.get(), path.substr(start, end-start)); start = end + 1;
    }
}
std::string leaf(const std::string& name) { return name.substr(name.find_last_of('/') + 1); }
std::vector<std::string> entries(int fd, std::size_t maximum) {
    auto copy = duplicate(fd); Directory dir(fdopendir(copy.release())); require(bool(dir), "cannot enumerate clipboard directory");
    // dup shares directory offset: callers enumerate a directory only once.
    std::vector<std::string> result; errno = 0;
    while (auto* entry = readdir(dir.get())) {
        const std::string name = entry->d_name; if (name == "." || name == "..") continue;
        require(result.size() < maximum, "clipboard directory has too many entries"); result.push_back(name);
    }
    require(errno == 0, "clipboard directory enumeration failed"); std::sort(result.begin(), result.end()); return result;
}
void remove_private(int parent, const std::string& name, unsigned depth = 0) noexcept {
    if (depth > 32) return;
    UniqueFd fd(openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    if (!fd) { (void)unlinkat(parent, name.c_str(), 0); return; }
    UniqueFd copy(fcntl(fd.get(), F_DUPFD_CLOEXEC, 3));
    if (!copy) return;
    DIR* raw = fdopendir(copy.get()); if (!raw) return;
    (void)copy.release(); Directory dir(raw); unsigned count = 0;
    while (auto* e = readdir(dir.get())) {
        if (++count > 100000) return;
        if (std::string_view(e->d_name) != "." && std::string_view(e->d_name) != "..") remove_private(fd.get(), e->d_name, depth+1);
    }
    (void)unlinkat(parent, name.c_str(), AT_REMOVEDIR);
}
struct Root {
    UniqueFd fd;
    std::string path;
    FileClipboardLimits limits;
    std::uint64_t allocated = 0;
    unsigned transfers = 0;
    struct Published { std::string name; dev_t device; ino_t inode; };
    std::vector<Published> published;
    ~Root() {
        for (const auto& item : published) {
            struct stat current{};
            if (fstatat(fd.get(), item.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
                current.st_dev == item.device && current.st_ino == item.inode && S_ISDIR(current.st_mode))
                remove_private(fd.get(), item.name);
        }
    }
};
class FileSource final : public ClipboardFileSource {
    std::vector<ClipboardFile> files_;
    struct File { UniqueFd fd; struct stat original{}; };
    std::vector<File> handles_;
    std::uint64_t total_ = 0;
    FileClipboardLimits limits_;
    void append(int parent, const std::string& native, const std::string& relative) {
        require(files_.size() < limits_.entries && validate_clipboard_path(relative) == relative, "invalid exported clipboard file path");
        // Pin the inode without opening a device/FIFO. Reopen only after fstat
        // confirms a regular file or directory, so path swaps cannot escape.
        UniqueFd anchor(openat(parent, native.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
        require(bool(anchor), "cannot pin clipboard source"); const auto before = attributes(anchor.get());
        require(S_ISDIR(before.st_mode) || S_ISREG(before.st_mode), "only regular files and directories may be copied");
        const auto pinned = "/proc/self/fd/" + std::to_string(anchor.get());
        UniqueFd fd(open(pinned.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (S_ISDIR(before.st_mode) ? O_DIRECTORY : 0)));
        require(bool(fd), "cannot open pinned clipboard source"); const auto st = attributes(fd.get());
        require(same(before, st), "clipboard source changed during open");
        const bool directory = S_ISDIR(st.st_mode);
        require(directory || (st.st_size >= 0 && std::uint64_t(st.st_size) <= limits_.bytes - total_), "clipboard source exceeds byte policy");
        ClipboardFile file{relative, directory, directory ? 0 : std::uint64_t(st.st_size), {}};
        if (st.st_mtim.tv_sec >= 0 && std::uint64_t(st.st_mtim.tv_sec) < 1000000000000ULL)
            file.modified = (std::uint64_t(st.st_mtim.tv_sec) + 11644473600ULL)*10000000ULL + std::uint64_t(st.st_mtim.tv_nsec)/100;
        files_.push_back(std::move(file));
        if (directory) {
            handles_.push_back({UniqueFd{}, st});
            for (const auto& name : entries(fd.get(), limits_.entries - files_.size())) append(fd.get(), name, relative + '/' + name);
            require(same(st, attributes(fd.get())), "clipboard directory changed during enumeration");
        } else { total_ += std::uint64_t(st.st_size); handles_.push_back({std::move(fd), st}); }
    }
public:
    FileSource(const Root& root, const std::vector<std::string>& paths) : limits_(root.limits) {
        require(!paths.empty() && paths.size() <= limits_.entries, "invalid native clipboard file count");
        for (const auto& path : paths) {
            require(path.size() <= 4096 && path.starts_with(root.path + '/'), "clipboard source is outside the configured root");
            const auto relative = path.substr(root.path.size()+1);
            require(validate_clipboard_path(relative) == relative, "noncanonical clipboard source path");
            auto parent = parent_of(root.fd.get(), relative); append(parent.get(), leaf(relative), leaf(relative));
        }
        (void)validate_clipboard_files(files_, limits_, true);
    }
    const std::vector<ClipboardFile>& files() const override { return files_; }
    Bytes read(std::size_t index, std::uint64_t offset, std::uint32_t count) const override {
        require(index < files_.size() && !files_[index].directory && offset <= *files_[index].size &&
                count <= *files_[index].size - offset && count <= 1024*1024, "clipboard source read exceeds bounds");
        const auto& handle = handles_[index]; require(same(handle.original, attributes(handle.fd.get())), "clipboard source has changed");
        Bytes bytes(count); std::size_t read = 0;
        while (read < count) {
            const auto n = pread(handle.fd.get(), bytes.data()+read, count-read, off_t(offset+read));
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "clipboard source read failed"); read += std::size_t(n);
        }
        require(same(handle.original, attributes(handle.fd.get())), "clipboard source changed during read"); return bytes;
    }
};
class FileSink final : public ClipboardFileSink {
    std::shared_ptr<Root> root_;
    std::vector<ClipboardFile> files_;
    std::vector<UniqueFd> handles_;
    std::vector<std::map<std::uint64_t, std::uint64_t>> ranges_;
    UniqueFd directory_;
    std::string name_;
    std::uint64_t bytes_ = 0;
    bool reserved_ = false, finished_ = false;
public:
    FileSink(std::shared_ptr<Root> root, std::vector<ClipboardFile> files) : root_(std::move(root)), files_(std::move(files)) {}
    ~FileSink() override {
        if (!finished_) {
            handles_.clear(); directory_.reset();
            if (!name_.empty()) remove_private(root_->fd.get(), name_);
            if (reserved_) { root_->allocated -= bytes_; --root_->transfers; }
        }
    }
    void initialize() {
        bytes_ = validate_clipboard_files(files_, root_->limits, true);
        require(root_->transfers < 16 && bytes_ <= root_->limits.bytes - root_->allocated, "session clipboard staging quota exceeded");
        root_->allocated += bytes_; ++root_->transfers; reserved_ = true;
        std::array<unsigned char,16> random{};
        for (std::size_t n = 0; n < random.size();) {
            const auto count = getrandom(random.data()+n, random.size()-n, 0);
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "clipboard staging entropy unavailable"); n += std::size_t(count);
        }
        std::string candidate = ".lrdp-clipboard-"; constexpr char hex[] = "0123456789abcdef";
        for (const auto b : random) { candidate += hex[b >> 4]; candidate += hex[b & 15]; }
        require(mkdirat(root_->fd.get(), candidate.c_str(), 0700) == 0, "cannot create private clipboard staging"); name_ = candidate;
        directory_ = directory_at(root_->fd.get(), name_);
        handles_.resize(files_.size()); ranges_.resize(files_.size());
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < files_.size(); ++i) { files_[i].name = validate_clipboard_path(files_[i].name); order.push_back(i); }
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            return std::count(files_[a].name.begin(), files_[a].name.end(), '/') < std::count(files_[b].name.begin(), files_[b].name.end(), '/');
        });
        for (const auto i : order) {
            const auto& file = files_[i]; auto parent = parent_of(directory_.get(), file.name); const auto basename = leaf(file.name);
            if (file.directory) require(mkdirat(parent.get(), basename.c_str(), 0700) == 0, "cannot create clipboard directory");
            else {
                handles_[i].reset(openat(parent.get(), basename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
                require(bool(handles_[i]) && ftruncate(handles_[i].get(), off_t(*file.size)) == 0, "cannot create clipboard file");
            }
        }
    }
    void write(std::size_t index, std::uint64_t offset, View bytes) override {
        require(!finished_ && index < files_.size() && handles_[index] && offset <= *files_[index].size &&
                bytes.size() <= *files_[index].size - offset && bytes.size() <= 65536, "clipboard write exceeds staging bounds");
        if (bytes.empty()) return;
        const auto end = offset + bytes.size(); auto& ranges = ranges_[index]; auto next = ranges.lower_bound(offset);
        require((next == ranges.end() || next->first >= end) && (next == ranges.begin() || std::prev(next)->second <= offset),
                "overlapping clipboard writes");
        require(ranges.size() < 1024, "clipboard receive fragmentation quota exceeded");
        for (std::size_t written = 0; written < bytes.size();) {
            const auto n = pwrite(handles_[index].get(), bytes.data()+written, bytes.size()-written, off_t(offset+written));
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "clipboard disk write failed"); written += std::size_t(n);
        }
        std::uint64_t begin = offset, finish = end;
        if (next != ranges.end() && next->first == end) { finish = next->second; next = ranges.erase(next); }
        if (next != ranges.begin() && std::prev(next)->second == offset) { const auto previous = std::prev(next); begin = previous->first; ranges.erase(previous); }
        ranges.emplace(begin, finish);
    }
    std::vector<std::string> finish() override {
        require(!finished_ && bool(directory_), "clipboard sink already completed or uninitialized");
        for (std::size_t i = 0; i < files_.size(); ++i) if (!files_[i].directory) {
            const auto& ranges = ranges_[i]; const auto size = *files_[i].size;
            require(size == 0 || (ranges.size() == 1 && ranges.begin()->first == 0 && ranges.begin()->second == size), "incomplete clipboard file cannot be published");
            require(fsync(handles_[i].get()) == 0, "cannot finalize clipboard data");
        }
        const auto expected = attributes(directory_.get()); struct stat actual{};
        require(fstatat(root_->fd.get(), name_.c_str(), &actual, AT_SYMLINK_NOFOLLOW) == 0 &&
                expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino, "clipboard staging directory changed");
        std::vector<std::string> paths;
        for (const auto& file : files_) if (file.name.find('/') == std::string::npos) paths.push_back(root_->path + '/' + name_ + '/' + file.name);
        root_->published.push_back({name_, actual.st_dev, actual.st_ino}); finished_ = true; return paths;
    }
};
class FileStore final : public ClipboardFileStore {
    std::shared_ptr<Root> root_ = std::make_shared<Root>();
public:
    FileStore(const std::string& path, FileClipboardLimits limits) {
        (void)validate_clipboard_files({{"policy",false,0,{}}}, limits);
        root_->limits = limits; root_->path = std::filesystem::absolute(path).lexically_normal().string();
        while (root_->path.size() > 1 && root_->path.back() == '/') root_->path.pop_back();
        require(root_->path != "/" && root_->path.size() < 3072 && root_->path.find('\0') == std::string::npos, "clipboard root must be a dedicated directory");
        root_->fd.reset(open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY)); require(bool(root_->fd), "cannot open filesystem root");
        std::size_t start = 1;
        while (start < root_->path.size()) {
            const auto end = root_->path.find('/', start);
            root_->fd = directory_at(root_->fd.get(), root_->path.substr(start, end == std::string::npos ? end : end-start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        const auto st = attributes(root_->fd.get());
        require(st.st_uid == geteuid() && !(st.st_mode & 0022), "clipboard root must be owned by this user and not writable by other users");
    }
    std::shared_ptr<const ClipboardFileSource> offer(const std::vector<std::string>& paths) override { return std::make_shared<FileSource>(*root_, paths); }
    std::unique_ptr<ClipboardFileSink> receive(const std::vector<ClipboardFile>& files) override {
        auto sink = std::make_unique<FileSink>(root_, files); sink->initialize(); return sink;
    }
};
}
std::shared_ptr<ClipboardFileStore> make_clipboard_file_store(const std::string& root, FileClipboardLimits limits) {
    return std::make_shared<FileStore>(root, limits);
}
} // namespace lrdp
