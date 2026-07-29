// Font family IDs: SHA256-of-headers sum (lib/EpdFont/scripts/build-font-ids.sh style).
// UI maps to Source Serif 4 (lists 12 pt, headers 14 pt). Battery % uses Noto 8.
#pragma once

#define BITTER_12_FONT_ID (1963441729)
#define BITTER_14_FONT_ID (-1705318616)
#define BITTER_16_FONT_ID (1467653849)
#define BITTER_18_FONT_ID (-582927980)
#define SOURCESERIF4_12_FONT_ID (386902914)
#define SOURCESERIF4_14_FONT_ID (-1077864260)
#define SOURCESERIF4_16_FONT_ID (1231166843)
#define SOURCESERIF4_18_FONT_ID (326065580)
// Lists / body chrome
#define UI_10_FONT_ID (SOURCESERIF4_12_FONT_ID)
// Headers / tabs (one step larger than list text)
#define UI_12_FONT_ID (SOURCESERIF4_14_FONT_ID)
#define SMALL_FONT_ID (674098198)

// Font ID 0 is reserved as the "not found" sentinel.
static_assert(BITTER_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BITTER_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BITTER_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BITTER_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SOURCESERIF4_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SMALL_FONT_ID != 0, "Font ID collision with sentinel");
