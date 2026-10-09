#include "lrdp/drive/protocol.hpp"
#include <algorithm>
#include <limits>
namespace lrdp::drive {
namespace {
unsigned major(Operation op) {
    switch (op) {
    case Operation::open:return 0; case Operation::close:return 2; case Operation::read:return 3; case Operation::write:return 4;
    case Operation::query_information:return 5; case Operation::set_information:return 6;
    case Operation::query_volume:return 10; case Operation::query_directory:return 12;
    }
    throw ProtocolError("unknown drive operation");
}
constexpr std::uint32_t mutation_access = 0x500d0116; // generic all/write, DELETE, WRITE_DAC/OWNER, write data/EA/attributes.
}
Protocol::Protocol(Limits limits) : limits_(limits) {
    require(limits.devices && limits.devices <= 64 && limits.handles && limits.handles <= 4096 &&
        limits.outstanding && limits.outstanding <= 256 && limits.transfer && limits.transfer <= 1024*1024 &&
        limits.timeout >= std::chrono::seconds(1) && limits.timeout <= std::chrono::minutes(5), "invalid drive policy");
    for (unsigned i=1; i<=limits.outstanding; ++i) free_ids_.push_back(i);
}
void Protocol::emit(unsigned kind,View body) {
    require(outbound_.size() < 512, "drive output queue must be drained"); outbound_.push_back(pdu(kind,body));
}
void Protocol::start(std::uint32_t id) {
    require(phase_ == Phase::idle,"drive protocol already started"); client_id_=id;
    Writer body; body.le16(1).le16(13).le32(id); emit(0x496e,body.bytes()); phase_=Phase::announce;
}
void Protocol::receive(View message) {
    require(message.size() <= std::size_t(limits_.transfer)+1024*1024,"drive message exceeds policy");
    Reader in(message); require(in.le16()==0x4472,"unnegotiated device-redirection component"); const auto kind=in.le16();
    if(kind==0x4343) {
        require(phase_==Phase::announce && in.le16()==1,"unexpected client announce");
        minor_=in.le16(); require(minor_==10 || minor_==12 || minor_==13,"unsupported RDPDR minor version");
        client_id_=in.le32(); in.end(); phase_=Phase::name;
    } else if(kind==0x434e) {
        require(phase_==Phase::name,"client name out of sequence"); const auto unicode=in.le32(); require(in.le32()==0,"unsupported client-name code page");
        const auto size=in.le32(); require(size && size<=4096,"client name exceeds policy"); auto text=in.take(size); in.end();
        if(unicode&1) {
            // MS-RDPEFS 2.2.2.4 defines a terminated, informational computer name.
            // Some clients include extra zero code units in ComputerNameLen.
            // Normalize only a zero suffix here, never paths or authenticated names.
            require(text.size()%2==0,"odd client computer-name length");
            while(text.size()>2 && text[text.size()-1]==0 && text[text.size()-2]==0 &&
                  text[text.size()-3]==0 && text[text.size()-4]==0) text=text.first(text.size()-2);
            (void)from_utf16le(text);
        } else { require(text.back()==0,"unterminated client name"); }
        Writer caps; caps.le16(1U + unsigned(limits_.files) + unsigned(limits_.printers)).le16(0);
        caps.le16(1).le16(44).le32(2).le32(0).le32(0).le16(1).le16(minor_)
            .le32(0x3fff).le32(0).le32(7).le32(0).le32(0).le32(0);
        if(limits_.files) caps.le16(4).le16(8).le32(2);
        if(limits_.printers) caps.le16(2).le16(8).le32(1);
        emit(0x5350,caps.bytes());
        Writer confirmed; confirmed.le16(1).le16(minor_).le32(client_id_); emit(0x4343,confirmed.bytes()); phase_=Phase::capabilities;
    } else if(kind==0x4350) capabilities(in);
    else {
        require(ready(),"device traffic before capability exchange");
        if(kind==0x4441) announce_devices(in);
        else if(kind==0x444d) remove_devices(in);
        else if(kind==0x4943) complete(in);
        else throw ProtocolError("unexpected client drive packet");
    }
}
void Protocol::capabilities(Reader& in) {
    require(phase_==Phase::capabilities,"duplicate or premature drive capabilities");
    const auto count=in.le16(); in.skip(2); require(count<=64,"too many drive capabilities");
    std::set<unsigned> seen; bool general=false,drives=false,printers=false; std::uint32_t extended=0;
    for(unsigned i=0;i<count;++i) {
        const auto type=in.le16(),length=in.le16(); const auto version=in.le32();
        require(length>=8 && seen.insert(type).second,"invalid or duplicate drive capability"); Reader cap(in.take(length-8));
        if(type==1) {
            require((version==1 && length==40)||(version==2 && length==44),"invalid general drive capability");
            cap.skip(8); require(cap.le16()==1,"unsupported general drive version"); (void)cap.le16(); cap.skip(8);
            extended=cap.le32(); cap.skip(8); if(version==2) cap.skip(4); cap.end(); general=true;
        } else if(type==2) {
            require(length==8 && version==1,"invalid printer capability"); printers=true;
        } else if(type==4) {
            require(length==8 && (version==1||version==2),"invalid drive capability"); drives=true;
        }
    }
    in.end(); require(general,"missing general device capability"); extended_=extended; client_drives_=drives && limits_.files; client_printers_=printers && limits_.printers; phase_=Phase::active;
    if(extended_ & 4) emit(0x554c);
}
void Protocol::announce_devices(Reader& in) {
    const auto count=in.le32(); require(count<=64,"device announcement count exceeds policy");
    std::vector<std::pair<std::uint32_t,std::uint32_t>> replies;
    std::vector<Device> accepted; std::set<std::uint32_t> batch;
    unsigned printer_count=0,defaults=0;
    for(const auto& [id,d]:devices_) { (void)id; if(d.printer){++printer_count; defaults+=unsigned((d.printer->flags&2)!=0);} }
    for(unsigned i=0;i<count;++i) {
        const auto type=in.le32(),id=in.le32(); const auto name=in.take(8); const auto bytes=in.le32();
        require(bytes<=65536,"device data exceeds policy"); const auto data=in.take(bytes);
        require(batch.insert(id).second && !devices_.contains(id),"duplicate live device ID");
        const auto zero=std::find(name.begin(),name.end(),0);
        bool valid=zero!=name.end() && zero!=name.begin();
        std::string label(name.begin(),zero);
        for(std::size_t n=0;n<label.size();++n) {
            const auto c=static_cast<unsigned char>(label[n]);
            valid &= c>=32 && c<127 && std::string_view("<>\"/\\|").find(char(c))==std::string_view::npos && (c!=':'||n+1==label.size());
        }
        std::uint32_t status=success;
        if(type==4 && client_printers_) {
            auto printer=printer_information(data);
            valid &= label.size()>3 && label.starts_with("PRN") &&
                std::all_of(label.begin()+3,label.end(),[](unsigned char c){return c>='0' && c<='9';});
            if(!valid || devices_.size()+accepted.size()>=limits_.devices || printer_count>=16 || ((printer.flags&2) && defaults)) status=denied;
            else { ++printer_count; defaults+=unsigned((printer.flags&2)!=0); accepted.push_back({{id,0},std::move(label),std::move(printer)}); }
        } else if(type!=8 || !client_drives_) status=unsupported;
        else if(!valid || devices_.size()+accepted.size()>=limits_.devices) status=denied;
        else accepted.push_back({{id,0},std::move(label),{}});
        replies.emplace_back(id,status);
    }
    in.end();
    require(next_generation_ <= std::numeric_limits<std::uint64_t>::max()-accepted.size(),"device generations exhausted");
    for(auto& device:accepted) { device.key.generation=next_generation_++; devices_.emplace(device.key.id,std::move(device)); }
    for(auto [id,status]:replies) { Writer w;w.le32(id).le32(status);emit(0x6472,w.bytes()); }
}
void Protocol::remove_devices(Reader& in) {
    require(extended_&1,"device removal was not negotiated"); const auto count=in.le32(); require(count<=64,"device removal exceeds policy");
    std::set<std::uint32_t> ids;
    for(unsigned i=0;i<count;++i) require(ids.insert(in.le32()).second,"duplicate removed device");
    in.end();
    for(auto id:ids) {
        // Removal of an explicitly rejected unsupported device is harmless.
        auto device=devices_.find(id); if(device==devices_.end())continue;
        const auto key=device->second.key; devices_.erase(device);
        std::erase_if(handles_,[&](const auto& item){return item.second.device==key;});
        for(auto& [completion,p]:pending_) if(!p.abandoned && p.request.device==key) {
            (void)completion; replies_.push_back({p.request.ticket,removed,0,0,{},p.request.purpose}); p.abandoned=true;
            // Keep the completion ID reserved until its old reply arrives. Reused
            // device IDs must never authorize an old handle or late completion.
        }
    }
}
bool Protocol::submit(const Request& request,Clock::time_point now) {
    require(ready(),"drive operation before initialization");
    require(request.purpose==Purpose::filesystem || request.purpose==Purpose::printer,"invalid device request purpose");
    require(request.ticket && !tickets_.contains({request.purpose,request.ticket}),"duplicate drive request ticket");
    require(replies_.size()<limits_.outstanding,"drive results must be drained");
    auto immediate=[&](std::uint32_t status){ replies_.push_back({request.ticket,status,0,0,{},request.purpose});return true; };
    const auto device=devices_.find(request.device.id);
    if(device==devices_.end() || device->second.key!=request.device)return immediate(removed);
    const bool printer=request.purpose==Purpose::printer;
    if(printer!=device->second.printer.has_value()) return immediate(denied);
    if(printer && request.operation!=Operation::open && request.operation!=Operation::write && request.operation!=Operation::close) return immediate(unsupported);
    std::uint32_t remote=0;
    const RemoteHandle* handle=nullptr;
    if(request.operation!=Operation::open) {
        const auto it=handles_.find(request.handle);
        if(it==handles_.end() || it->second.device!=request.device)return immediate(invalid_handle);
        handle=&it->second;remote=handle->file;
        for(const auto& [id,p]:pending_) {
            (void)id;
            if(!p.abandoned && p.request.operation!=Operation::open && p.request.handle==request.handle)return false;
        }
    } else if(handles_.size()+pending_.size()>=limits_.handles)return false;
    const bool changes=request.operation==Operation::write || request.operation==Operation::set_information ||
        (request.operation==Operation::open && (request.disposition!=1 || (request.access & mutation_access)));
    if(!printer && changes && !limits_.writable)return immediate(denied);
    if(!printer && request.operation==Operation::write && !(handle->access & (0x50000006U)))return immediate(denied);
    if(request.operation==Operation::set_information && !(handle->access & mutation_access))return immediate(denied);
    if(request.operation==Operation::query_directory && !handle->directory)return immediate(0xc0000103);
    if(free_ids_.empty())return false;
    const auto body=request_body(request,limits_.transfer); const auto id=free_ids_.front();
    Writer w;w.le32(request.device.id).le32(remote).le32(id).le32(major(request.operation))
        .le32(request.operation==Operation::query_directory ? 1 : 0).raw(body);
    emit(0x4952,w.bytes()); pending_.emplace(id,Pending{request,now+limits_.timeout});tickets_.insert({request.purpose,request.ticket});free_ids_.pop_front();return true;
}
void Protocol::complete(Reader& in) {
    const auto device=in.le32(),id=in.le32(),status=in.le32(); auto found=pending_.find(id);
    require(found!=pending_.end() && found->second.request.device.id==device,"uncorrelated drive I/O completion");
    auto& pending=found->second;const auto& request=pending.request;
    Reply reply{request.ticket,status,0,0,{},request.purpose};
    if(status!=success) {
        require(in.remaining()<=16,"error completion has unexpected payload"); in.skip(in.remaining());
    } else if(request.operation==Operation::open) {
        const auto remote=in.le32(); if(!in.empty())in.skip(1); in.end();
        if(!pending.abandoned) {
            require(next_handle_!=std::numeric_limits<std::uint64_t>::max(),"local file handles exhausted");
            for(const auto& [cookie,h]:handles_) { (void)cookie;require(h.device!=request.device || h.file!=remote,"client reused a live file ID"); }
            reply.handle=next_handle_++; handles_.emplace(reply.handle,RemoteHandle{request.device,remote,request.access,(request.options&1)!=0});
        }
    } else if(request.operation==Operation::close) {
        require(in.remaining()==4 || in.remaining()==5,"invalid drive close completion");in.skip(in.remaining());handles_.erase(request.handle);
    } else if(request.operation==Operation::write || request.operation==Operation::set_information) {
        reply.transferred=in.le32(); if(!in.empty())in.skip(1);in.end();
        if(request.operation==Operation::write)require(reply.transferred<=request.length,"client wrote beyond requested range");
        else require(reply.transferred==request.data.size(),"set information completion size mismatch");
    } else {
        const auto length=in.le32();require(length<=limits_.transfer,"drive response data exceeds policy");
        const auto data=in.take(length);if(request.operation!=Operation::read && !in.empty())in.skip(1);in.end();
        if(request.operation==Operation::read)require(length<=request.length,"drive read exceeds requested length");
        else if(request.operation==Operation::query_directory)(void)directory_information(data);
        else if(request.operation==Operation::query_information) {
            if(request.information==4)(void)basic_information(data);
            else if(request.information==5){FileInfo info;standard_information(info,data);}
            else require(length==8,"invalid attribute-tag information");
        } else if(request.operation==Operation::query_volume)require(length==(request.information==3?24U:32U),"invalid volume-size information");
        reply.transferred=length;reply.data.assign(data.begin(),data.end());
    }
    if(!pending.abandoned) {require(replies_.size()<limits_.outstanding,"drive results must be drained");replies_.push_back(std::move(reply));}
    tickets_.erase({request.purpose,request.ticket});pending_.erase(found);free_ids_.push_back(id);
}
void Protocol::tick(Clock::time_point now) const {
    for(const auto& [id,p]:pending_) {(void)id;require(now<p.deadline,"redirected device I/O timed out; connection must close to retire remote handles");}
}
std::vector<Device> Protocol::devices() const {std::vector<Device> result;for(const auto& [id,d]:devices_){(void)id;result.push_back(d);}return result;}
std::vector<Bytes> Protocol::drain(){std::vector<Bytes> result;result.swap(outbound_);return result;}
std::vector<Reply> Protocol::take_replies(){std::vector<Reply> result;result.swap(replies_);return result;}
} // namespace lrdp::drive
