#include "lrdp/printing/jobs.hpp"
#include <algorithm>
#include <limits>

namespace lrdp::printing {
void Jobs::publish(std::vector<drive::Device> devices) {
    std::erase_if(devices,[](const auto& d){return !d.printer;});
    require(devices.size()<=16,"native printer device quota"); devices_=std::move(devices);
}
void Jobs::submit(std::uint64_t cookie, drive::DeviceKey device, std::shared_ptr<const Source> source) {
    require(cookie && !jobs_.contains(cookie) && jobs_.size()<job_limit && results_.empty(),"print job queue is full or cookie reused");
    require(source && source->size()>0 && source->size()<=byte_limit-held_bytes_,"print job byte quota");
    require(std::any_of(devices_.begin(),devices_.end(),[&](const auto& d){return d.key==device;}),"printer generation unavailable");
    Job job; job.cookie=cookie; job.device=device; job.source=std::move(source);
    job.deadline=drive::Clock::now()+std::chrono::minutes(5);
    const auto bytes=job.source->size(); jobs_.emplace(cookie,std::move(job)); held_bytes_+=bytes;
}
void Jobs::cancel(std::uint64_t cookie) {
    const auto found=jobs_.find(cookie); if(found!=jobs_.end()) found->second.status=0xc0000120;
}
void Jobs::finish(std::map<std::uint64_t,Job>::iterator it) {
    const auto& job=it->second;
    require(results_.size()<job_limit,"print results must be drained");
    results_.push_back({job.cookie,job.offset,job.status});
    held_bytes_-=job.source->size(); jobs_.erase(it);
}
std::vector<drive::Request> Jobs::take_requests(std::uint32_t chunk) {
    require(chunk>0 && chunk<=65536,"invalid printer chunk policy");
    std::vector<drive::Request> requests;
    std::set<drive::DeviceKey> busy;
    for(auto it=jobs_.begin();it!=jobs_.end();) {
        auto& job=it->second;
        // One job per printer; independent printers make progress concurrently.
        if(!busy.insert(job.device).second || job.pending) {++it;continue;}
        if(std::none_of(devices_.begin(),devices_.end(),[&](const auto& d){return d.key==job.device;})) {
            job.status=drive::removed; job.handle=0;
        }
        if(drive::Clock::now()>=job.deadline) job.status=0xc00000b5;
        if(job.status) {
            if(!job.handle) {auto dead=it++;finish(dead);continue;}
            job.state=State::close;
        }
        drive::Request request;
        require(next_ticket_!=std::numeric_limits<std::uint64_t>::max(),"printer tickets exhausted");
        request.ticket=next_ticket_++; request.device=job.device; request.purpose=drive::Purpose::printer;
        request.handle=job.handle;
        if(job.state==State::open) request.operation=drive::Operation::open;
        else if(job.state==State::close) request.operation=drive::Operation::close;
        else {
            request.operation=drive::Operation::write;
            request.length=std::uint32_t(std::min<std::uint64_t>(chunk,job.source->size()-job.offset));
            try {
                request.data=job.source->read(job.offset,request.length);
                require(request.data.size()==request.length,"print snapshot returned a short read");
            } catch(...) {
                job.status=0xc0000185;job.state=State::close;request.operation=drive::Operation::close;
                request.data.clear();request.length=0;
            }
            job.requested=request.length;
        }
        job.pending=request.ticket;requests.push_back(std::move(request));++it;
    }
    return requests;
}
void Jobs::complete(const drive::Reply& reply) {
    require(reply.purpose==drive::Purpose::printer,"filesystem reply sent to printer queue");
    auto found=std::find_if(jobs_.begin(),jobs_.end(),[&](const auto& item){return item.second.pending==reply.ticket;});
    require(found!=jobs_.end() && reply.ticket,"uncorrelated printer job reply");
    auto& job=found->second;job.pending=0;
    if(reply.status && !job.status)job.status=reply.status;
    if(job.state==State::open) {
        if(reply.status) {finish(found);return;}
        require(reply.handle,"printer create returned no local handle");job.handle=reply.handle;job.state=job.status?State::close:State::write;
    } else if(job.state==State::write) {
        require(reply.transferred<=job.requested,"printer acknowledged an excessive byte count");
        if(!reply.status) {
            if(!reply.transferred)job.status=0xc0000185; // No-progress write must never loop or imply success.
            else job.offset+=reply.transferred;
        }
        if(job.status || job.offset==job.source->size())job.state=State::close;
    } else finish(found); // Success is not published before the remote close ACK.
}
std::vector<Result> Jobs::take_results(){std::vector<Result> result;result.swap(results_);return result;}
} // namespace lrdp::printing
