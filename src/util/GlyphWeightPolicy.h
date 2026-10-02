#pragma once

#include <cstdint>

// How much of a 2-bit glyph the BW pass inks. Values match
// GfxRenderer::BwGlyphWeight (Normal=0, Mild=1, Dense=2).
//
// History: Dense on body text fattened capitals (H/I/L). Mild fills AA holes
// without growing the outline. X4 UI is FAST with no grey pass, so leftover
// light fringe reads as holes ("whispy"). Dense is for that chrome only.
namespace glyphweight {

enum class Bw : uint8_t { Normal = 0, Mild = 1, Dense = 2 };

template <class E>
constexpr E as(const Bw w) {
  return static_cast<E>(static_cast<uint8_t>(w));
}

// Home / settings / menus. X3 keeps Mild (waveform + PPI already look solid).
// X4 inks the whole light fringe — UI never runs a grayscale pass.
inline Bw chrome(const bool x4) { return x4 ? Bw::Dense : Bw::Mild; }

// Reader body. AA-on leaves light fringe for the grey multipass.
// AA-off uses Mild on both panels (Dense made capitals look bold).
inline Bw reader(const bool aaOn) { return aaOn ? Bw::Normal : Bw::Mild; }

}  // namespace glyphweight
