#include "lrdp/drive/filesystem.hpp"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
using namespace lrdp;
using namespace lrdp::drive;
namespace {
unsigned checks = 0;
void check(bool v, const char* text) { ++checks; if(!v) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { ++checks; try { fn(); } catch(const ProtocolError&) { return; } throw std::runtime_error("expected malformed-volume rejection"); }
template<class F> void ioerror(F fn, int code) { ++checks; try { fn(); } catch(const IoError& e) { check(e.code().value()==code,"expected native error"); return; } throw std::runtime_error("expected I/O error"); }
Bytes record(unsigned kind, std::uint64_t total, std::uint64_t caller, std::uint64_t actual,
             std::uint32_t sectors = 8, std::uint32_t bytes = 512) {
    Writer out; write_u64(out,total); write_u64(out,caller); if(kind==7) write_u64(out,actual); out.le32(sectors).le32(bytes); return out.bytes();
}
class Peer final : public Client {
public:
    Limits policy; std::vector<Request> calls; std::uint64_t generation=5;
    Bytes full=record(7,1ULL<<32,123456,1ULL<<30), simple=record(3,1ULL<<33,777,777);
    std::uint32_t full_status=0, size_status=0; bool invalidate=false; unsigned live=0;
    Reply call(Request r) override {
        calls.push_back(r); Reply reply; reply.ticket=r.ticket;
        check(r.device==DeviceKey{9,generation},"current device generation attached to every request");
        if(r.operation==Operation::open) {
            check(r.access==0x80 && r.disposition==1 && (r.options&0x200000),"capacity query never requests write rights"); ++live; reply.handle=123;
        } else if(r.operation==Operation::close) { check(live==1,"one capacity handle"); --live; }
        else if(r.operation==Operation::query_information) {
            Writer out;
            if(r.information==4) out.zeros(32).le32(0x10);
            else { write_u64(out,0);write_u64(out,0);out.le32(1).u8(0).u8(1); }
            reply.data=out.bytes();
        } else {
            check(r.operation==Operation::query_volume && r.handle==123,"volume query uses validated handle");
            reply.status = r.information==7?full_status:size_status;
            reply.data = r.information==7?full:simple;
            if(reply.status) reply.data.clear();
            if(invalidate) throw IoError(ENODEV,"fixture removed drive");
        }
        return reply;
    }
    std::vector<Device> devices() const override { return {{{9,generation},"DATA"}}; }
    const Limits& limits() const override { return policy; }
    unsigned queries(unsigned kind) const { unsigned n=0;for(const auto& r:calls) n+=r.operation==Operation::query_volume&&r.information==kind;return n; }
};
void tests() {
    for(unsigned kind:{3U,7U}) {
        const auto bytes=record(kind,1ULL<<33,1ULL<<31,1ULL<<32);
        const auto parsed=volume_space(bytes,kind);
        check(parsed.unit_bytes==4096 && parsed.total_units==(1ULL<<33) && parsed.quota_aware==(kind==7),"64-bit capacity and geometry");
        for(std::size_t n=0;n<bytes.size();++n) rejects([&]{(void)volume_space(View(bytes).first(n),kind);});
        auto trailing=bytes; trailing.push_back(0); rejects([&]{(void)volume_space(trailing,kind);});
        for(unsigned field=0;field<(kind==7?3U:2U);++field) {
            auto negative=bytes; negative[field*8+7]|=0x80;rejects([&]{(void)volume_space(negative,kind);});
        }
    }
    rejects([]{(void)volume_space(record(7,10,9,9,0,512),7);});
    rejects([]{(void)volume_space(record(7,10,9,9,8,0),7);});
    rejects([]{(void)volume_space(record(7,1ULL<<62,1,1),7);});
    rejects([]{(void)volume_space(record(7,0,0,1ULL<<62),7);});
    rejects([]{(void)volume_space(record(3,0,0,0),8);});
    const auto quota=volume_space(record(7,1000,70,999999),7);struct statvfs native{};
    native_statvfs(quota,false,native);
    check(native.f_blocks==1000 && native.f_bfree==1000 && native.f_bavail==70 && (native.f_flag&ST_RDONLY),"quota does not expose physical capacity as user allowance");
    native_statvfs(volume_space(record(7,100,200,12),7),true,native);
    check(native.f_bfree==12 && native.f_bavail==12 && !(native.f_flag&ST_RDONLY),"conservative inconsistent-counter normalization");
    const auto before=native; auto invalid=quota;invalid.unit_bytes=0;
    rejects([&]{native_statvfs(invalid,false,native);});
    check(std::memcmp(&before,&native,sizeof(native))==0,"failed conversion leaves output untouched");
    // libfuse serializes bsize/frsize as uint32 even on a 64-bit host.
    // Valid 64-bit RDP geometry must not be silently narrowed on that boundary.
    auto oversized_unit=volume_space(record(7,1,1,1,1U<<30,4),7);
    ioerror([&]{native_statvfs(oversized_unit,false,native);},EOVERFLOW);
    check(std::memcmp(&before,&native,sizeof(native))==0,"FUSE geometry overflow leaves output untouched");
    Peer peer;Filesystem fs(peer);
    const auto path="/"+device_name(peer.devices()[0]);
    check(fs.space("/").total_units==0 && peer.calls.empty(),"virtual root is not a duplicated sum of client volumes");
    check(fs.space(path).total_units==(1ULL<<32) && peer.queries(7)==1 && peer.queries(3)==0 && peer.live==0,"full-size preferred and handle closed");
    for(const auto status:{unsupported,0xc0000003U,0xc0000010U}) {
        peer.calls.clear();peer.full_status=status;
        check(!fs.space(path).quota_aware && peer.queries(7)==1 && peer.queries(3)==1 && peer.live==0,"unsupported class falls back once");
    }
    peer.calls.clear();peer.full_status=denied;
    ioerror([&]{(void)fs.space(path);},EACCES);check(peer.queries(3)==0 && peer.live==0,"access denial not hidden by fallback");
    peer.full_status=unsupported;peer.size_status=unsupported;
    ioerror([&]{(void)fs.space(path);},EOPNOTSUPP);check(peer.live==0,"unsupported size queries close handle");
    peer.calls.clear();peer.full_status=0;peer.size_status=0;peer.full.pop_back();
    rejects([&]{(void)fs.space(path);}); check(peer.queries(3)==0 && peer.live==0,"malformed success is not silently downgraded");
    peer.full=record(7,100,20,30);peer.calls.clear();peer.invalidate=true;
    ioerror([&]{(void)fs.space(path);},ENODEV);check(peer.live==0,"disconnect releases facade handle");peer.invalidate=false;
    ++peer.generation;peer.calls.clear();ioerror([&]{(void)fs.space(path);},ENODEV);
    check(peer.calls.empty(),"stale device path cannot use replacement capacity");
    check(fs.space("/"+device_name(peer.devices()[0])).total_units==100,"new generation resolves freshly");
    std::mt19937 random(0x564f4c55);
    for(unsigned i=0;i<20000;++i) {
        Bytes bytes(random()%64);for(auto& b:bytes)b=std::uint8_t(random());
        try {const auto s=volume_space(bytes,i&1?3:7);native_statvfs(s,false,native);}
        catch(const ProtocolError&) {} catch(const IoError&) {}
    }
}
}
int main(){try{tests();std::cout<<checks<<" checks passed plus 20000 malformed-volume probes: quota-aware capacity, fallback, lifecycle and checked statvfs\n";}
catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
