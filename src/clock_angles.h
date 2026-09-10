#pragma once
#include <Arduino.h>

// Real per-frame sun/moon rotation angle, extracted from the
// actual clock_00..clock_63.png pixel data (tools/gen_clock_frames.py)
// rather than assumed to be perfectly linear. Degrees, fixed-point
// x10 (e.g. -900 = -90.0 deg), screen-space convention (-90 = up).
// See README.md's Asset provenance section before sharing this
// repo publicly.
static const uint16_t CLOCK_ANGLE_STEPS = 64;
static const int16_t clockAngleTenthsDeg[64] = { -900, -854, -700, -607, -440, -412, -351, -246, -207, -146, -99, -57, 9, 58, 101, 129, 157, 197, 258, 286, 314, 358, 401, 429, 488, 517, 545, 573, 661, 723, 844, 1057, 1086, 1149, 1211, 1292, 1455, 1483, 1523, 1551, 1579, 1607, 1635, 1663, 1691, 1719, 1748, 1776, 1804, 1832, 1860, 1888, 1916, 1944, 1973, 2001, 2029, 2057, 2151, 2224, 2252, 2407, 2500, 2654 };
