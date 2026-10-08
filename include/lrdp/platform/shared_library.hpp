#pragma once
#include "lrdp/wire.hpp"
#include <dlfcn.h>
#include <utility>

namespace lrdp {
// Loading documented platform libraries in the server is not application injection.
class SharedLibrary final {
    void* handle_ = nullptr;
public:
    explicit SharedLibrary(const char* soname) : handle_(dlopen(soname, RTLD_NOW | RTLD_LOCAL)) {
        require(handle_ != nullptr, std::string("required platform library is unavailable: ") + soname);
    }
    ~SharedLibrary() { if (handle_) dlclose(handle_); }
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    template<class Function> Function symbol(const char* name) const {
        auto* value = dlsym(handle_, name);
        require(value != nullptr, std::string("platform entry point is unavailable: ") + name);
        return reinterpret_cast<Function>(value); // POSIX dlsym function-pointer conversion.
    }
};
} // namespace lrdp
