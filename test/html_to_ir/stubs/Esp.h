#pragma once
// Host stub for the Arduino-ESP32 Esp.h singleton.
//
// ChapterIr/HtmlToIr consult free + largest-contiguous heap to decide whether a
// growth step is safe. On the host there is no such budget, so report a large
// figure and let the parser run to completion — these tests are about parse
// CORRECTNESS, not the device's OOM behaviour.
//
// The values are settable so a test can also drive the low-heap paths.

#include <cstddef>
#include <cstdint>

class EspStub {
 public:
  uint32_t getFreeHeap() const { return freeHeap_; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap_; }

  void setFreeHeap(const uint32_t v) { freeHeap_ = v; }
  void setMaxAllocHeap(const uint32_t v) { maxAllocHeap_ = v; }
  void reset() {
    freeHeap_ = 4u * 1024u * 1024u;
    maxAllocHeap_ = 4u * 1024u * 1024u;
  }

 private:
  uint32_t freeHeap_ = 4u * 1024u * 1024u;
  uint32_t maxAllocHeap_ = 4u * 1024u * 1024u;
};

extern EspStub ESP;
