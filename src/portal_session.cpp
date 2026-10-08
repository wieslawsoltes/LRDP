#include "lrdp/platform/portal_session.hpp"
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
struct PortalSession::Impl {
    Context context;
    std::unique_ptr<GDBusConnection, ObjectDelete> bus;
    std::unique_ptr<GCancellable, ObjectDelete> cancel{g_cancellable_new()};
    std::string owner, session;
    PortalStream selected;
    std::vector<guint> subscriptions;
    struct AsyncState { unsigned pending = 0; bool failed = false, stopping = false; };
    std::shared_ptr<AsyncState> async = std::make_shared<AsyncState>();
    bool closed = false, clipboard_enabled = false;
    std::uint64_t generation = 0;
    std::optional<std::string> requested_mime, ready_text;
    struct ReadTransfer { UniqueFd fd; std::string text; std::uint64_t generation; Clock::time_point deadline; };
    std::optional<ReadTransfer> incoming;
    struct WriteRequest { std::uint32_t serial; std::string mime; std::shared_ptr<const std::string> text; };
    struct WriteTransfer { UniqueFd fd; std::shared_ptr<const std::string> text; std::size_t offset; Clock::time_point deadline; };
    std::deque<WriteRequest> requests;
    std::map<std::uint32_t, WriteTransfer> outgoing;
    std::shared_ptr<const std::string> local;

    ~Impl() {
        context.check(); async->stopping = true;
        if (bus) {
            for (auto id : subscriptions) g_dbus_connection_signal_unsubscribe(bus.get(), id);
            subscriptions.clear(); incoming.reset(); outgoing.clear(); requests.clear();
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
        ++generation; incoming.reset(); requested_mime.reset(); ready_text.reset();
        gboolean own = FALSE; g_variant_lookup(fields.get(), "session_is_owner", "b", &own); if (own) return;
        Variant formats(g_variant_lookup_value(fields.get(), "mime_types", G_VARIANT_TYPE_STRING_ARRAY));
        if (!formats) return;
        require(g_variant_n_children(formats.get()) <= 256, "too many portal clipboard formats");
        GVariantIter iter; g_variant_iter_init(&iter, formats.get()); const gchar* name = nullptr;
        while (g_variant_iter_next(&iter, "&s", &name)) {
            if (std::string_view(name) == utf8_mime) { requested_mime = name; break; }
            if (std::string_view(name) == "text/plain") requested_mime = name;
        }
    }
    void transfer(GVariant* parameters) {
        require(g_variant_is_of_type(parameters, G_VARIANT_TYPE("(osu)")), "invalid clipboard transfer signal");
        const gchar* path = nullptr; const gchar* mime = nullptr; guint serial = 0;
        g_variant_get(parameters, "(&o&su)", &path, &mime, &serial); if (path != session) return;
        require(requests.size() + outgoing.size() < 16, "portal clipboard transfer queue exceeded");
        requests.push_back({serial, mime, local});
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
    void done(std::uint32_t serial, bool success) {
        enqueue(clipboard, "SelectionWriteDone", g_variant_new("(oub)", session.c_str(), serial, gboolean(success)));
    }
    void poll() {
        context.iterate(); check(); if (!clipboard_enabled) return;
        if (requested_mime) {
            auto mime = std::move(*requested_mime); requested_mime.reset();
            auto fd = descriptor(clipboard, "SelectionRead", g_variant_new("(os)", session.c_str(), mime.c_str())); fd.nonblocking();
            incoming.emplace(ReadTransfer{std::move(fd), {}, generation, Clock::now() + std::chrono::seconds(5)});
        }
        if (incoming) {
            auto& input = *incoming;
            if (input.generation != generation || Clock::now() >= input.deadline) incoming.reset();
            else {
                char buffer[16384];
                for (unsigned i = 0; i < 4 && incoming; ++i) {
                    const auto n = ::read(input.fd.get(), buffer, sizeof(buffer));
                    if (n > 0) {
                        if (std::size_t(n) > clipboard_quota - input.text.size()) { incoming.reset(); break; }
                        input.text.append(buffer, std::size_t(n));
                    } else if (n == 0) {
                        try { require(utf16le(input.text).size() <= clipboard_quota, "clipboard text exceeds wire quota"); ready_text = std::move(input.text); }
                        catch (const ProtocolError&) {}
                        incoming.reset();
                    } else if (errno == EINTR) continue;
                    else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    else incoming.reset();
                }
            }
        }
        if (!requests.empty()) {
            auto request = std::move(requests.front()); requests.pop_front();
            if (!request.text || (request.mime != utf8_mime && request.mime != "text/plain") || outgoing.contains(request.serial)) done(request.serial, false);
            else {
                auto fd = descriptor(clipboard, "SelectionWrite", g_variant_new("(ou)", session.c_str(), request.serial)); fd.nonblocking();
                outgoing.emplace(request.serial, WriteTransfer{std::move(fd), std::move(request.text), 0, Clock::now() + std::chrono::seconds(5)});
            }
        }
        std::size_t budget = 65536;
        for (auto it = outgoing.begin(); it != outgoing.end();) {
            auto& output = it->second; bool failed = Clock::now() >= output.deadline;
            if (!failed && budget && output.offset < output.text->size()) {
                const auto count = std::min(budget, output.text->size() - output.offset);
                const auto n = ::write(output.fd.get(), output.text->data() + output.offset, count);
                if (n > 0) { output.offset += std::size_t(n); budget -= std::size_t(n); }
                else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) failed = true;
            }
            if (failed || output.offset == output.text->size()) {
                const auto serial = it->first; output.fd.reset(); it = outgoing.erase(it); done(serial, !failed);
            } else ++it;
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
void PortalSession::set_clipboard(std::string text) {
    impl_->check(); require(impl_->clipboard_enabled, "portal clipboard was not granted");
    require(text.size() <= clipboard_quota && utf16le(text).size() <= clipboard_quota, "portal clipboard text exceeds quota");
    impl_->local = std::make_shared<const std::string>(std::move(text));
    ++impl_->generation; impl_->incoming.reset(); impl_->ready_text.reset(); impl_->requested_mime.reset();
    const gchar* types[] = {utf8_mime, "text/plain"};
    impl_->enqueue(clipboard, "SetSelection", g_variant_new("(o@a{sv})", impl_->session.c_str(), options({{"mime_types", g_variant_new_strv(types, 2)}})));
}
std::optional<std::string> PortalSession::take_clipboard() {
    impl_->check(); auto result = std::move(impl_->ready_text); impl_->ready_text.reset(); return result;
}
} // namespace lrdp
