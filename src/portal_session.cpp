#include "lrdp/platform/portal_session.hpp"
#include "lrdp/platform/mime_clipboard.hpp"
#include "lrdp/clipboard/file_uri.hpp"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <poll.h>
#include <thread>

namespace lrdp {
namespace {
constexpr auto service = "org.freedesktop.portal.Desktop";
constexpr auto object = "/org/freedesktop/portal/desktop";
constexpr auto remote = "org.freedesktop.portal.RemoteDesktop";
constexpr auto screen = "org.freedesktop.portal.ScreenCast";
constexpr auto clipboard = "org.freedesktop.portal.Clipboard";
constexpr auto session_interface = "org.freedesktop.portal.Session";
constexpr auto utf8_mime = "text/plain;charset=utf-8";
constexpr std::size_t clipboard_quota = 1024 * 1024;
using Clock = std::chrono::steady_clock;
struct VariantDelete { void operator()(GVariant* p) const { if (p) g_variant_unref(p); } };
using Variant = std::unique_ptr<GVariant, VariantDelete>;
struct ObjectDelete { template<class T> void operator()(T* p) const { if (p) g_object_unref(p); } };
struct Error {
    GError* value = nullptr;
    ~Error() { if (value) g_error_free(value); }
    void check(const void* result, const char* operation) const {
        require(result != nullptr, std::string(operation) + ": " + (value ? value->message : "missing D-Bus result"));
    }
};
struct Context {
    GMainContext* value = g_main_context_new();
    const std::thread::id owner = std::this_thread::get_id();
    Context() { require(value != nullptr, "cannot create portal event context"); g_main_context_push_thread_default(value); }
    ~Context() { g_main_context_pop_thread_default(value); g_main_context_unref(value); }
    void check() const { require(owner == std::this_thread::get_id(), "portal used from the wrong thread"); }
    void iterate() { check(); for (unsigned n = 0; n < 128 && g_main_context_pending(value); ++n) g_main_context_iteration(value, FALSE); }
};
GVariant* options(std::initializer_list<std::pair<const char*, GVariant*>> fields = {}) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    for (const auto& [key, value] : fields) g_variant_builder_add(&b, "{sv}", key, value);
    return g_variant_builder_end(&b);
}
std::string token() {
    gchar* value = g_uuid_string_random(); require(value != nullptr, "cannot allocate portal request token");
    std::string result = "lrdp_" + std::string(value); g_free(value);
    std::replace(result.begin(), result.end(), '-', '_'); return result;
}
struct Subscription {
    GDBusConnection* connection; guint id;
    ~Subscription() { if (id) g_dbus_connection_signal_unsubscribe(connection, id); }
};
}
struct PortalSession::Impl : MimeClipboardTransport {
    Context context;
    std::unique_ptr<GDBusConnection, ObjectDelete> bus;
    std::unique_ptr<GCancellable, ObjectDelete> cancel{g_cancellable_new()};
    std::string owner, session;
    PortalStream selected;
    std::vector<guint> subscriptions;
    struct AsyncState { unsigned pending = 0; bool failed = false, stopping = false; };
    std::shared_ptr<AsyncState> async = std::make_shared<AsyncState>();
    bool closed = false, clipboard_enabled = false;
    MimeClipboard mime{*this};
    bool rich_enabled = false, files_enabled = false;
    std::optional<std::string> ready_text;
    std::optional<RichClipboard> ready_rich;
    std::optional<std::vector<std::string>> ready_files;
    Impl() { configure_mimes(); }
    void configure_mimes() {
        std::vector<std::string> formats{utf8_mime, "text/plain"};
        if (files_enabled) {
            formats.insert(formats.begin(), {"x-special/gnome-copied-files", "text/uri-list"});
        }
        if (rich_enabled) {
            formats.push_back("text/html");
#ifdef LRDP_HAVE_PNG
            formats.push_back("image/png");
#endif
            formats.push_back("image/bmp"); formats.push_back("image/x-bmp");
        }
        clear_ready(); mime.supported(std::move(formats));
    }
    void clear_ready() { ready_text.reset(); ready_rich.reset(); ready_files.reset(); }
    static MimeBytes bytes(std::string_view text) { return std::make_shared<const Bytes>(text.begin(), text.end()); }

