#include "lrdp/printing/native.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>

using namespace lrdp;
namespace {
unsigned checks=0;
void check(bool value,const char* text){++checks;require(value,text);}
template<class F>void rejects(F f){++checks;try{f();}catch(const ProtocolError&){return;}throw std::runtime_error("unsafe print operation accepted");}
struct Directory {
    std::string path;
    Directory(){char name[]="/tmp/lrdp-print-test-XXXXXX";auto* p=mkdtemp(name);require(p!=nullptr,"temporary directory");path=p;}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void write_file(const std::string& path,View bytes){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));require(bool(f),"test file write");}
std::size_t fds(){return std::size_t(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{}));}
}
int main(){try{
    Directory root;const auto input=root.path+"/input";Bytes bytes(180003);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=std::uint8_t(i*17+3);write_file(input,bytes);
    auto snapshot=printing::snapshot_file(input);check((fcntl(snapshot.get(),F_GET_SEALS)&15)==15,"all source immutability seals");
    check(write(snapshot.get(),bytes.data(),1)<0 && errno==EPERM,"sealed source accepts a write");
    check(ftruncate(snapshot.get(),1)<0 && errno==EPERM,"sealed source accepts truncation");
    auto source=printing::sealed_source(std::move(snapshot));write_file(input,Bytes{42});
    check(source->read(65530,100)==Bytes(bytes.begin()+65530,bytes.begin()+65630),"snapshot survives input mutation");
    rejects([&]{source->read(bytes.size(),1);});rejects([&]{source->read(0,65537);});
    {UniqueFd plain(memfd_create("unsealed",MFD_CLOEXEC|MFD_ALLOW_SEALING));check(ftruncate(plain.get(),10)==0,"unsealed fixture");rejects([&]{printing::sealed_source(std::move(plain));});}
    std::filesystem::create_symlink(input,root.path+"/link");rejects([&]{printing::snapshot_file(root.path+"/link");});
    check(mkfifo((root.path+"/fifo").c_str(),0600)==0,"FIFO fixture");rejects([&]{printing::snapshot_file(root.path+"/fifo");});
    rejects([&]{printing::snapshot_file(root.path);});write_file(input,{});rejects([&]{printing::snapshot_file(input);});
    {UniqueFd large(open(input.c_str(),O_WRONLY));check(ftruncate(large.get(),off_t(printing::Jobs::byte_limit+1))==0,"oversized fixture");}
    rejects([&]{printing::snapshot_file(input);});write_file(input,bytes);
    const auto baseline=fds();std::string endpoint_path;
    {
        printing::NativeEndpoint endpoint(root.path);endpoint_path=endpoint.path();
        drive::Device printer{{9,1},"PRN1",drive::PrinterInfo{0,"PostScript","Printer 日本語"}};endpoint.publish({printer});
        struct stat st{};check(lstat(endpoint_path.c_str(),&st)==0 && (st.st_mode&0777)==0600,"owner-only socket mode");
        Bytes received;unsigned opens=0,closes=0;
        auto wait=[&](auto& future){
            const auto deadline=drive::Clock::now()+std::chrono::seconds(5);
            while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){
                endpoint.pump();for(const auto& request:endpoint.take_requests(65536)){
                    drive::Reply reply;reply.ticket=request.ticket;reply.purpose=drive::Purpose::printer;
                    if(request.operation==drive::Operation::open){++opens;reply.handle=1;}
                    else if(request.operation==drive::Operation::write){reply.transferred=std::min<unsigned>(request.length,7333);received.insert(received.end(),request.data.begin(),request.data.begin()+reply.transferred);}
                    else{check(request.operation==drive::Operation::close,"unsupported native print operation");++closes;}
                    endpoint.complete(reply);
                }
                require(drive::Clock::now()<deadline,"native printer test deadline");std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return future.get();
        };
        auto listed=std::async(std::launch::async,[&]{return printing::list_printers(endpoint.path());});
        const auto devices=wait(listed);check(devices.size()==1 && devices[0].key==printer.key && devices[0].printer->name==printer.printer->name,"native list generation/name");
        auto printed=std::async(std::launch::async,[&]{return printing::submit_file(endpoint.path(),printer.key,input);});
        const auto result=wait(printed);check(!result.status && result.transferred==bytes.size() && received==bytes && opens==1 && closes==1,"native short-write job bytes and close-gated success");
        auto stale=std::async(std::launch::async,[&]{return printing::submit_file(endpoint.path(),{9,99},input);});
        check(wait(stale).status!=0 && opens==1,"stale generation cannot create a remote job");
        // A disconnected endpoint can outlive Session but must release sealed data immediately.
        auto abandoned=std::async(std::launch::async,[&]{return printing::submit_file(endpoint.path(),printer.key,input);});
        const auto deadline=drive::Clock::now()+std::chrono::seconds(5);bool queued=false;
        while(!queued){endpoint.pump();queued=!endpoint.take_requests(65536).empty();
            require(drive::Clock::now()<deadline,"abandoned job deadline");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        endpoint.disconnect();rejects([&]{(void)abandoned.get();});
        check(fds()==baseline,"disconnect must release job descriptors even before endpoint destruction");
        check(endpoint.take_requests(65536).empty(),"disconnected endpoint cannot send old jobs");
        check(!std::filesystem::exists(endpoint_path),"orderly endpoint unlink");
    }
    check(fds()==baseline,"native printer descriptor leak");
    check(chmod(root.path.c_str(),0755)==0,"unsafe-root fixture");rejects([&]{printing::NativeEndpoint endpoint(root.path);});
    std::cout<<"PASS: "<<checks<<" native printer checks; sealed sources, partial writes, exact-generation selection, socket permissions, unsafe sources, descriptor and directory cleanup\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
