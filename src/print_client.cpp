#include "print_native_internal.hpp"
namespace lrdp::printing {
std::vector<drive::Device> list_printers(const std::string& path) {
    auto fd=local::connect(path);const auto bytes=local::exchange(fd.get(),local::list,{},-1,std::chrono::seconds(5));Reader in(bytes);
    require(in.le32()==0,"native printer list request rejected");const auto count=in.le32();require(count<=16,"native printer list exceeds quota");
    std::vector<drive::Device> devices;std::set<drive::DeviceKey> seen;
    for(unsigned i=0;i<count;++i) {
        drive::Device device;device.key.id=in.le32();device.key.generation=drive::read_u64(in);drive::PrinterInfo info;info.flags=in.le32();
        info.name=local::text(in);info.driver=local::text(in);
        require(device.key.generation && !info.name.empty() && seen.insert(device.key).second,"invalid native printer identity");
        device.name=info.name;device.printer=std::move(info);devices.push_back(std::move(device));
    }
    in.end();return devices;
}
Result submit_file(const std::string& path,drive::DeviceKey printer,const std::string& source) {
    auto snapshot=snapshot_file(source);auto fd=local::connect(path);Writer body;body.le32(printer.id);drive::write_u64(body,printer.generation);
    const auto bytes=local::exchange(fd.get(),local::submit,body.bytes(),snapshot.get(),std::chrono::seconds(330));Reader in(bytes);
    Result result;result.status=in.le32();result.transferred=drive::read_u64(in);in.end();return result;
}
} // namespace lrdp::printing
