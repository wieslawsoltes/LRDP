#include "lrdp/clipboard.hpp"
#include "lrdp/clipboard/rich_content.hpp"
#include <algorithm>
#include <bit>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
using namespace lrdp;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { ++checks; try { f(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected ProtocolError"); }
ClipboardImage image() { return {3,2,{10,20,30,0,40,50,60,128,70,80,90,255,100,110,120,255,130,140,150,255,160,170,180,255}}; }
void replace(Bytes& bytes, std::string_view name, std::string_view value) {
    const auto at = std::search(bytes.begin(),bytes.end(),name.begin(),name.end());
    check(at != bytes.end(),"field to replace exists");
    std::copy(value.begin(),value.end(),at+std::ptrdiff_t(name.size()));
}
void html() {
    const std::string fragment="<b>Zażółć 🚀 世界</b>\r\n<p>test</p>";
    const auto encoded=encode_clipboard_html(fragment);
    check(decode_clipboard_html(encoded)==fragment,"CF_HTML preserves UTF-8 fragment byte offsets");
    check(decode_clipboard_html(View(encoded).first(encoded.size()-1))==fragment,"CF_HTML optional terminal NUL");
    check(decode_clipboard_html(encode_clipboard_html(""))=="","empty HTML fragment");
    auto bad=encoded; replace(bad,"EndFragment:","9999999999"); rejects([&]{ (void)decode_clipboard_html(bad); });
    bad=encoded; replace(bad,"StartHTML:","9999999999"); rejects([&]{ (void)decode_clipboard_html(bad); });
    bad=encoded; replace(bad,"Version:","7.0"); rejects([&]{ (void)decode_clipboard_html(bad); });
    for(std::size_t n=0;n<encoded.size()-1;++n) rejects([&]{ (void)decode_clipboard_html(View(encoded).first(n)); });
    auto absent=[](std::size_t start,std::size_t end) {
        std::ostringstream s; s<<"Version:0.9\nStartHTML:-1\nEndHTML:-1\nStartFragment:"<<std::setfill('0')<<std::setw(10)<<start
            <<"\nEndFragment:"<<std::setw(10)<<end<<"\n"; return s.str();
    };
    const auto first=absent(0,0).size(); const auto no_context=absent(first,first+fragment.size())+fragment;
    check(decode_clipboard_html(View(reinterpret_cast<const std::uint8_t*>(no_context.data()),no_context.size()))==fragment,"absent HTML context and LF header");
    rejects([]{ (void)encode_clipboard_html(std::string("x\0y",3)); });
    rejects([]{ (void)encode_clipboard_html(std::string("\xc0\xaf",2)); });
    bad=encoded;
    const auto pos=std::string_view(reinterpret_cast<const char*>(bad.data()),bad.size()).find(fragment);
    check(pos!=std::string_view::npos,"UTF-8 fragment mutation location"); bad[pos]=0xff; rejects([&]{ (void)decode_clipboard_html(bad); });
}
void bitmaps() {
    const auto source=image(); const auto v5=encode_clipboard_dib(source);
    check(v5.size()==124+24,"V5 bitmap byte count");
    check(decode_clipboard_dib(v5)==source,"V5 straight alpha exact roundtrip");
    check(decode_clipboard_bmp(encode_clipboard_bmp(source))==source,"BMP native payload roundtrip");
    auto opaque=source; for(std::size_t i=3;i<opaque.bgra.size();i+=4) opaque.bgra[i]=255;
    check(decode_clipboard_dib(encode_clipboard_dib(source,false))==opaque,"24-bit legacy DIB opaque conversion");
    auto top=v5; const std::uint32_t negative=std::bit_cast<std::uint32_t>(std::int32_t(-2));
    for(unsigned i=0;i<4;++i) top[8+i]=std::uint8_t(negative>>(i*8));
    std::swap_ranges(top.begin()+124,top.begin()+136,top.begin()+136);
    check(decode_clipboard_dib(top)==source,"top-down DIB orientation");
    for(std::size_t n=0;n<v5.size();++n) rejects([&]{ (void)decode_clipboard_dib(View(v5).first(n)); });
    auto bad=v5; bad[12]=2; rejects([&]{ (void)decode_clipboard_dib(bad); });
    bad=v5; bad[44]=0xff; rejects([&]{ (void)decode_clipboard_dib(bad); });
    bad=v5; bad[112]=1; rejects([&]{ (void)decode_clipboard_dib(bad); });
    bad=v5; bad[4]=0xff; bad[5]=0xff; bad[6]=0xff; rejects([&]{ (void)decode_clipboard_dib(bad); });
    bad=v5; bad[16]=1; rejects([&]{ (void)decode_clipboard_dib(bad); });
    auto bmp=encode_clipboard_bmp(source); bmp[10]=0; rejects([&]{ (void)decode_clipboard_bmp(bmp); });
    Writer palette; palette.le32(40).le32(3).le32(1).le16(1).le16(1).le32(0).le32(4).zeros(8).le32(2).le32(0)
        .raw({0,0,0,0,255,255,255,0,0xa0,0,0,0});
    auto monochrome=decode_clipboard_dib(palette.bytes());
    check(monochrome.width==3 && monochrome.bgra[0]==255 && monochrome.bgra[4]==0 && monochrome.bgra[8]==255,"palette bit order");
    RichClipboard oversized; oversized.text=std::string(rich_clipboard_limit,'x');
    rejects([&]{ oversized.validate(); });
}
Bytes capabilities() { Writer w; w.le16(1).le16(0).le16(1).le16(12).le32(2).le32(2); return clipboard_pdu(7,0,w.bytes()); }
Bytes formats(bool rich=true) {
    Writer w; w.le32(13).le16(0);
    if(rich) w.le32(0xc123).raw(utf16le("HTML Format")).le32(17).le16(0).le32(8).le16(0);
    return clipboard_pdu(2,0,w.bytes());
}
unsigned requested(const ClipboardResult& r) {
    check(!r.outbound.empty(),"data request exists"); Reader w(r.outbound.back());
    check(w.le16()==4 && w.le16()==0 && w.le32()==4,"format request shape"); const auto id=w.le32(); w.end(); return id;
}
View payload(const Bytes& pdu) { Reader r(pdu); check(r.le16()==5 && r.le16()==1,"successful data response"); const auto n=r.le32(); const auto d=r.take(n); r.end(); return d; }
void state_machine() {
    Clipboard clip; clip.configure_rich(); (void)clip.start(); (void)clip.accept(capabilities());
    RichClipboard local; local.text="local\ntext"; local.html="<b>LOCAL 🚀</b>"; local.image=image();
    auto offer=clip.set_local_rich(local); check(offer.size()==1,"rich format publication");
    Writer request; request.le32(0xc001);
    auto response=clip.accept(clipboard_pdu(4,0,request.bytes())); check(decode_clipboard_html(payload(response.outbound[0]))==*local.html,"local HTML format-ID mapping");
    request=Writer(); request.le32(17); response=clip.accept(clipboard_pdu(4,0,request.bytes()));
    check(decode_clipboard_dib(payload(response.outbound[0]))==*local.image,"local DIBV5 serving");
    auto newer=local; newer.html="<p>NEW</p>"; check(clip.set_local_rich(newer).empty(),"offer coalescing");
    request=Writer(); request.le32(0xc001); response=clip.accept(clipboard_pdu(4,0,request.bytes()));
    check(decode_clipboard_html(payload(response.outbound[0]))==*local.html,"outstanding offer retains immutable snapshot");
    (void)clip.accept(clipboard_pdu(3,1)); response=clip.accept(clipboard_pdu(4,0,request.bytes()));
    check(decode_clipboard_html(payload(response.outbound[0]))==*newer.html,"ACK publishes new snapshot");
    auto receive=clip.accept(formats()); check(requested(receive)==13,"text first in rich snapshot");
    receive=clip.accept(clipboard_pdu(5,1,utf16le("remote\r\ntext"))); check(!receive.remote_rich && requested(receive)==0xc123,"peer registered HTML identifier");
    receive=clip.accept(clipboard_pdu(5,1,encode_clipboard_html("<em>remote</em>"))); check(requested(receive)==17,"prefer DIBV5 over DIB");
    receive=clip.accept(clipboard_pdu(5,1,encode_clipboard_dib(image())));
    check(receive.remote_rich && receive.remote_rich->image==image() && receive.remote_rich->html=="<em>remote</em>" && receive.remote_rich->text=="remote\ntext","atomic rich snapshot completion");
    (void)clip.accept(formats()); (void)clip.accept(formats(false));
    receive=clip.accept(clipboard_pdu(5,1,utf16le("stale"))); check(!receive.remote_rich && !receive.remote_text && requested(receive)==13,"stale rich completion discarded");
    receive=clip.accept(clipboard_pdu(5,1,utf16le("plain"))); check(receive.remote_text=="plain","new text selection survives generation race");
    (void)clip.accept(formats()); receive=clip.accept(clipboard_pdu(5,2)); check(requested(receive)==0xc123,"failed optional text advances");
    receive=clip.accept(clipboard_pdu(5,1,encode_clipboard_html("<b>partial</b>"))); check(requested(receive)==17,"partial HTML snapshot awaits image");
    receive=clip.accept(clipboard_pdu(5,2)); check(receive.remote_rich && !receive.remote_rich->image && receive.remote_rich->html,"failed image preserves valid HTML");
    Clipboard plain; (void)plain.start(); (void)plain.accept(capabilities()); check(requested(plain.accept(formats()))==13,"default text-only policy preserved");
    rejects([&]{ (void)plain.set_local_rich(local); });
}
void adversarial() {
    std::mt19937 rng(0x52494348);
    for(unsigned n=0;n<10000;++n) {
        Bytes bytes(rng()%1000); for(auto& b:bytes) b=std::uint8_t(rng());
        try{ (void)decode_clipboard_dib(bytes); }catch(const ProtocolError&){}
        try{ (void)decode_clipboard_html(bytes); }catch(const ProtocolError&){}
    }
}
}
int main() {
    try{ html(); bitmaps(); state_machine(); adversarial(); std::cout<<"PASS: "<<checks<<" rich clipboard checks and 20000 malformed codec probes\n"; return 0; }
    catch(const std::exception& e){ std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n'; return 1; }
}
