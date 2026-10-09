#include "lrdp/platform/portal_session.hpp"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <poll.h>
#include <sys/mman.h>
#include <thread>

using namespace lrdp;
namespace {
constexpr auto object = "/org/freedesktop/portal/desktop";
constexpr auto remote = "org.freedesktop.portal.RemoteDesktop";
constexpr auto clip = "org.freedesktop.portal.Clipboard";
constexpr auto screen = "org.freedesktop.portal.ScreenCast";
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
GVariant* dict(std::initializer_list<std::pair<const char*, GVariant*>> fields = {}) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    for (const auto& [key, value] : fields) g_variant_builder_add(&b, "{sv}", key, value);
    return g_variant_builder_end(&b);
}
const char* xml = R"XML(<node>
<interface name="org.freedesktop.portal.RemoteDesktop">
 <property name="version" type="u" access="read"/><property name="AvailableDeviceTypes" type="u" access="read"/>
 <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="SelectDevices"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="NotifyPointerMotionAbsolute"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="in"/><arg type="d" direction="in"/><arg type="d" direction="in"/></method>
 <method name="NotifyPointerButton"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
 <method name="NotifyKeyboardKeycode"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
 <method name="NotifyKeyboardKeysym"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
 <method name="NotifyPointerAxisDiscrete"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="in"/><arg type="i" direction="in"/></method>
</interface>
<interface name="org.freedesktop.portal.ScreenCast">
 <property name="version" type="u" access="read"/><property name="AvailableSourceTypes" type="u" access="read"/><property name="AvailableCursorModes" type="u" access="read"/>
 <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="OpenPipeWireRemote"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="h" direction="out"/></method>
</interface>
<interface name="org.freedesktop.portal.Clipboard">
 <property name="version" type="u" access="read"/>
 <method name="RequestClipboard"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/></method>
 <method name="SetSelection"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/></method>
 <method name="SelectionRead"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="h" direction="out"/></method>
 <method name="SelectionWrite"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="h" direction="out"/></method>
 <method name="SelectionWriteDone"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="b" direction="in"/></method>
 <signal name="SelectionOwnerChanged"><arg type="o"/><arg type="a{sv}"/></signal>
 <signal name="SelectionTransfer"><arg type="o"/><arg type="s"/><arg type="u"/></signal>
