// Single translation unit holding the vendored minimp3 implementation (CC0, see
// third_party/minimp3_LICENSE). Float output so hybrid-frame overshoot isn't clipped away.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "third_party/minimp3.h"
