#include "lrdp/session/broker.hpp"
#include <charconv>
#include <iostream>
int main(int argc,char** argv){
    using namespace lrdp;
    try{
        require((argc==4 || argc==5) && std::string_view(argv[1])=="--socket",
            "Usage: lrdp-sessionctl --socket /private/directory/broker.sock list|terminate ID");
        persistent::BrokerClient client(argv[2]);const std::string_view op=argv[3];
        if(op=="list" && argc==4){
            for(const auto& e:client.list())std::cout<<e.id<<'\t'<<(e.attached?"attached":"detached")<<'\t'<<e.principal<<'\n';
        }else if(op=="terminate" && argc==5){
            std::uint32_t id=0;std::string_view s=argv[4];auto r=std::from_chars(s.data(),s.data()+s.size(),id);
            require(r.ec==std::errc{} && r.ptr==s.data()+s.size() && id,"invalid persistent session ID");client.terminate(id);
        }else throw ProtocolError("invalid sessionctl command");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
