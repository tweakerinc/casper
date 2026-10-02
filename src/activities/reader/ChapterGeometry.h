#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
class Epub;
class GfxRenderer;
namespace rivulet {class RivuletEngine;}
// Shared geometry only: the reader and every indexer fit the same image boxes.
// Does not decode pixels, paint, or borrow a framebuffer.
namespace chaptergeometry {
size_t prepareRange(Epub& epub,GfxRenderer& renderer,rivulet::RivuletEngine& target,
                    const std::string& spineHref,uint8_t imageMode,size_t first,size_t count);
void prepare(Epub& epub, GfxRenderer& renderer, rivulet::RivuletEngine& target,
             const std::string& spineHref, uint8_t imageMode);
}
