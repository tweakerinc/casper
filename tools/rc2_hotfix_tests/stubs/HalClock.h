#pragma once
#include <cstdint>
struct MockClock { bool isAvailable()const{return false;} bool getDateTime(uint16_t&,uint8_t&,uint8_t&,uint8_t&,uint8_t&){return false;} };
inline MockClock halClock;
