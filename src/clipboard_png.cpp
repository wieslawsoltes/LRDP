#include "lrdp/clipboard/rich_content.hpp"
#include <png.h>
namespace lrdp {
namespace {
struct Image { png_image value{}; Image(){value.version=PNG_IMAGE_VERSION;} ~Image(){png_image_free(&value);} };
}
ClipboardImage decode_clipboard_png(View data) {
    require(!data.empty() && data.size()<=rich_clipboard_limit,"PNG clipboard quota"); Image png;
    require(png_image_begin_read_from_memory(&png.value,data.data(),data.size())!=0,"invalid PNG clipboard");
    ClipboardImage out{png.value.width,png.value.height,{}};
    require(out.width && out.height && out.width<=8192 && out.height<=8192 && std::uint64_t(out.width)*out.height<=(rich_clipboard_limit-124)/4,"PNG decoded allocation quota");
    out.bgra.resize(std::size_t(out.width)*out.height*4); png.value.format=PNG_FORMAT_BGRA;
    require(png_image_finish_read(&png.value,nullptr,out.bgra.data(),0,nullptr)!=0,"PNG clipboard decoding failed"); return out;
}
Bytes encode_clipboard_png(const ClipboardImage& image) {
    image.validate(); Image png; png.value.width=image.width; png.value.height=image.height; png.value.format=PNG_FORMAT_BGRA;
    png_alloc_size_t size=0; require(png_image_write_to_memory(&png.value,nullptr,&size,0,image.bgra.data(),0,nullptr)!=0 && size<=rich_clipboard_limit,"PNG clipboard output quota");
    Bytes out(size); require(png_image_write_to_memory(&png.value,out.data(),&size,0,image.bgra.data(),0,nullptr)!=0,"PNG clipboard encoding failed"); out.resize(size); return out;
}
}