</interface>
<interface name="org.freedesktop.portal.Session"><method name="Close"/><signal name="Closed"><arg type="a{sv}"/></signal></interface>
</node>)XML";
class FakePortal {
    std::thread thread_;
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    GDBusConnection* bus_ = nullptr;
    GDBusNodeInfo* interfaces_ = nullptr;
    std::vector<guint> objects_;
    std::mutex mutex_;
    std::string session_, peer_, copied_;
    std::map<std::string, std::string> source_;
    std::map<unsigned, UniqueFd> writes_;
    bool clipboard_requested_ = false, started_ = false;
    void signal(const char* name, GVariant* parameters, const char* iface = clip, const char* path = object) {
        GError* error = nullptr;
        check(g_dbus_connection_emit_signal(bus_, peer_.c_str(), path, iface, name, parameters, &error), "fake portal signal failed");
        if (error) g_error_free(error);
    }
    static void return_fd(GDBusMethodInvocation* invocation, int fd) {
        auto* list = g_unix_fd_list_new(); GError* error = nullptr;
        const auto index = g_unix_fd_list_append(list, fd, &error);
        check(index >= 0, "fake descriptor transfer failed");
        g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(h)", index), list); g_object_unref(list);
        if (error) g_error_free(error);
    }
    static UniqueFd memory(std::string_view text) {
        UniqueFd fd(memfd_create("lrdp-portal-fixture", MFD_CLOEXEC)); check(bool(fd), "memfd unavailable");
        std::size_t written = 0;
        while (written < text.size()) { const auto n = write(fd.get(), text.data() + written, text.size() - written); check(n > 0, "memfd write failed"); written += std::size_t(n); }
        check(lseek(fd.get(), 0, SEEK_SET) == 0, "memfd rewind failed"); return fd;
    }
    void method(const char* iface, const char* name, GVariant* parameters, GDBusMethodInvocation* invocation) {
        std::lock_guard lock(mutex_); const std::string_view method = name;
        if (method == "Close") { ++closes; g_dbus_method_invocation_return_value(invocation, nullptr); return; }
        if (method == "CreateSession" || method == "SelectDevices" || method == "SelectSources" || method == "Start") {
            auto* options = g_variant_get_child_value(parameters, g_variant_n_children(parameters) - 1);
            const gchar* handle = nullptr; check(g_variant_lookup(options, "handle_token", "&s", &handle), "missing request token");
            peer_ = g_dbus_method_invocation_get_sender(invocation); std::string sender = peer_.substr(1); std::replace(sender.begin(), sender.end(), '.', '_');
            const auto request_path = std::string(object) + "/request/" + sender + '/' + handle;
            GVariant* result = nullptr;
            if (method == "CreateSession") {
                const gchar* token = nullptr; check(g_variant_lookup(options, "session_handle_token", "&s", &token), "missing session token");
                session_ = std::string(object) + "/session/" + sender + '/' + token; clipboard_requested_ = started_ = false;
                static const GDBusInterfaceVTable table = vtable();
                objects_.push_back(g_dbus_connection_register_object(bus_, session_.c_str(), interfaces_->interfaces[3], &table, this, nullptr, nullptr));
                result = dict({{"session_handle", g_variant_new_string(session_.c_str())}});
            } else if (method == "SelectDevices") {
                guint types = 0; check(g_variant_lookup(options, "types", "u", &types) && types == 3, "wrong input-device request"); result = dict();
            } else if (method == "SelectSources") {
                guint types = 0, cursor = 0; gboolean multiple = TRUE;
                check(g_variant_lookup(options, "types", "u", &types) && types == 1, "wrong capture source request");
                check(g_variant_lookup(options, "cursor_mode", "u", &cursor) && cursor == 2, "cursor was not requested embedded");
                check(g_variant_lookup(options, "multiple", "b", &multiple) && !multiple, "unexpected multiple streams"); result = dict();
            } else {
                check(clipboard_requested_, "clipboard must be requested before Start"); started_ = true;
                GVariantBuilder streams; g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
                g_variant_builder_add(&streams, "(u@a{sv})", 42U, dict({{"size", g_variant_new("(ii)", 1280, 720)}, {"pipewire-serial", g_variant_new_uint64(9000000001ULL)}}));
                result = dict({{"devices", g_variant_new_uint32(3)}, {"streams", g_variant_builder_end(&streams)},
                    {"clipboard_enabled", g_variant_new_boolean(grant_clipboard.load())}});
            }
            // Emit before returning the method handle: catches subscribe-after-call races.
            signal("Response", g_variant_new("(u@a{sv})", method == "Start" && deny_start.load() ? 1U : 0U, result),
                   "org.freedesktop.portal.Request", request_path.c_str());
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request_path.c_str())); g_variant_unref(options); return;
        }
        if (method == "RequestClipboard") { check(!started_, "late clipboard request"); clipboard_requested_ = true; }
        else if (method == "OpenPipeWireRemote") { auto fd = memory("granted-fd"); return_fd(invocation, fd.get()); return; }
        else if (method == "SelectionRead") {
            const gchar *path = nullptr, *mime = nullptr; g_variant_get(parameters, "(&o&s)", &path, &mime);
            check(path == session_, "wrong read session"); auto fd = memory(source_.at(mime)); return_fd(invocation, fd.get()); return;
        }
        else if (method == "SelectionWrite") {
            const gchar* path = nullptr; guint serial = 0; g_variant_get(parameters, "(&ou)", &path, &serial);
            auto fd = memory(""); return_fd(invocation, fd.get()); writes_.insert_or_assign(serial, std::move(fd)); return;
        } else if (method == "SelectionWriteDone") {
            const gchar* path = nullptr; guint serial = 0; gboolean success = FALSE; g_variant_get(parameters, "(&oub)", &path, &serial, &success);
            if (success) {
                auto it = writes_.find(serial); check(it != writes_.end(), "write completed without a descriptor");
                const auto size = lseek(it->second.get(), 0, SEEK_END); check(size >= 0 && size <= 8 * 1024 * 1024, "invalid copied text length");
                copied_.resize(std::size_t(size)); check(pread(it->second.get(), copied_.data(), copied_.size(), 0) == size, "copied text unreadable");
                writes_.erase(it); ++successful_copies;
            } else ++failed_copies;
        } else if (method == "SetSelection") {
            ++selections; signal("SelectionOwnerChanged", g_variant_new("(o@a{sv})", session_.c_str(), dict({{"session_is_owner", g_variant_new_boolean(TRUE)}})));
        } else if (std::string_view(iface) == remote && method.starts_with("Notify")) ++inputs;
        else throw std::runtime_error("unknown fake portal method");
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
    static GDBusInterfaceVTable vtable() {
        GDBusInterfaceVTable value{};
        value.method_call = [](GDBusConnection*, const gchar*, const gchar*, const gchar* iface, const gchar* name, GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
            auto& self = *static_cast<FakePortal*>(data);
            try { self.method(iface, name, parameters, invocation); }
            catch (const std::exception& error) { self.failed = true; g_dbus_method_invocation_return_dbus_error(invocation, "org.lrdp.Test.Error", error.what()); }
        };
        value.get_property = [](GDBusConnection*, const gchar*, const gchar*, const gchar* iface, const gchar* name, GError**, gpointer) -> GVariant* {
            if (std::string_view(name) == "version") return g_variant_new_uint32(std::string_view(iface) == remote ? 2 : std::string_view(iface) == screen ? 6 : 1);
            return g_variant_new_uint32(std::string_view(name) == "AvailableSourceTypes" ? 1 : std::string_view(name) == "AvailableCursorModes" ? 2 : 3);
        };
        return value;
    }
