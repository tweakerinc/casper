// Font family IDs: SHA256-of-headers sum (lib/EpdFont/scripts/build-font-ids.sh style).
// Built-in UI chrome + Penumbra: Source Serif 4 only. Bitter is the alternate reader face.
#pragma once

#define BITTER_12_FONT_ID (1963441729)
#define BITTER_14_FONT_ID (-1705318616)
#define BITTER_16_FONT_ID (1467653849)
#define BITTER_18_FONT_ID (-582927980)
// 8 pt regular — Recents author, battery %, button hints (was Noto Sans 8).
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
static_assert(BITTER_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BITTER_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BITTER_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BITTER_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_8_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_72_CLOCK_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SMALL_FONT_ID != 0, "Font ID collision with sentinel");
