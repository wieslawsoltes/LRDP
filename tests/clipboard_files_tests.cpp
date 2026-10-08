#include "lrdp/clipboard.hpp"
#include <algorithm>
#include <iostream>
#include <random>
using namespace lrdp;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F fn) { try { fn(); } catch (const ProtocolError&) { return; } throw std::runtime_error("expected protocol rejection"); }
struct Stored {
    std::vector<ClipboardFile> descriptors;
    std::vector<Bytes> bytes;
    bool finished = false;
    unsigned aborted = 0;
};
class Source final : public ClipboardFileSource {
    std::shared_ptr<Stored> stored_;
public:
    explicit Source(std::shared_ptr<Stored> data):stored_(std::move(data)) {}
    const std::vector<ClipboardFile>& files() const override { return stored_->descriptors; }
    Bytes read(std::size_t i, std::uint64_t offset, std::uint32_t n) const override {
        const auto& bytes = stored_->bytes.at(i); require(offset <= bytes.size() && n <= bytes.size()-offset,"memory range");
        return Bytes(bytes.begin()+std::ptrdiff_t(offset),bytes.begin()+std::ptrdiff_t(offset+n));
    }
};
class Sink final : public ClipboardFileSink {
    std::shared_ptr<Stored> stored_;
public:
    explicit Sink(std::shared_ptr<Stored> data):stored_(std::move(data)) {}
    ~Sink() override { if (!stored_->finished) ++stored_->aborted; }
    void write(std::size_t index, std::uint64_t offset, View bytes) override {
        auto& target=stored_->bytes.at(index); require(offset <= target.size() && bytes.size() <= target.size()-offset,"sink bounds");
        std::copy(bytes.begin(),bytes.end(),target.begin()+std::ptrdiff_t(offset));
    }
    std::vector<std::string> finish() override { stored_->finished=true; return {"/confined/received"}; }
};
class Store final : public ClipboardFileStore {
public:
    std::shared_ptr<Stored> source=std::make_shared<Stored>(), received;
    std::shared_ptr<const ClipboardFileSource> offer(const std::vector<std::string>&) override { return std::make_shared<Source>(source); }
    std::unique_ptr<ClipboardFileSink> receive(const std::vector<ClipboardFile>& descriptors) override {
        received=std::make_shared<Stored>(); received->descriptors=descriptors;
        for(const auto& d:descriptors) received->bytes.emplace_back(std::size_t(d.directory?0:d.size.value()));
        return std::make_unique<Sink>(received);
    }
};
unsigned type(const Bytes& bytes) { Reader r(bytes); return r.le16(); }
void deliver(ClipboardFiles& engine,const Bytes& bytes) {
    Reader r(bytes); const auto t=r.le16(),f=r.le16(); check(r.le32()==r.remaining(),"PDU header length");
    engine.accept(t,f,r.take(r.remaining()));
}
void populate(Store& store) {
    store.source->descriptors={{"folder",true,0,{}},{"folder/zażółć 🚀.bin",false,200001,{}},{"empty",false,0,{}}};
    store.source->bytes={Bytes{},Bytes(200001),Bytes{}};
    for(std::size_t i=0;i<store.source->bytes[1].size();++i) store.source->bytes[1][i]=std::uint8_t(i*31+17);
}
void wire() {
    std::vector<ClipboardFile> files={{"folder",true,0,{}},{"folder/a",false,0x123456,0x0102030405060708ULL}};
    const auto encoded=encode_clipboard_files(files); check(encoded.size()==4+2*592,"descriptor exact size");
    Reader field(View(encoded).subspan(4+592));
    check(field.le32()==0x4064,"descriptor flags");
    for(auto c:field.take(32)) check(!c,"reserved block one");
    check(field.le32()==0x80,"file attributes");
    for(auto c:field.take(16)) check(!c,"reserved block two");
    check(field.le32()==0x05060708 && field.le32()==0x01020304,"FILETIME endian");
    check(field.le32()==0 && field.le32()==0x123456,"file size high then low");
    check(from_utf16le(field.take(18))=="folder\\a","Windows relative filename");
    const auto decoded=decode_clipboard_files(encoded);
    check(decoded[0].directory && decoded[1].name=="folder/a" && decoded[1].modified==files[1].modified,"descriptor decode");
    for(const auto name:{"../x","/root","x//y","x/../y","C:\\x","x:ads","x\\..\\y","NUL.txt","con","a.","a ","a?b","a/"})
        rejects([&]{(void)validate_clipboard_path(name);});
    rejects([]{(void)validate_clipboard_path(std::string("a\0b",3));});
    rejects([]{(void)validate_clipboard_files({{"a/b",false,1,{}}});});
    rejects([]{(void)validate_clipboard_files({{"A",false,1,{}},{"a",false,1,{}}});});
    rejects([]{(void)validate_clipboard_files({{"big",false,1024ULL*1024*1024+1,{}}});});
    rejects([]{(void)validate_clipboard_files({{"unknown",false,{},{}}},{},true);});
    for(std::size_t n=0;n<encoded.size();++n) rejects([&]{(void)decode_clipboard_files(View(encoded).first(n));});
    std::mt19937 random(0x46494c45);
    for(unsigned i=0;i<10000;++i) {
        auto bad=encoded; for(unsigned j=0;j<8;++j) bad[random()%bad.size()]=std::uint8_t(random());
        try {(void)decode_clipboard_files(bad);} catch(const ProtocolError&) {}
    }
}
void transfers() {
    auto a=std::make_shared<Store>(), b=std::make_shared<Store>(); populate(*a);
    ClipboardFiles sender(a), receiver(b); sender.negotiate(0x1e); receiver.negotiate(0x1e);
    sender.publish(sender.offer({"/test"})); receiver.begin_remote();
    auto begin=receiver.drain(); check(begin.size()==1 && type(begin[0])==10,"lock before metadata"); deliver(sender,begin[0]);
    auto descriptors=decode_clipboard_files(sender.file_list()); descriptors[1].size.reset();
    receiver.list(encode_clipboard_files(descriptors));
    // Publishing a new source cannot affect a locked transfer already in progress.
    sender.publish({});
    for(unsigned turn=0;turn<20 && !b->received; ++turn) {
        for(const auto& p:receiver.drain()) deliver(sender,p);
        for(const auto& p:sender.drain()) deliver(receiver,p);
    }
    check(b->received!=nullptr && receiver.pending()<=4,"size query and bounded pipeline");
    for(unsigned turn=0;turn<20 && !b->received->finished;++turn) {
        for(const auto& p:receiver.drain()) deliver(sender,p);
        auto replies=sender.drain(); std::reverse(replies.begin(),replies.end());
        for(const auto& p:replies) deliver(receiver,p);
    }
    check(b->received->finished && b->received->bytes==a->source->bytes,"out-of-order file reassembly");
    check(receiver.take_completed()==std::vector<std::string>{"/confined/received"},"publish after all bytes");
    auto done=receiver.drain(); check(done.size()==1 && type(done[0])==11,"unlock completed transfer"); deliver(sender,done[0]);
    Writer request; request.le32(991).le32(1).le32(2).le32(0).le32(0).le32(10).le32(1);
    sender.accept(8,0,request.bytes()); check(sender.drain()[0][2]==2,"unknown/retired lock fails");
    sender.publish(sender.offer({"/test"}));
    request=Writer{}; request.le32(992).le32(1).le32(2).le32(0).le32(1).le32(10);
    sender.accept(8,0,request.bytes()); check(sender.drain()[0][2]==2,"high offset is bounded without truncation");
    request=Writer{}; request.le32(993).le32(1).le32(1).le32(0).le32(0).le32(8);
    sender.accept(8,0,request.bytes()); auto response=sender.drain()[0]; Reader size(View(response).subspan(12));
    check(size.le32()==200001 && size.le32()==0,"size response is little-endian u64");
    receiver.begin_remote(); (void)receiver.drain(); receiver.list(sender.file_list());
    const auto late=receiver.drain(); check(receiver.pending()==4,"pipeline filled"); receiver.cancel();
    check(b->received->aborted==1 && !b->received->finished,"cancel aborts private sink");
    auto old=late.front(); Reader r(View(old).subspan(8)); const auto old_id=r.le32();
    Writer stale; stale.le32(old_id).zeros(65536); receiver.accept(9,1,stale.bytes());
    check(!receiver.take_completed(),"late completion cannot publish cancelled transfer");
    Writer unsolicited; unsolicited.le32(0xffffffff); rejects([&]{receiver.accept(9,1,unsolicited.bytes());});
    receiver.begin_remote(); (void)receiver.drain(); receiver.list(sender.file_list());
    auto reads=receiver.drain(); Reader read(View(reads[0]).subspan(8)); Writer short_read; short_read.le32(read.le32());
    receiver.accept(9,1,short_read.bytes()); check(receiver.take_error().has_value() && b->received->aborted==1,"premature EOF aborts");
    // A legal short RANGE is retried at the next offset, without overlapping writes.
    FileClipboardLimits narrow; narrow.chunk = 11000;
    ClipboardFiles small_sender(a, narrow), small_receiver(b);
    small_sender.negotiate(4); small_receiver.negotiate(4); small_sender.publish(small_sender.offer({"/test"}));
    small_receiver.begin_remote(); small_receiver.list(small_sender.file_list());
    for (unsigned turn = 0; turn < 100 && !b->received->finished; ++turn) {
        for (const auto& p : small_receiver.drain()) deliver(small_sender, p);
        for (const auto& p : small_sender.drain()) deliver(small_receiver, p);
    }
    check(b->received->finished && b->received->bytes == a->source->bytes, "short ranges resume without holes");
    receiver.begin_remote(); receiver.tick(std::chrono::steady_clock::now()+std::chrono::seconds(31));
    check(receiver.take_error().has_value() && !receiver.awaiting_list(),"metadata timeout unlocks");
}
void clipboard() {
    auto store=std::make_shared<Store>(); populate(*store);
    Clipboard c; c.configure_files(store); auto init=c.start(); check(init[0][20]==0x1e,"capabilities enabled explicitly");
    Writer caps; caps.le16(1).le16(0).le16(1).le16(12).le32(999).le32(0x1e);
    (void)c.accept(clipboard_pdu(7,0,caps.bytes())); // Informational version must not gate flags.
    auto offer=c.set_local_files({"/test"}); check(offer.size()==1 && type(offer[0])==2,"local file offer");
    Reader local(View(offer[0]).subspan(8)); const auto format=local.le32();
    check(from_utf16le(local.take(local.remaining()))=="FileGroupDescriptorW","registered format name");
    Writer request; request.le32(format); auto reply=c.accept(clipboard_pdu(4,0,request.bytes()));
    check(decode_clipboard_files(View(reply.outbound[0]).subspan(8)).size()==3,"file metadata request");
    (void)c.accept(clipboard_pdu(3,1));
    Writer formats; formats.le32(0xc742).raw(utf16le("FileGroupDescriptorW"));
    auto remote=c.accept(clipboard_pdu(2,0,formats.bytes()));
    check(remote.outbound.size()==3 && type(remote.outbound[0])==3 && type(remote.outbound[1])==10,"ACK lock request order");
    Reader mapped(View(remote.outbound[2]).subspan(8)); check(mapped.le32()==0xc742,"remote format ID remapping");
    (void)c.set_local("new local clipboard");
    auto stale=c.accept(clipboard_pdu(5,1,encode_clipboard_files(store->source->descriptors)));
    check(!stale.remote_files && !stale.remote_text && !store->received,"local copy invalidates old remote file list");
    Clipboard legacy; legacy.configure_files(store); (void)legacy.start();
    caps=Writer{}; caps.le16(1).le16(0).le16(1).le16(12).le32(1).le32(4);
    (void)legacy.accept(clipboard_pdu(7,0,caps.bytes()));
    const auto short_offer=legacy.set_local_files({"test"})[0];
    check(short_offer[2]==4 && short_offer.size()==44,"short format name uses ASCII flag");
    Clipboard disabled; auto plain=disabled.start(); check(plain[0][20]==2,"default text-only capabilities");
}
}
int main() {
    try { wire(); transfers(); clipboard(); std::cout << "PASS: file descriptors, 10000 mutations, confined names, lock snapshots, size queries, out-of-order chunks, cancellation, deadlines, clipboard ID remapping\n"; return 0; }
    catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
