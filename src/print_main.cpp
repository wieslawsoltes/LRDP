#include "lrdp/printing/native.hpp"
#include <charconv>
#include <iostream>
#include <limits>
using namespace lrdp;
namespace {
std::uint64_t number(std::string_view value) {
    std::uint64_t result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    require(!value.empty() && parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size(),"invalid printer identity");return result;
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--help") {
            std::cout<<"Usage: lrdp-print list /private/path/print.sock\n"
                     <<"       lrdp-print submit /private/path/print.sock DEVICE:GENERATION printer-ready-file\n"
                     <<"Raw printer-ready data only. No rendering, automatic retry or physical completion guarantee.\n";return 0;
        }
        require(argc>=3,"use lrdp-print --help");const std::string_view command=argv[1];
        if(command=="list") {
            require(argc==3,"list requires only a socket path");
            for(const auto& device:printing::list_printers(argv[2]))std::cout<<device.key.id<<':'<<device.key.generation<<'\t'<<device.printer->name<<'\t'<<device.printer->driver<<'\n';
        } else {
            require(command=="submit" && argc==5,"submit requires socket, device:generation, and source file");
            const std::string_view key=argv[3];const auto separator=key.find(':');require(separator!=std::string_view::npos,"missing printer generation");
            const auto id=number(key.substr(0,separator)),generation=number(key.substr(separator+1));require(id<=std::numeric_limits<std::uint32_t>::max() && generation,"invalid printer generation");
            const auto result=printing::submit_file(argv[2],{std::uint32_t(id),generation},argv[4]);
            if(result.status){std::cerr<<"Print failed: status=0x"<<std::hex<<result.status<<std::dec<<", acknowledged_bytes="<<result.transferred<<"; partial printing is possible, do not retry automatically\n";return 1;}
            std::cout<<"Remote spool closed successfully; acknowledged_bytes="<<result.transferred<<" (not a physical page-completion guarantee)\n";
        }
        return 0;
    }catch(const std::exception& error){std::cerr<<"lrdp-print: "<<error.what()<<'\n';return 1;}
}
