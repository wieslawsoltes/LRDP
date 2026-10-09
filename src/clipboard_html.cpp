#include "lrdp/clipboard/rich_content.hpp"
#include <charconv>
#include <iomanip>
#include <map>
#include <sstream>
namespace lrdp {
void validate_clipboard_utf8(std::string_view text) {
    require(text.size() <= rich_clipboard_limit, "rich clipboard text exceeds quota");
    // Validation must not allocate a UTF-16 shadow of multi-megabyte HTML.
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 128) { require(first != 0, "embedded rich clipboard NUL"); continue; }
        std::uint32_t scalar = 0, minimum = 0; unsigned extra = 0;
        if ((first & 0xe0) == 0xc0) { scalar = first & 31; minimum = 0x80; extra = 1; }
        else if ((first & 0xf0) == 0xe0) { scalar = first & 15; minimum = 0x800; extra = 2; }
        else if ((first & 0xf8) == 0xf0) { scalar = first & 7; minimum = 0x10000; extra = 3; }
        else throw ProtocolError("invalid rich clipboard UTF-8 lead byte");
        require(extra <= text.size() - i, "truncated rich clipboard UTF-8");
        for (unsigned n = 0; n < extra; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            require((next & 0xc0) == 0x80, "invalid rich clipboard UTF-8 continuation");
            scalar = (scalar << 6) | (next & 63);
        }
        require(scalar >= minimum && scalar <= 0x10ffff && (scalar < 0xd800 || scalar > 0xdfff), "invalid rich clipboard Unicode scalar");
    }
}
void RichClipboard::validate() const {
    std::size_t size = 0;
    for (auto* s : {&text, &html}) if (*s) {
        require((*s)->size() <= rich_clipboard_limit - size, "rich clipboard snapshot exceeds quota"); size += (*s)->size();
    }
    if (image) {
        image->validate(); require(image->bgra.size() <= rich_clipboard_limit - size, "rich clipboard snapshot exceeds quota"); size += image->bgra.size();
    }
    require(size <= rich_clipboard_limit - 4096, "rich clipboard snapshot exceeds quota");
    for (auto* s : {&text, &html}) if (*s) validate_clipboard_utf8(**s);
}
Bytes encode_clipboard_html(std::string_view fragment) {
    validate_clipboard_utf8(fragment);
    constexpr std::string_view before = "<html><body><!--StartFragment-->", after = "<!--EndFragment--></body></html>";
    auto header = [](std::size_t start, std::size_t end, std::size_t first, std::size_t last) {
        std::ostringstream out;
        out << "Version:1.0\r\n" << std::setfill('0')
            << "StartHTML:" << std::setw(10) << start << "\r\nEndHTML:" << std::setw(10) << end
            << "\r\nStartFragment:" << std::setw(10) << first << "\r\nEndFragment:" << std::setw(10) << last << "\r\n";
        return out.str();
    };
    const auto start = header(0,0,0,0).size(), first = start + before.size(), last = first + fragment.size(), end = last + after.size();
    require(end + 1 <= rich_clipboard_limit, "HTML clipboard output exceeds quota");
    const auto body = header(start,end,first,last) + std::string(before) + std::string(fragment) + std::string(after);
    Bytes out(body.begin(),body.end()); out.push_back(0); return out;
}
std::string decode_clipboard_html(View data) {
    require(!data.empty() && data.size() <= rich_clipboard_limit, "invalid HTML clipboard size");
    if (data.back() == 0) data = data.first(data.size()-1);
    const std::string_view all(reinterpret_cast<const char*>(data.data()),data.size());
    std::map<std::string,std::int64_t> numbers;
    bool version = false; std::size_t offset = 0;
    while (offset < all.size() && offset < 4096) {
        if (numbers.contains("StartHTML") && numbers.at("StartHTML") >= 0 && offset >= std::size_t(numbers.at("StartHTML"))) break;
        if (all[offset] == '<') break;
        const auto end = all.find_first_of("\r\n",offset);
        require(end != all.npos && end < 4096, "unterminated HTML clipboard header");
        const auto line = all.substr(offset,end-offset);
        const auto colon = line.find(':');
        require(colon != line.npos, "invalid HTML clipboard field");
        const auto name=line.substr(0,colon); auto value=line.substr(colon+1);
        while (!value.empty() && value.front()==' ') value.remove_prefix(1);
        while (!value.empty() && value.back()==' ') value.remove_suffix(1);
        if (name=="Version") { require(!version && (value=="1.0" || value=="0.9"), "unsupported HTML clipboard version"); version=true; }
        else if (name=="StartHTML" || name=="EndHTML" || name=="StartFragment" || name=="EndFragment" || name=="StartSelection" || name=="EndSelection") {
            std::int64_t n=0; const auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
            require(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && n>=-1 && numbers.emplace(std::string(name),n).second,"invalid/duplicate HTML clipboard offset");
        }
        offset=end+1; if (all[end]=='\r' && offset<all.size() && all[offset]=='\n') ++offset;
        if (numbers.contains("StartHTML") && numbers.at("StartHTML")==-1 && numbers.contains("StartFragment") && offset==std::size_t(numbers.at("StartFragment"))) break;
    }
    require(version && numbers.contains("StartHTML") && numbers.contains("EndHTML") && numbers.contains("StartFragment") && numbers.contains("EndFragment"),"missing HTML clipboard offsets");
    const auto start=numbers.at("StartHTML"), end=numbers.at("EndHTML"), first=numbers.at("StartFragment"), last=numbers.at("EndFragment");
    require(first>=std::int64_t(offset) && last>=first && std::uint64_t(last)<=data.size(),"HTML fragment outside payload");
    require((start==-1 && end==-1) || (start>=std::int64_t(offset) && start<=first && end>=last && std::uint64_t(end)<=data.size()),"HTML context outside payload");
    if (numbers.contains("StartSelection") || numbers.contains("EndSelection")) {
        require(numbers.contains("StartSelection") && numbers.contains("EndSelection"),"incomplete HTML selection offsets");
        require(numbers.at("StartSelection")>=first && numbers.at("EndSelection")>=numbers.at("StartSelection") && numbers.at("EndSelection")<=last,"HTML selection outside fragment");
    }
    validate_clipboard_utf8(all.substr(offset));
    auto fragment=std::string(all.substr(std::size_t(first),std::size_t(last-first))); validate_clipboard_utf8(fragment); return fragment;
}
}
