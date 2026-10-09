#include "lrdp/session/registry.hpp"
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <algorithm>

namespace lrdp::persistent {
namespace {
ReconnectCookie random_cookie(std::uint32_t id) {
    ReconnectCookie c; c.logon_id=id;
    require(RAND_bytes(c.bytes.data(), int(c.bytes.size())) == 1, "reconnect randomness unavailable"); return c;
}
void principal_valid(std::string_view name) {
    require(!name.empty() && name.size() <= 1024, "invalid authenticated principal");
    for (const unsigned char c : name) require(c >= 32 && c != 127, "invalid authenticated principal");
    (void)utf16le(name); // Validate UTF-8, preserving exact domain, case and realm.
}
}
ReconnectCookie enhanced_verifier(const ReconnectCookie& server) {
    ReconnectCookie result; result.logon_id=server.logon_id;
    const std::array<unsigned char,32> client_random{}; unsigned count=0;
    require(HMAC(EVP_md5(),server.bytes.data(),int(server.bytes.size()),client_random.data(),client_random.size(),
                 result.bytes.data(),&count) && count==16,"protocol HMAC-MD5 unavailable (no fallback)"); return result;
}
Registry::Record::~Record() {
    OPENSSL_cleanse(current.bytes.data(),current.bytes.size()); OPENSSL_cleanse(candidate.bytes.data(),candidate.bytes.size());
}
Registry::Registry(std::size_t capacity,std::chrono::seconds retention):capacity_(capacity),retention_(retention) {
    require(capacity>0 && capacity<=64 && retention.count()>0 && retention<=std::chrono::hours(24),"invalid persistent-session limits");
}
Registry::Record& Registry::owned(std::uint64_t owner) {
    require(owner!=0,"invalid lease owner");
    for(auto& [id,record]:records_) { (void)id; if(record.owner==owner)return record; }
    throw ProtocolError("lease is not owned by this connection");
}
Lease Registry::acquire(std::string principal,const std::optional<ReconnectCookie>& verifier,std::uint64_t owner,Time now) {
    principal_valid(principal); require(owner!=0,"invalid lease owner");
    for(const auto& [id,r]:records_) { (void)id; require(r.owner!=owner,"connection already owns a lease"); }
    if(verifier) {
        auto it=records_.find(verifier->logon_id);
        require(it!=records_.end(),"session cannot be resumed"); auto& r=it->second;
        auto expected=enhanced_verifier(r.current);
        const bool valid=CRYPTO_memcmp(expected.bytes.data(),verifier->bytes.data(),16)==0;
        OPENSSL_cleanse(expected.bytes.data(),expected.bytes.size());
        require(valid && r.principal==principal && r.retained && !r.owner && now<r.expires,"session cannot be resumed");
        auto next=random_cookie(it->first); r.candidate=next; r.owner=owner; r.committed=false;
        OPENSSL_cleanse(next.bytes.data(),next.bytes.size()); return {it->first,false};
    }
    require(records_.size()<capacity_,"persistent desktop capacity reached");
    std::uint32_t id=0;
    for(unsigned attempts=0;attempts<32;++attempts) {
        require(RAND_bytes(reinterpret_cast<unsigned char*>(&id),sizeof(id))==1,"session identifier randomness unavailable");
        if(id && !records_.contains(id))break;
        id=0;
    }
    require(id!=0,"cannot allocate unique session identifier");
    auto candidate=random_cookie(id);
    auto [it,inserted]=records_.try_emplace(id);require(inserted,"session ID collision");
    auto& r=it->second;r.principal=std::move(principal);r.candidate=candidate;r.owner=owner;r.expires=now+retention_;
    OPENSSL_cleanse(candidate.bytes.data(),candidate.bytes.size()); return {id,true};
}
std::optional<ReconnectCookie> Registry::commit(std::uint64_t owner,bool retain,Time now) {
    auto& r=owned(owner);require(!r.committed,"lease already committed");
    r.committed=true; r.retained=retain;
    if(!retain)return {};
    r.current=r.candidate;OPENSSL_cleanse(r.candidate.bytes.data(),r.candidate.bytes.size());r.rotated=now;return r.current;
}
ReconnectCookie Registry::rotate(std::uint64_t owner,Time now) {
    auto& r=owned(owner);require(r.retained && r.committed,"uncommitted reconnect rotation");
    require(now-r.rotated>=std::chrono::hours(1),"reconnect cookie rotation is premature");
    r.current=random_cookie(r.current.logon_id);r.rotated=now;return r.current;
}
std::optional<std::uint32_t> Registry::release(std::uint64_t owner,Time now) {
    for(auto it=records_.begin();it!=records_.end();++it) {
        auto& r=it->second;if(r.owner!=owner)continue;
        if(!r.retained) { const auto id=it->first;records_.erase(it);return id; }
        if(r.committed)r.expires=now+retention_;
        r.owner=0;r.committed=false;OPENSSL_cleanse(r.candidate.bytes.data(),r.candidate.bytes.size());return {};
    }
    return {};
}
std::vector<std::uint32_t> Registry::expire(Time now) {
    std::vector<std::uint32_t> removed;
    for(auto it=records_.begin();it!=records_.end();) {
        if(!it->second.owner && now>=it->second.expires) { removed.push_back(it->first);it=records_.erase(it); }
        else ++it;
    }
    return removed;
}
bool Registry::erase(std::uint32_t id) {return records_.erase(id)!=0;}
std::vector<Summary> Registry::list() const {
    std::vector<Summary> out;for(const auto& [id,r]:records_)out.push_back({id,r.owner!=0,r.principal});return out;
}
} // namespace lrdp::persistent