public:
    std::atomic<bool> failed = false, deny_start = false, grant_clipboard = true;
    std::atomic<unsigned> inputs = 0, selections = 0, closes = 0, successful_copies = 0, failed_copies = 0;
    explicit FakePortal(const char* address) {
        std::promise<void> started; auto ready = started.get_future();
        thread_ = std::thread([&, location = std::string(address)] {
            try {
                context_ = g_main_context_new(); g_main_context_push_thread_default(context_); loop_ = g_main_loop_new(context_, FALSE);
                GError* error = nullptr;
                bus_ = g_dbus_connection_new_for_address_sync(location.c_str(), GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, &error);
                check(bus_ != nullptr, "fake portal bus connection failed"); g_dbus_connection_set_exit_on_close(bus_, FALSE);
                interfaces_ = g_dbus_node_info_new_for_xml(xml, &error); check(interfaces_ != nullptr, "invalid fake portal XML");
                static const GDBusInterfaceVTable table = vtable();
                for (unsigned i = 0; i < 3; ++i) objects_.push_back(g_dbus_connection_register_object(bus_, object, interfaces_->interfaces[i], &table, this, nullptr, &error));
                auto* reply = g_dbus_connection_call_sync(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                    g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
                check(reply != nullptr, "cannot own fake portal name"); g_variant_unref(reply); started.set_value();
                g_main_loop_run(loop_);
            } catch (...) { failed = true; try { started.set_exception(std::current_exception()); } catch (...) {} }
            if (bus_) { for (auto id : objects_) if (id) g_dbus_connection_unregister_object(bus_, id); g_dbus_connection_close_sync(bus_, nullptr, nullptr); g_object_unref(bus_); }
            if (interfaces_) g_dbus_node_info_unref(interfaces_);
            if (loop_) g_main_loop_unref(loop_);
            if (context_) { g_main_context_pop_thread_default(context_); g_main_context_unref(context_); }
        });
        try { ready.get(); } catch (...) { thread_.join(); throw; }
    }
    ~FakePortal() {
        g_main_context_invoke(context_, [](gpointer data) -> gboolean { g_main_loop_quit(static_cast<GMainLoop*>(data)); return G_SOURCE_REMOVE; }, loop_);
        thread_.join();
    }
    void offer(std::string text) { offer_formats({{"text/plain;charset=utf-8", std::move(text)}}); }
    void offer_formats(std::map<std::string, std::string> content) {
        std::lock_guard lock(mutex_); source_ = std::move(content); std::vector<const gchar*> types;
        for (const auto& [type, unused] : source_) { (void)unused; types.push_back(type.c_str()); }
        signal("SelectionOwnerChanged", g_variant_new("(o@a{sv})", session_.c_str(),
            dict({{"session_is_owner", g_variant_new_boolean(FALSE)}, {"mime_types", g_variant_new_strv(types.data(), gssize(types.size()))}})));
    }
    void copy(unsigned serial, const char* mime = "text/plain;charset=utf-8") {
        std::lock_guard lock(mutex_); signal("SelectionTransfer", g_variant_new("(osu)", session_.c_str(), mime, serial));
    }
    std::string copied() { std::lock_guard lock(mutex_); return copied_; }
    void revoke() { std::lock_guard lock(mutex_); signal("Closed", g_variant_new("(@a{sv})", dict()), "org.freedesktop.portal.Session", session_.c_str()); }
};
}
int main() {
    auto* bus = g_test_dbus_new(G_TEST_DBUS_NONE); g_test_dbus_up(bus);
    try {
        {
            FakePortal fake(g_test_dbus_get_bus_address(bus));
            {
                PortalSession session;
                check(session.clipboard_available() && session.stream().node == 42 && session.stream().serial == 9000000001ULL && session.stream().embedded_cursor,
                      "portal permissions/stream identity were not parsed");
                auto fd = session.open_pipewire(); char marker[10]{}; check(read(fd.get(), marker, sizeof(marker)) == 10 && std::string(marker, 10) == "granted-fd", "granted descriptor ownership");
                auto until = [&](auto condition) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!condition()) { session.poll(); check(!fake.failed && std::chrono::steady_clock::now() < deadline, "portal test timed out"); ::poll(nullptr, 0, 1); }
                };
                session.pointer(100.5, 200.25); session.button(272, true); session.button(272, false);
                session.key(30, true); session.key(30, false); session.keysym(0x0101f680, true); session.keysym(0x0101f680, false); session.wheel(false, -1);
                until([&] { return fake.inputs == 8; });
                std::string large(180000, 'x'); large += "\nZażółć 🚀"; std::optional<std::string> received;
                fake.offer(large); until([&] { if (auto text = session.take_clipboard()) received = std::move(*text); return received.has_value(); });
                check(*received == large, "portal clipboard input corrupted");
                session.set_clipboard(large); until([&] { return fake.selections == 1; });
                fake.copy(17); until([&] { return fake.successful_copies == 1; }); check(fake.copied() == large, "portal clipboard output corrupted");
                fake.copy(18, "application/unsupported"); until([&] { return fake.failed_copies == 1; });
                check(session.enable_rich_clipboard() && session.enable_file_clipboard(), "optional native formats were not enabled");
                RichClipboard rich; rich.text = "plain 🚀"; rich.html = "<p>" + std::string(1200000, 'h') + "日本語</p>";
                rich.image = ClipboardImage{2, 2, {0, 0, 255, 255, 0, 128, 0, 128, 255, 0, 0, 255, 4, 5, 6, 0}};
                const auto bmp = encode_clipboard_bmp(*rich.image);
                fake.offer_formats({{"text/plain;charset=utf-8", *rich.text}, {"text/html", *rich.html},
                    {"image/bmp", std::string(bmp.begin(), bmp.end())}});
                std::optional<RichClipboard> native_rich;
                until([&] { if (auto value = session.take_clipboard_rich()) native_rich = std::move(value); return native_rich && native_rich->html.has_value(); });
                check(native_rich->text == rich.text && native_rich->html == rich.html && native_rich->image == rich.image, "portal multi-format incoming snapshot");
                session.set_clipboard_rich(rich); until([&] { return fake.selections == 2; });
                fake.copy(20, "text/html"); until([&] { return fake.successful_copies == 2; });
                check(fake.copied() == *rich.html, "large HTML outgoing descriptor stream");
#ifdef LRDP_TEST_PNG
                fake.copy(21, "image/png"); until([&] { return fake.successful_copies == 3; });
                const auto png = fake.copied();
                check(decode_clipboard_png(View(reinterpret_cast<const std::uint8_t*>(png.data()), png.size())) == *rich.image, "portal alpha PNG export");
#else
                fake.copy(21, "image/bmp"); until([&] { return fake.successful_copies == 3; });
                check(fake.copied() == std::string(bmp.begin(), bmp.end()), "portal BMP export");
#endif
                fake.offer_formats({{"text/uri-list", "file:///tmp/a%20b.txt\r\nfile:///tmp/%E6%97%A5%E6%9C%AC\r\n"}, {"text/html", "ignored for file selection"}});
                std::optional<std::vector<std::string>> paths;
                until([&] { if (auto value = session.take_clipboard_files()) paths = std::move(value); return paths.has_value(); });
                check(*paths == std::vector<std::string>{"/tmp/a b.txt", "/tmp/日本"}, "portal file URI conversion and precedence");
                session.set_clipboard_files(*paths); until([&] { return fake.selections == 3; });
                fake.copy(22, "x-special/gnome-copied-files"); until([&] { return fake.successful_copies == 4; });
                check(fake.copied() == "copy\nfile:///tmp/a%20b.txt\nfile:///tmp/%E6%97%A5%E6%9C%AC\n", "portal copied-files ownership");
                fake.offer_formats({{"text/uri-list", "file://untrusted-host/a.txt\r\n"}});
                std::optional<std::string> cleared;
                until([&] { if (auto value = session.take_clipboard()) cleared = std::move(value); return cleared.has_value(); });
                check(cleared->empty(), "invalid local files clear the stale remote offer");
                fake.revoke(); bool rejected = false; const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!rejected && std::chrono::steady_clock::now() < deadline) {
                    try { session.poll(); } catch (const ProtocolError&) { rejected = true; } ::poll(nullptr, 0, 1);
                }
                check(rejected, "revoked portal session remained usable");
            }
            check(fake.closes == 1, "portal session was not closed on destruction");
            fake.grant_clipboard = false;
            { PortalSession no_clipboard; check(!no_clipboard.clipboard_available() && !no_clipboard.enable_file_clipboard() && !no_clipboard.enable_rich_clipboard(), "denied clipboard was advertised"); }
            fake.deny_start = true; bool denied = false;
            try { PortalSession cancelled; } catch (const ProtocolError&) { denied = true; }
            check(denied && fake.closes == 3 && !fake.failed, "cancelled consent leaked a session");
        }
        g_test_dbus_down(bus); g_object_unref(bus);
        std::cout << "PASS: private D-Bus portal, early response race, granted descriptors, input API, 180 KiB text, 1.2 MB HTML, alpha images, file URIs, permission denial/revocation and cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; g_test_dbus_down(bus); g_object_unref(bus); return 1;
    }
}
