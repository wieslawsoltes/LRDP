#include "lrdp/drive/protocol.hpp"
namespace lrdp::drive {
PrinterInfo printer_information(View data) {
    Reader in(data); PrinterInfo result; result.flags=in.le32();
    require((result.flags&~31U)==0 && in.le32()==0,"invalid printer flags or code page");
    const auto pnp=in.le32(),driver=in.le32(),name=in.le32(),cached=in.le32();
    require(pnp<=4096 && driver>0 && driver<=1024 && name>0 && name<=1024 && cached<=65536,"printer metadata exceeds quota");
    // PnP and cached configuration are opaque hints, never executable drivers.
    in.skip(pnp); const auto driver_data=in.take(driver);
    if(result.flags&1) {
        require(driver_data.back()==0,"unterminated ASCII printer driver");
        for(auto c:driver_data.first(driver_data.size()-1)) require(c>=32 && c<127,"invalid ASCII printer driver");
        result.driver.assign(driver_data.begin(),driver_data.end()-1);
    } else result.driver=from_utf16le(driver_data);
    result.name=from_utf16le(in.take(name)); in.skip(cached); in.end();
    for(const auto* text:{&result.name,&result.driver}) {
        require(!text->empty() && text->size()<=512,"printer name exceeds UTF-8 quota");
        for(unsigned char c:*text) require(c>=32 && c!=127,"printer name contains control characters");
    }
    return result;
}
} // namespace lrdp::drive
