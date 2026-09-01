#pragma once

namespace rivulet {

// Drop-cap paragraphs used to always start a new page when y > 0. That split
// "Chapter 1" onto an empty title page and put the first paragraph on page 2.
// Print books keep the heading and the opening drop-cap on the same page.
// Only break when this page already has body prose (or a full-bleed plate).
inline bool dropCapStartsNewPage(const bool atBlockStart, const bool yPastOrigin, const bool pageHasProse) {
  return atBlockStart && yPastOrigin && pageHasProse;
}

}  // namespace rivulet
