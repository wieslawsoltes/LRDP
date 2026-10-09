#include "lrdp/session/broker.hpp"
#include <charconv>
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t running=1;
void stop(int){running=0;}
unsigned number(std::string_view s,unsigned max){unsigned n=0;auto r=std::from_chars(s.data(),s.data()+s.size(),n);lrdp::require(r.ec==std::errc{} && r.ptr==s.data()+s.size() && n>0 && n<=max,"invalid numeric option");return n;}
}
int main(int argc,char** argv){
    using namespace lrdp;
    try{
        std::string directory;HeadlessOptions options;unsigned capacity=4,seconds=1800;
        for(int i=1;i<argc;++i){
            const std::string_view option=argv[i];
            auto value=[&]()->std::string{require(i+1<argc,"missing option value");return argv[++i];};
            if(option=="--directory")directory=value();
            else if(option=="--max-desktops")capacity=number(value(),64);
            else if(option=="--retain-seconds")seconds=number(value(),86400);
            else if(option=="--desktop-command")options.command={value()};
            else if(option=="--desktop-arg")options.command.push_back(value());
            else if(option=="--xorg-executable")options.xorg=value();
            else if(option=="--help"){
                std::cout<<"lrdp-sessiond --directory /private/0700/directory [--retain-seconds 1800] [--max-desktops 4]\n"
                         <<"  [--desktop-command /usr/bin/xterm] [--desktop-arg ARG] [--xorg-executable /usr/lib/xorg/Xorg]\n"
                         <<"Rootless per-Unix-account desktop retention. No PAM impersonation or broker-restart recovery.\n";return 0;
            }else throw ProtocolError("unknown session broker option");
        }
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);std::signal(SIGPIPE,SIG_IGN);
        persistent::Broker broker(directory,options,capacity,std::chrono::seconds(seconds));
        std::cout<<"LRDP session broker: "<<broker.socket_path()<<'\n'<<std::flush;
        while(running)broker.poll(100);return 0;
    }catch(const std::exception& e){std::cerr<<"LRDP session broker: "<<e.what()<<'\n';return 1;}
}
