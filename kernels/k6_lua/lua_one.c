/*
 * The Lua core and the libraries K6 uses, compiled as one translation unit
 * (as upstream's onelua.c does). Seeds that Lua normally draws from the clock
 * and from addresses are fixed, so string hashing and table.sort pivots, and
 * therefore the work done, are identical in every run.
 *
 * Not included: io, os, package and debug libraries (K6 opens none of them).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#define luai_makeseed(L) ((unsigned int)0x6b36u)
#define l_randomizePivot() 0u

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#include "lprefix.h"

/* core */
#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"

/* libraries */
#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "lmathlib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
