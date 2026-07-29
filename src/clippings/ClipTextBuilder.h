#pragma once

#include <cstdint>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/reader/WordRef.h"

namespace ClipTextBuilder {

ClippingResult build(const std::vector<WordRef>& words, const uint16_t* wordOrder, int fromOrder, int toOrder,
                     int totalOrder, int startPageInSection, int sectionPageCount);

}  // namespace ClipTextBuilder
