// Font family IDs: SHA256-of-headers sum (lib/EpdFont/scripts/build-font-ids.sh style).
// Built-in UI chrome + Penumbra: Source Serif 4. Alternate reader face: Literata.
// Reader body ship sizes: 10/12/14/16. Source Serif 8 = UI chrome only.
#pragma once

// Literata (serif, OFL) — built-in reader family (10–16).
// Stem-calibrated ppem + gap-fix (iwalton3/cpfont-editor pipeline).
#define LITERATA_10_FONT_ID (-1128177077)
#define LITERATA_12_FONT_ID (2090520927)
#define LITERATA_14_FONT_ID (-847079762)
#define LITERATA_16_FONT_ID (-209681255)
// 8 pt regular — Recents author, battery %, button hints (not a reader body size).
// Stem-calibrated ppem + gap-fix (iwalton3/cpfont-editor pipeline).
#define SOURCESERIF4_8_FONT_ID (1470095001)
// 10 pt regular + bold — Recents titles / menu small.
#define SOURCESERIF4_10_FONT_ID (-324599973)
#define SOURCESERIF4_12_FONT_ID (876380291)
#define SOURCESERIF4_14_FONT_ID (426921930)
#define SOURCESERIF4_16_FONT_ID (1484141743)
// Home / UI large titles (Source Serif only — not a Literata reader body size).
#define SOURCESERIF4_18_FONT_ID (652444703)
// Penumbra large time (digits + colon only; Source Serif Bold 72 pt 2-bit).
#define SOURCESERIF4_72_CLOCK_FONT_ID (-746347856)
// Lists / body chrome
#define UI_10_FONT_ID (SOURCESERIF4_12_FONT_ID)
// Headers / tabs (one step larger than list text)
#define UI_12_FONT_ID (SOURCESERIF4_14_FONT_ID)
// Small chrome → Source Serif 8 (no separate Noto family).
#define SMALL_FONT_ID (SOURCESERIF4_8_FONT_ID)

// Font ID 0 is reserved as the "not found" sentinel.
static_assert(LITERATA_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LITERATA_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LITERATA_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LITERATA_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_8_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_72_CLOCK_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SMALL_FONT_ID != 0, "Font ID collision with sentinel");
