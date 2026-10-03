#pragma once
#include <Esp.h>
struct DisplayStub { int getDisplayWidth() const { return 792; } int getDisplayHeight() const { return 528; } };
inline DisplayStub display;
