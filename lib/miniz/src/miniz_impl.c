/* Compiles the vendored miniz with Casper's configuration. The include
 * order is load-bearing (the config defines/renames must be seen first). */
// clang-format off
#include "MinizConfig.h"

#include "../third_party/miniz.c"
// clang-format on
