#include "lrdp/platform/clipboard_file_store.hpp"
#include "lrdp/clipboard/file_uri.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace lrdp;
namespace fs = std::filesystem;
namespace {
void check(bool c,const char* m) {if(!c) throw std::runtime_error(m);}
template<class F> void rejects(F f) {try {f();} catch(const ProtocolError&) {return;} throw std::runtime_error("expected rejection");}
struct Temp {
    fs::path path;
    Temp() { char name[]="/tmp/lrdp-file-test-XXXXXX"; auto* p=mkdtemp(name); check(p!=nullptr,"temp directory"); path=p; }
    ~Temp() {std::error_code ec; fs::remove_all(path,ec);}
};
Bytes load(const fs::path& p) {std::ifstream f(p,std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f),{});}
std::size_t staging(const fs::path& root) {
    std::size_t count=0; for(const auto& e:fs::directory_iterator(root)) if(e.path().filename().string().starts_with(".lrdp-clipboard-")) ++count;
    return count;
}
void uris() {
    std::vector<std::string> paths={"/tmp/zażółć 🚀#%.txt","/a/b"};
    check(decode_file_uris(encode_file_uris(paths))==paths,"URI Unicode/percent roundtrip");
    check(decode_file_uris(encode_file_uris(paths,true),true)==paths,"GNOME copied-files roundtrip");
    check(decode_file_uris("#comment\r\nfile://localhost/tmp/a%20b\r\n")==std::vector<std::string>{"/tmp/a b"},"localhost authority");
    check(decode_file_uris("cut\nfile:///a\n",true)==std::vector<std::string>{"/a"},"cut is read as copy, never source deletion");
    for(const auto uri:{"file://evil/a","smb://evil/a","file:///a%00x","file:///a%0a","file:///a?query","file:///a#frag","file:///a%QQ","file:///a%","file://user@localhost/a"})
        rejects([&]{(void)decode_file_uris(uri);});
}
void files() {
    Temp root, outside; fs::create_directories(root.path/"source/empty");
    Bytes original(200003); for(std::size_t i=0;i<original.size();++i) original[i]=std::uint8_t(i*13);
    {std::ofstream f(root.path/"source/世界 🚀.bin",std::ios::binary); f.write(reinterpret_cast<const char*>(original.data()),std::streamsize(original.size()));}
    std::ofstream(root.path/"zero");
    auto store=make_clipboard_file_store(root.path.string());
    auto source=store->offer({(root.path/"source").string(),(root.path/"zero").string()});
    check(source->files().size()==4,"recursive source descriptors include empty directories");
    std::size_t file=0; for(std::size_t i=0;i<source->files().size();++i) if(source->files()[i].name.ends_with(".bin")) file=i;
    check(source->read(file,170001,100)==Bytes(original.begin()+170001,original.begin()+170101),"bounded native pread");
    rejects([&]{(void)store->offer({outside.path.string()});});
    rejects([&]{(void)store->offer({root.path.string()+"/../outside"});});
    fs::create_symlink(outside.path,root.path/"symbolic");
    rejects([&]{(void)store->offer({(root.path/"symbolic").string()});});
    rejects([&]{(void)store->offer({(root.path/"symbolic/anything").string()});});
    check(mkfifo((root.path/"fifo").c_str(),0600)==0,"create FIFO test");
    rejects([&]{(void)store->offer({(root.path/"fifo").string()});});
    fs::create_symlink(root.path, outside.path/"link-root");
    rejects([&]{(void)make_clipboard_file_store((outside.path/"link-root").string());});
    chmod(outside.path.c_str(),0777); rejects([&]{(void)make_clipboard_file_store(outside.path.string());});
    auto descriptors=source->files(); std::reverse(descriptors.begin(),descriptors.end());
    auto sink=store->receive(descriptors); check(staging(root.path)==1,"private staging created");
    rejects([&]{(void)sink->finish();});
    std::size_t target=0; for(std::size_t i=0;i<descriptors.size();++i) if(descriptors[i].name.ends_with(".bin")) target=i;
    std::vector<std::pair<std::size_t,std::size_t>> blocks;
    for(std::size_t offset=0;offset<original.size();offset+=65536) blocks.emplace_back(offset,std::min<std::size_t>(65536,original.size()-offset));
    std::reverse(blocks.begin(),blocks.end());
    for(const auto [offset,count]:blocks) sink->write(target,offset,View(original).subspan(offset,count));
    rejects([&]{sink->write(target,0,View(original).first(1));});
    const auto paths=sink->finish(); check(paths.size()==2,"top-level file URIs only");
    fs::path published;
    for(const auto& path:paths) if(fs::path(path).filename()=="source") published=path;
    check(load(published/"世界 🚀.bin")==original && fs::is_directory(published/"empty"),"native received tree bytes");
    struct stat st{}; check(stat((published/"世界 🚀.bin").c_str(),&st)==0 && (st.st_mode&0777)==0600,"received files never executable");
    check(stat(published.c_str(),&st)==0 && (st.st_mode&0777)==0700,"private received directory permissions");
    sink.reset(); check(staging(root.path)==1,"completed clipboard remains available during session");
    {auto abort=store->receive({{"partial",false,10,{}}}); abort->write(0,0,Bytes{1,2});}
    check(staging(root.path)==1,"aborted staging removed");
    {std::ofstream f(root.path/"source/世界 🚀.bin",std::ios::binary|std::ios::app); f.put('x');}
    rejects([&]{(void)source->read(file,0,1);});
    FileClipboardLimits tiny; tiny.bytes=8; auto small=make_clipboard_file_store(root.path.string(),tiny);
    auto a=small->receive({{"first",false,8,{}}}); a->write(0,0,Bytes(8)); (void)a->finish(); a.reset();
    rejects([&]{(void)small->receive({{"second",false,1,{}}});});
    rejects([&]{(void)small->offer({(root.path/"source").string()});});
    small.reset(); store.reset(); check(!fs::exists(published) && staging(root.path)==0,"session cleanup only removes owned staging");
    check(fs::exists(root.path/"source/世界 🚀.bin"),"source files preserved after session");
}
}
int main() {
    try {uris(); files(); std::cout<<"PASS: descriptor-relative file confinement, symlink/FIFO rejection, pinned reads, mutation detection, out-of-order disk writes, non-executable atomic publication and quota/abort/session cleanup\n"; return 0;}
    catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;}
}
