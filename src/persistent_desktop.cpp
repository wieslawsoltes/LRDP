#include "lrdp/platform/persistent_desktop.hpp"
#include "lrdp/platform/randr_outputs.hpp"
#include "lrdp/platform/x11_managed.hpp"
#include "lrdp/session/broker.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <openssl/crypto.h>
#include "lrdp/platform/shared_library.hpp"
#include <X11/Xlib.h>

namespace lrdp {
namespace {
class AuthorityScope {
    // One X11 adapter is owned by this connection's event-loop thread. Use
    // Xlib's explicit authorization, not process environment mutation while
    // audio/drive workers may concurrently consult their own environment.
public:
    explicit AuthorityScope(const std::string& path) {
        UniqueFd fd(open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));struct stat info{};
        require(fd && fstat(fd.get(),&info)==0 && S_ISREG(info.st_mode) && info.st_uid==geteuid() &&
                (info.st_mode&0777)==0600 && info.st_size>0 && info.st_size<=256,"invalid leased Xauthority file");
        std::array<std::uint8_t,256> bytes{};
        struct Wipe { decltype(bytes)& b; ~Wipe(){OPENSSL_cleanse(b.data(),b.size());} } wipe{bytes};
        std::size_t used=0;
        while(used<std::size_t(info.st_size)) {
            const auto n=read(fd.get(),bytes.data()+used,std::size_t(info.st_size)-used);
            if(n<0 && errno==EINTR)continue;
            require(n>0,"truncated leased Xauthority file");used+=std::size_t(n);
        }
        Reader in(View(bytes).first(used));require(in.be16()==65535,"invalid private Xauthority family");
        require(in.be16()==0,"invalid private Xauthority address");in.skip(in.be16());
        const auto name=in.take(in.be16());
        constexpr std::string_view expected="MIT-MAGIC-COOKIE-1";
        require(name.size()==expected.size() && std::equal(name.begin(),name.end(),expected.begin()),"unsupported private Xauthority protocol");
        const auto key=in.take(in.be16());require(key.size()==16,"invalid private Xauthority key length");in.end();
        XSetAuthorization(reinterpret_cast<char*>(const_cast<std::uint8_t*>(name.data())),int(name.size()),
                          reinterpret_cast<char*>(const_cast<std::uint8_t*>(key.data())),int(key.size()));
    }
    ~AuthorityScope(){XSetAuthorization(nullptr,0,nullptr,0);}
    AuthorityScope(const AuthorityScope&)=delete;
    AuthorityScope& operator=(const AuthorityScope&)=delete;
};
void release_crashed_input(const std::string& name) {
    // This is an exclusively leased private display, never a physical desktop.
    // The old connection may have died before it could release its held keys.
    SharedLibrary test("libXtst.so.6");
    using Event=int(*)(Display*,unsigned,Bool,unsigned long);
    auto key=test.symbol<Event>("XTestFakeKeyEvent"), button=test.symbol<Event>("XTestFakeButtonEvent");
    std::unique_ptr<Display,decltype(&XCloseDisplay)> display(XOpenDisplay(name.c_str()),XCloseDisplay);
    require(display!=nullptr,"cannot reset retained desktop input");
    char keys[32]{};XQueryKeymap(display.get(),keys);
    for (unsigned i=8;i<256;++i) if(static_cast<unsigned char>(keys[i/8])&(1U<<(i%8))) key(display.get(),i,False,CurrentTime);
    for (unsigned i=1;i<=9;++i) button(display.get(),i,False,CurrentTime);
    XSync(display.get(),False);
}
class PersistentDesktop final:public Desktop,public ReconnectService {
    std::string socket_path_,principal_;
    // Lease outlives the X11 adapter so input/clipboard resources are released
    // before another connection may attach to the private display.
    std::unique_ptr<persistent::BrokerClient> lease_;
    std::shared_ptr<RandrOutputs> outputs_=std::make_shared<RandrOutputs>();
    std::unique_ptr<Desktop> native_;
    Layout pending_=validate_layout({Monitor{}});
    bool rich_=false,files_=false,activated_=false,retained_=false;
    persistent::Time rotation_{};
    Desktop& native()const{require(native_!=nullptr,"persistent desktop is not attached");return *native_;}
public:
    PersistentDesktop(std::string socket,std::string principal):socket_path_(std::move(socket)),principal_(std::move(principal)){}
    ReconnectService* reconnection()override{return this;}
    void prepare(const std::optional<ReconnectCookie>& verifier)override {
        require(!lease_,"persistent desktop already prepared");
        lease_=std::make_unique<persistent::BrokerClient>(socket_path_);
        const auto attachment=lease_->acquire(principal_,verifier);
        AuthorityScope authority(attachment.authority);
        release_crashed_input(attachment.display);
        native_=make_managed_x11_desktop(attachment.display,[outputs=outputs_](auto* display,const Layout& layout){return outputs->apply(display,layout);});
        require(native_->resize(pending_),"retained desktop resize failed");
        if(rich_)require(native_->enable_rich_clipboard(),"retained desktop lacks rich clipboard");
        if(files_)require(native_->enable_file_clipboard(),"retained desktop lacks file clipboard");
    }
    std::optional<ReconnectCookie> activated(bool supports)override {
        require(lease_ && native_ && !activated_,"persistent desktop activation out of sequence");
        auto cookie=lease_->commit(supports);activated_=true;retained_=supports;
        rotation_=persistent::Clock::now()+std::chrono::hours(1);return cookie;
    }
    std::optional<ReconnectCookie> refresh()override {
        if(!retained_ || persistent::Clock::now()<rotation_)return {};
        auto cookie=lease_->rotate();rotation_=persistent::Clock::now()+std::chrono::hours(1);return cookie;
    }
    void pump()override{if(lease_)lease_->check();if(native_)native_->pump();}
    Layout layout()const override{return native_?native_->layout():pending_;}
    bool resizable()const override{return true;}
    bool resize(const Layout& layout)override{if(native_)return native_->resize(layout);pending_=validate_layout(layout.monitors);return true;}
    Frame capture()override{return native().capture();}
    std::shared_ptr<const PointerShape> pointer_shape()override{return native().pointer_shape();}
    void input(const InputEvent& event)override{native().input(event);}
    void release_input()override{if(native_)native_->release_input();}
    bool enable_rich_clipboard()override{rich_=true;return native_?native_->enable_rich_clipboard():true;}
    bool enable_file_clipboard()override{files_=true;return native_?native_->enable_file_clipboard():true;}
    void set_clipboard_rich(RichClipboard content)override{native().set_clipboard_rich(std::move(content));}
    std::optional<RichClipboard> poll_clipboard_rich()override{return native().poll_clipboard_rich();}
    void set_clipboard_files(std::vector<std::string> paths)override{native().set_clipboard_files(std::move(paths));}
    std::optional<std::vector<std::string>> poll_clipboard_files()override{return native().poll_clipboard_files();}
    void set_clipboard(std::string text)override{native().set_clipboard(std::move(text));}
    std::optional<std::string> poll_clipboard()override{return native().poll_clipboard();}
};
}
std::unique_ptr<Desktop> make_persistent_desktop(std::string socket,std::string principal){return std::make_unique<PersistentDesktop>(std::move(socket),std::move(principal));}
}
