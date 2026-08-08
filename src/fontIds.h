// Font family IDs: SHA256-of-headers sum (lib/EpdFont/scripts/build-font-ids.sh style).
// Built-in UI chrome + Penumbra: Source Serif 4. Alternate reader face: Lexend Deca (OFL).
// Sizes: both families ship 8/10/12/14/16/18 (UI smallest is 8 pt).
#pragma once

// Lexend Deca (sans, OFL) — alternate built-in reader family.
#define LEXENDDECA_8_FONT_ID (-537075679)
#define LEXENDDECA_10_FONT_ID (-1602494176)
#define LEXENDDECA_12_FONT_ID (-789173636)
#define LEXENDDECA_14_FONT_ID (300363550)
#define LEXENDDECA_16_FONT_ID (-940581834)
#define LEXENDDECA_18_FONT_ID (-2078415541)
// 8 pt regular — Recents author, battery %, button hints.
#define SOURCESERIF4_8_FONT_ID (-2097557390)
// 10 pt regular + bold — Recents titles.
#define SOURCESERIF4_10_FONT_ID (1970618696)
#define SOURCESERIF4_12_FONT_ID (386902914)
#define SOURCESERIF4_14_FONT_ID (-1077864260)
#define SOURCESERIF4_16_FONT_ID (1231166843)
#define SOURCESERIF4_18_FONT_ID (326065580)
// Penumbra large time (digits + colon only; Source Serif Bold 72 pt 2-bit).
#define SOURCESERIF4_72_CLOCK_FONT_ID (-746347856)
// Lists / body chrome
#define UI_10_FONT_ID (SOURCESERIF4_12_FONT_ID)
// Headers / tabs (one step larger than list text)
#define UI_12_FONT_ID (SOURCESERIF4_14_FONT_ID)
// Small chrome → Source Serif 8 (no separate Noto family).
#define SMALL_FONT_ID (SOURCESERIF4_8_FONT_ID)

// Font ID 0 is reserved as the "not found" sentinel.
static_assert(LEXENDDECA_8_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LEXENDDECA_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LEXENDDECA_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LEXENDDECA_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LEXENDDECA_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(LEXENDDECA_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_8_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_72_CLOCK_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SMALL_FONT_ID != 0, "Font ID collision with sentinel");