    ~Impl() {
        context.check(); async->stopping = true;
        if (bus) {
            for (auto id : subscriptions) g_dbus_connection_signal_unsubscribe(bus.get(), id);
            subscriptions.clear(); mime.clear(); clear_ready();
            if (!session.empty() && !g_dbus_connection_is_closed(bus.get())) {
                Error error;
                Variant ignored(g_dbus_connection_call_sync(bus.get(), owner.c_str(), session.c_str(), session_interface,
                    "Close", nullptr, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error.value));
            }
            g_cancellable_cancel(cancel.get());
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (async->pending && Clock::now() < deadline) { context.iterate(); ::poll(nullptr, 0, 1); }
            Error error; (void)g_dbus_connection_close_sync(bus.get(), nullptr, &error.value); context.iterate();
        }
    }
    void check() const {
        context.check();
        require(bus && !closed && !async->failed && !g_dbus_connection_is_closed(bus.get()), "portal session was revoked or a native operation failed");
    }
    Variant call(const char* iface, const char* method, GVariant* parameters, const GVariantType* reply = G_VARIANT_TYPE_UNIT,
                 const char* path = object, const char* destination = nullptr) {
        Error error;
        Variant result(g_dbus_connection_call_sync(bus.get(), destination ? destination : owner.c_str(), path, iface, method,
            parameters, reply, G_DBUS_CALL_FLAGS_NONE, 5000, cancel.get(), &error.value));
        error.check(result.get(), method); return result;
    }
    std::uint32_t property(const char* iface, const char* name, bool optional = false, const char* destination = nullptr) {
        Error error;
        Variant result(g_dbus_connection_call_sync(bus.get(), destination ? destination : owner.c_str(), object,
            "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 5000, cancel.get(), &error.value));
        if (!result && optional) return 0;
        error.check(result.get(), name);
        Variant boxed(g_variant_get_child_value(result.get(), 0)); Variant value(g_variant_get_variant(boxed.get()));
        require(g_variant_is_of_type(value.get(), G_VARIANT_TYPE_UINT32), "portal property has wrong type"); return g_variant_get_uint32(value.get());
    }
    template<class Build> Variant request(const char* iface, const char* method, Build build) {
        const auto id = token(); std::string sender = g_dbus_connection_get_unique_name(bus.get());
        require(!sender.empty() && sender[0] == ':', "portal connection has no unique bus name"); sender.erase(0, 1);
        std::replace(sender.begin(), sender.end(), '.', '_');
        const auto path = std::string(object) + "/request/" + sender + '/' + id;
        struct Reply { bool received = false; guint code = 2; Variant result; } reply;
        Subscription subscription{bus.get(), g_dbus_connection_signal_subscribe(bus.get(), owner.c_str(),
            "org.freedesktop.portal.Request", "Response", path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
                auto& r = *static_cast<Reply*>(data);
                if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")) || r.received) return;
                GVariant* result = nullptr; g_variant_get(parameters, "(u@a{sv})", &r.code, &result);
                r.result.reset(result); r.received = true;
            }, &reply, nullptr)};
        const auto result = call(iface, method, build(id), G_VARIANT_TYPE("(o)"));
        const gchar* returned = nullptr; g_variant_get(result.get(), "(&o)", &returned);
        require(returned == path, "portal returned an unexpected request handle");
        const auto deadline = Clock::now() + std::chrono::seconds(90);
        while (!reply.received) {
            context.iterate(); check();
            if (Clock::now() >= deadline) {
                Error error; Variant ignored(g_dbus_connection_call_sync(bus.get(), owner.c_str(), path.c_str(),
                    "org.freedesktop.portal.Request", "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error.value));
                throw ProtocolError("portal consent request timed out");
            }
            if (!reply.received) ::poll(nullptr, 0, 2);
        }
        require(reply.code == 0, "portal request was cancelled or denied"); return std::move(reply.result);
    }
    void enqueue(const char* iface, const char* method, GVariant* parameters) {
        check(); require(async->pending < 256, "portal asynchronous operation queue is full");
        auto* retained = new std::shared_ptr<AsyncState>(async); ++async->pending;
        g_dbus_connection_call(bus.get(), owner.c_str(), object, iface, method, parameters, G_VARIANT_TYPE_UNIT,
            G_DBUS_CALL_FLAGS_NONE, 3000, cancel.get(),
            [](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::shared_ptr<AsyncState>> reference(static_cast<std::shared_ptr<AsyncState>*>(data));
                auto& state = **reference; Error error;
                Variant reply(g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error.value));
                if (!reply && !state.stopping) state.failed = true;
                --state.pending;
            }, retained);
    }
    UniqueFd descriptor(const char* iface, const char* method, GVariant* parameters) {
        check(); Error error; GUnixFDList* raw_fds = nullptr;
        Variant reply(g_dbus_connection_call_with_unix_fd_list_sync(bus.get(), owner.c_str(), object, iface, method,
            parameters, G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &raw_fds, cancel.get(), &error.value));
        std::unique_ptr<GUnixFDList, ObjectDelete> fds(raw_fds); error.check(reply.get(), method);
        gint index = -1; g_variant_get(reply.get(), "(h)", &index);
        require(fds && index >= 0 && index < g_unix_fd_list_get_length(fds.get()), "portal returned an invalid descriptor index");
        UniqueFd fd(g_unix_fd_list_get(fds.get(), index, &error.value)); require(bool(fd), "cannot duplicate portal descriptor");
        require(fcntl(fd.get(), F_SETFD, FD_CLOEXEC) == 0, "cannot protect portal descriptor across exec"); return fd;
    }
    void owner_changed(GVariant* parameters) {
        require(g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})")), "invalid clipboard owner signal");
        const gchar* path = nullptr; GVariant* raw = nullptr; g_variant_get(parameters, "(&o@a{sv})", &path, &raw); Variant fields(raw);
        if (path != session) return;
        clear_ready();
        gboolean own = FALSE; g_variant_lookup(fields.get(), "session_is_owner", "b", &own);
        Variant formats(g_variant_lookup_value(fields.get(), "mime_types", G_VARIANT_TYPE_STRING_ARRAY));
        std::vector<std::string> names;
        if (formats) {
            require(g_variant_n_children(formats.get()) <= 256, "too many portal clipboard formats");
            GVariantIter iter; g_variant_iter_init(&iter, formats.get()); const gchar* name = nullptr;
            while (g_variant_iter_next(&iter, "&s", &name)) names.emplace_back(name);
        }
        mime.owner_changed(std::move(names), own);
    }
    void transfer(GVariant* parameters) {
        require(g_variant_is_of_type(parameters, G_VARIANT_TYPE("(osu)")), "invalid clipboard transfer signal");
        const gchar* path = nullptr; const gchar* type = nullptr; guint serial = 0;
        g_variant_get(parameters, "(&o&su)", &path, &type, &serial); if (path != session) return;
        mime.transfer(serial, type);
    }

    void start() {
        Error error; gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel.get(), &error.value);
        error.check(address, "resolve session bus"); std::unique_ptr<gchar, decltype(&g_free)> location(address, g_free);
        bus.reset(g_dbus_connection_new_for_address_sync(address,
            GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, cancel.get(), &error.value)); error.check(bus.get(), "connect portal session bus");
        g_dbus_connection_set_exit_on_close(bus.get(), FALSE);
        const auto version = property(remote, "version", false, service);
        require(version >= 1, "RemoteDesktop portal unavailable");
        const auto name = call("org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", service), G_VARIANT_TYPE("(s)"),
                               "/org/freedesktop/DBus", "org.freedesktop.DBus");
        const gchar* unique = nullptr; g_variant_get(name.get(), "(&s)", &unique); owner = unique;
        subscriptions.push_back(g_dbus_connection_signal_subscribe(bus.get(), "org.freedesktop.DBus", "org.freedesktop.DBus",
            "NameOwnerChanged", "/org/freedesktop/DBus", service, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
                static_cast<Impl*>(data)->closed = true;
            }, this, nullptr));
        require((property(remote, "AvailableDeviceTypes") & 3) == 3, "portal does not provide keyboard and pointer control");
        require(property(screen, "AvailableSourceTypes") & 1, "portal does not provide monitor capture");
        const auto created = request(remote, "CreateSession", [&](const std::string& id) {
            const auto sid = token(); return g_variant_new("(@a{sv})", options({{"handle_token", g_variant_new_string(id.c_str())},
                {"session_handle_token", g_variant_new_string(sid.c_str())}}));
        });
        const gchar* path = nullptr;
        require(g_variant_lookup(created.get(), "session_handle", "&s", &path) && g_variant_is_object_path(path), "portal returned an invalid session handle");
        session = path;
        subscriptions.push_back(g_dbus_connection_signal_subscribe(bus.get(), owner.c_str(), session_interface, "Closed", session.c_str(),
            nullptr, G_DBUS_SIGNAL_FLAGS_NONE, [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
                static_cast<Impl*>(data)->closed = true;
            }, this, nullptr));
        (void)request(remote, "SelectDevices", [&](const std::string& id) {
            return g_variant_new("(o@a{sv})", session.c_str(), options({{"handle_token", g_variant_new_string(id.c_str())}, {"types", g_variant_new_uint32(3)}}));
        });
        const auto modes = property(screen, "AvailableCursorModes", true); selected.embedded_cursor = (modes & 2) != 0;
        (void)request(screen, "SelectSources", [&](const std::string& id) {
            auto* opts = options({{"handle_token", g_variant_new_string(id.c_str())}, {"types", g_variant_new_uint32(1)},
                {"multiple", g_variant_new_boolean(FALSE)}, {"cursor_mode", g_variant_new_uint32(selected.embedded_cursor ? 2 : 1)}});
            return g_variant_new("(o@a{sv})", session.c_str(), opts);
        });
        const bool clip_supported = version >= 2 && property(clipboard, "version", true) >= 1;
        if (clip_supported) {
            (void)call(clipboard, "RequestClipboard", g_variant_new("(o@a{sv})", session.c_str(), options()));
            // D-Bus arg0 string rules do not match object-path-typed arguments.
            // Pin the unique sender here, then check the exact session path in
            // both callbacks; unrelated sessions can never alter this clipboard.
            subscriptions.push_back(g_dbus_connection_signal_subscribe(bus.get(), owner.c_str(), clipboard, nullptr, object, nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE, [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* signal, GVariant* parameters, gpointer data) {
                    auto& self = *static_cast<Impl*>(data);
                    try {
                        if (std::string_view(signal) == "SelectionOwnerChanged") self.owner_changed(parameters);
                        else if (std::string_view(signal) == "SelectionTransfer") self.transfer(parameters);
                    } catch (...) { self.closed = true; }
                }, this, nullptr));
        }
        const auto started = request(remote, "Start", [&](const std::string& id) {
            return g_variant_new("(os@a{sv})", session.c_str(), "", options({{"handle_token", g_variant_new_string(id.c_str())}}));
        });
        guint devices = 0; require(g_variant_lookup(started.get(), "devices", "u", &devices) && (devices & 3) == 3,
                                   "user did not grant required input devices");
        gboolean clip_enabled = FALSE; g_variant_lookup(started.get(), "clipboard_enabled", "b", &clip_enabled);
        clipboard_enabled = clip_supported && clip_enabled;
        Variant streams(g_variant_lookup_value(started.get(), "streams", G_VARIANT_TYPE("a(ua{sv})")));
        require(streams && g_variant_n_children(streams.get()) == 1, "portal must grant exactly one selected monitor");
        Variant stream(g_variant_get_child_value(streams.get(), 0)); GVariant* raw = nullptr;
        g_variant_get(stream.get(), "(u@a{sv})", &selected.node, &raw); Variant properties(raw);
        guint64 serial = 0; if (g_variant_lookup(properties.get(), "pipewire-serial", "t", &serial)) selected.serial = serial;
        gint width = 0, height = 0;
        if (!g_variant_lookup(properties.get(), "logical_size", "(ii)", &width, &height))
            (void)g_variant_lookup(properties.get(), "size", "(ii)", &width, &height);
        require(width >= 0 && height >= 0 && width <= 32768 && height <= 32768, "invalid portal logical dimensions");
        selected.logical_width = unsigned(width); selected.logical_height = unsigned(height); check();
    }
    UniqueFd read_mime(const std::string& type) override {
        return descriptor(clipboard, "SelectionRead", g_variant_new("(os)", session.c_str(), type.c_str()));
    }
    UniqueFd write_mime(std::uint32_t serial) override {
        return descriptor(clipboard, "SelectionWrite", g_variant_new("(ou)", session.c_str(), serial));
    }
    void finish_mime(std::uint32_t serial, bool success) override {
        enqueue(clipboard, "SelectionWriteDone", g_variant_new("(oub)", session.c_str(), serial, gboolean(success)));
    }
    void offer_mimes(const std::vector<std::string>& formats) override {
        std::vector<const gchar*> names;
        for (const auto& format : formats) names.push_back(format.c_str());
        enqueue(clipboard, "SetSelection", g_variant_new("(o@a{sv})", session.c_str(),
            options({{"mime_types", g_variant_new_strv(names.data(), gssize(names.size()))}})));
    }
    void poll() {
        context.iterate(); check(); if (!clipboard_enabled) return;
        mime.poll(); auto content = mime.take(); if (!content) return;
        clear_ready();
        auto text = [&](std::string_view name) -> std::optional<std::string> {
            const auto it = content->find(name); if (it == content->end()) return {};
            return std::string(it->second->begin(), it->second->end());
        };
        try {
            if (files_enabled) {
                if (auto uris = text("x-special/gnome-copied-files")) { ready_files = decode_file_uris(*uris, true); return; }
                if (auto uris = text("text/uri-list")) { ready_files = decode_file_uris(*uris); return; }
            }
            RichClipboard result;
            result.text = text(utf8_mime); if (!result.text) result.text = text("text/plain");
            if (result.text) require(utf16le(*result.text).size() <= clipboard_quota, "portal text exceeds RDP text quota");
            if (rich_enabled) {
                result.html = text("text/html");
#ifdef LRDP_HAVE_PNG
                if (auto it = content->find("image/png"); it != content->end()) result.image = decode_clipboard_png(*it->second);
#endif
                if (!result.image) for (const char* type : {"image/bmp", "image/x-bmp"}) {
                    if (auto it = content->find(type); it != content->end()) { result.image = decode_clipboard_bmp(*it->second); break; }
                }
                result.validate(); ready_rich = std::move(result);
            } else ready_text = result.text.value_or("");
        } catch (const ProtocolError&) {
            // Bad local MIME data must clear the previous remote offer, not expose
            // stale files or disconnect a correctly authenticated RDP session.
            ready_text = "";
        }
    }

};
PortalSession::PortalSession() : impl_(std::make_unique<Impl>()) { impl_->start(); }
PortalSession::~PortalSession() = default;
const PortalStream& PortalSession::stream() const { impl_->check(); return impl_->selected; }
const std::string& PortalSession::path() const { return impl_->session; }
bool PortalSession::clipboard_available() const { return impl_->clipboard_enabled; }
UniqueFd PortalSession::open_pipewire() {
    return impl_->descriptor(screen, "OpenPipeWireRemote", g_variant_new("(o@a{sv})", impl_->session.c_str(), options()));
}
void PortalSession::poll() { impl_->poll(); }
void PortalSession::flush() {
    impl_->check(); Error error;
    require(g_dbus_connection_flush_sync(impl_->bus.get(), impl_->cancel.get(), &error.value), "cannot flush portal input");
    impl_->context.iterate();
}
void PortalSession::pointer(double x, double y) {
    require(std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0, "invalid portal pointer coordinates");
    impl_->enqueue(remote, "NotifyPointerMotionAbsolute", g_variant_new("(o@a{sv}udd)", impl_->session.c_str(), options(), impl_->selected.node, x, y));
}
void PortalSession::button(unsigned button, bool down) {
    require(button >= 0x110 && button <= 0x11f, "invalid evdev pointer button");
    impl_->enqueue(remote, "NotifyPointerButton", g_variant_new("(o@a{sv}iu)", impl_->session.c_str(), options(), gint(button), guint(down)));
}
void PortalSession::key(unsigned key, bool down) {
    require(key > 0 && key <= 0x2ff, "invalid evdev keyboard code");
    impl_->enqueue(remote, "NotifyKeyboardKeycode", g_variant_new("(o@a{sv}iu)", impl_->session.c_str(), options(), gint(key), guint(down)));
}
void PortalSession::keysym(std::uint32_t symbol, bool down) {
    require(symbol <= 0x0110ffff, "invalid Unicode keysym");
    impl_->enqueue(remote, "NotifyKeyboardKeysym", g_variant_new("(o@a{sv}iu)", impl_->session.c_str(), options(), gint(symbol), guint(down)));
}
void PortalSession::wheel(bool horizontal, int steps) {
    require(steps >= -32 && steps <= 32, "portal wheel burst exceeds policy");
    impl_->enqueue(remote, "NotifyPointerAxisDiscrete", g_variant_new("(o@a{sv}ui)", impl_->session.c_str(), options(), guint(horizontal), gint(steps)));
}
bool PortalSession::enable_rich_clipboard() {
    impl_->check(); if (!impl_->clipboard_enabled) return false;
    impl_->rich_enabled = true; impl_->configure_mimes(); return true;
}
bool PortalSession::enable_file_clipboard() {
    impl_->check(); if (!impl_->clipboard_enabled) return false;
    impl_->files_enabled = true; impl_->configure_mimes(); return true;
}
void PortalSession::set_clipboard_rich(RichClipboard content) {
    impl_->check(); require(impl_->clipboard_enabled && impl_->rich_enabled, "portal rich clipboard was not enabled");
    content.validate(); MimeContent formats;
    if (content.text) {
        require(utf16le(*content.text).size() <= clipboard_quota, "portal text exceeds RDP text quota");
        auto bytes = Impl::bytes(*content.text); formats.emplace(utf8_mime, bytes); formats.emplace("text/plain", bytes);
    }
    if (content.html) formats.emplace("text/html", Impl::bytes(*content.html));
    if (content.image) {
#ifdef LRDP_HAVE_PNG
        formats.emplace("image/png", std::make_shared<const Bytes>(encode_clipboard_png(*content.image)));
#else
        formats.emplace("image/bmp", std::make_shared<const Bytes>(encode_clipboard_bmp(*content.image)));
#endif
    }
    impl_->clear_ready(); impl_->mime.set(std::move(formats));
}
std::optional<RichClipboard> PortalSession::take_clipboard_rich() {
    impl_->check(); auto result = std::move(impl_->ready_rich); impl_->ready_rich.reset(); return result;
}
void PortalSession::set_clipboard_files(std::vector<std::string> paths) {
    impl_->check(); require(impl_->clipboard_enabled && impl_->files_enabled, "portal file clipboard was not enabled");
    MimeContent formats;
    formats.emplace("text/uri-list", Impl::bytes(encode_file_uris(paths)));
    formats.emplace("x-special/gnome-copied-files", Impl::bytes(encode_file_uris(paths, true)));
    impl_->clear_ready(); impl_->mime.set(std::move(formats));
}
std::optional<std::vector<std::string>> PortalSession::take_clipboard_files() {
    impl_->check(); auto result = std::move(impl_->ready_files); impl_->ready_files.reset(); return result;
}
void PortalSession::set_clipboard(std::string text) {
    impl_->check(); require(impl_->clipboard_enabled, "portal clipboard was not granted");
    require(text.size() <= clipboard_quota && utf16le(text).size() <= clipboard_quota, "portal clipboard text exceeds quota");
    const auto bytes = Impl::bytes(text); impl_->clear_ready();
    impl_->mime.set({{utf8_mime, bytes}, {"text/plain", bytes}});
}
std::optional<std::string> PortalSession::take_clipboard() {
    impl_->check(); auto result = std::move(impl_->ready_text); impl_->ready_text.reset(); return result;
}
} // namespace lrdp
