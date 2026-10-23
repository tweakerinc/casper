#pragma once

namespace rivulet {

// Body lines that should follow a heading cluster on the same page. Two is the
// print-book default; fall back to one when the heading is too tall for two.
inline constexpr int kKeepWithNextBodyLines = 2;

// A heading (plus attached ornaments / chapter-number blocks) must not sit at
// the bottom of a page with its first paragraph on the next page.
//
// Move the cluster to the next page unless enough following body lines fit.
// Never move a cluster that started at the page top, or one that cannot fit
// on an empty page with at least one body line (that would loop forever).
inline bool headingClusterMovesToNextPage(const bool clusterStartedAtPageTop, const int clusterHeightPx,
                                          const int remainingAfterClusterPx, const int bodyLinePx,
                                          const int pageHeightPx) {
  if (clusterStartedAtPageTop) return false;
  if (bodyLinePx <= 0 || pageHeightPx <= 0 || clusterHeightPx < 0) return false;
  int need = kKeepWithNextBodyLines * bodyLinePx;
  if (clusterHeightPx + need > pageHeightPx) need = bodyLinePx;
  if (clusterHeightPx + need > pageHeightPx) return false;
  return remainingAfterClusterPx < need;
}

}  // namespace rivulet
