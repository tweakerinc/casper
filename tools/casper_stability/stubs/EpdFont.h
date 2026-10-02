#pragma once
#include <cstdint>
struct EpdGlyph { uint8_t width=8, height=12; int8_t top=12, left=0; uint16_t advanceX=128; uint32_t dataOffset=0; };
struct EpdFontData { const void* groups = reinterpret_cast<void*>(1); int identity=0; };
