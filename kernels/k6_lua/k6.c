/*
 * K6 — interpreter start and script. Each task creates a fresh Lua state
 * (no JIT), opens the standard libraries it needs, compiles a fixed script
 * from source and runs it: the short launch burst of an embedded scripting
 * runtime. The script returns an integer, which is the task checksum.
 *
 * Lua's memory comes from an arena of this file (one per thread, reset for every task), not from the C library:
 * the interpreter allocates and frees thousands of small blocks per run, and with malloc the result followed
 * the operating system's allocator more than the processor (the same laptop took 1.6 times longer under
 * Windows than under Linux).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernels.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

static const char SCRIPT[] =
    "math.randomseed(42)\n"
    "local M = 2147483647\n"
    "local acc = 0\n"
    "-- tables and sorting with a comparator\n"
    "local t = {}\n"
    "for i = 1, 4000 do t[i] = math.random(1, 1000000) end\n"
    "table.sort(t, function(a, b) return a > b end)\n"
    "for i = 1, #t, 97 do acc = (acc + t[i]) % M end\n"
    "-- string building, formatting and patterns\n"
    "local parts = {}\n"
    "for i = 1, 2000 do parts[#parts + 1] = string.format('item-%05d:%x', i, i * 7919) end\n"
    "local s = table.concat(parts, ',')\n"
    "for k, v in s:gmatch('item%-(%d+):(%x+)') do acc = (acc + tonumber(k) + tonumber(v, 16)) % M end\n"
    "s = s:gsub('item', 'it'):upper()\n"
    "acc = (acc + #s + s:byte(1000)) % M\n"
    "-- metatables and closures\n"
    "local Vec = {}\n"
    "Vec.__index = Vec\n"
    "function Vec.new(x, y) return setmetatable({x = x, y = y}, Vec) end\n"
    "function Vec.__add(a, b) return Vec.new(a.x + b.x, a.y + b.y) end\n"
    "function Vec:dot(o) return self.x * o.x + self.y * o.y end\n"
    "local v = Vec.new(0, 0)\n"
    "for i = 1, 5000 do v = v + Vec.new(i % 7, i % 11) end\n"
    "acc = (acc + v:dot(Vec.new(3, 5))) % M\n"
    "local function counter() local n = 0 return function() n = n + 1 return n end end\n"
    "local c = counter()\n"
    "for i = 1, 3000 do acc = (acc + c()) % M end\n"
    "-- coroutines\n"
    "local gen = coroutine.wrap(function() for i = 1, 3000 do coroutine.yield(i * i % 1009) end end)\n"
    "for i = 1, 3000 do acc = (acc + gen()) % M end\n"
    "-- integer sieve\n"
    "local N, comp = 20000, {}\n"
    "for i = 2, N do\n"
    "  if not comp[i] then\n"
    "    acc = (acc + i) % M\n"
    "    for j = i * i, N, i do comp[j] = true end\n"
    "  end\n"
    "end\n"
    "-- string-keyed hash map\n"
    "local m = {}\n"
    "for i = 1, 3000 do local k = 'k' .. (i * 31 % 1000) m[k] = (m[k] or 0) + i end\n"
    "local keys = {}\n"
    "for k in pairs(m) do keys[#keys + 1] = k end\n"
    "table.sort(keys)\n"
    "for i, k in ipairs(keys) do acc = (acc + i * m[k]) % M end\n"
    "-- recursion\n"
    "local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end\n"
    "acc = (acc + fib(20)) % M\n"
    "return acc\n";

typedef struct k6 {
  size_t runs;
} k6;

/*
 * The arena: blocks are cut from one buffer and, when freed, kept in a list per size class for reuse. Classes
 * are multiples of 16 bytes up to 512, then powers of two. Lua passes a block's size when it frees or resizes
 * it, so blocks carry no header. The arena is far larger than the script needs, and a
 * request it cannot meet fails like a failed malloc (the task then returns 0, a wrong checksum).
 */
#define K6_ARENA ((size_t)32 << 20)
#define K6_SMALL 512
#define K6_CLASSES (K6_SMALL / 16 + 40)

typedef struct arena {
  unsigned char *base;
  size_t used;
  void *free_list[K6_CLASSES];
} arena;

static size_t size_class(size_t n, size_t *rounded) {
  if (n <= K6_SMALL) {
    size_t c = n ? (n - 1) / 16 : 0;
    *rounded = (c + 1) * 16;
    return c;
  }
  size_t c = K6_SMALL / 16, sz = K6_SMALL * 2;
  while (sz < n) sz *= 2, c++;
  *rounded = sz;
  return c;
}

static void *arena_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  arena *a = ud;
  size_t orounded = 0, nrounded = 0, oc = ptr ? size_class(osize, &orounded) : 0;
  if (nsize == 0) {
    if (ptr) {
      *(void **)ptr = a->free_list[oc];
      a->free_list[oc] = ptr;
    }
    return NULL;
  }
  size_t nc = size_class(nsize, &nrounded);
  if (ptr && nc == oc) return ptr;
  if (nc >= K6_CLASSES) return NULL;
  void *p = a->free_list[nc];
  if (p) {
    a->free_list[nc] = *(void **)p;
  } else {
    if (nrounded > K6_ARENA - a->used) return NULL;
    p = a->base + a->used;
    a->used += nrounded;
  }
  if (ptr) {
    memcpy(p, ptr, osize < nsize ? osize : nsize);
    *(void **)ptr = a->free_list[oc];
    a->free_list[oc] = ptr;
  }
  return p;
}

static void *k6_scratch_new(const void *p) {
  (void)p;
  arena *a = calloc(1, sizeof *a);
  if (a && !(a->base = malloc(K6_ARENA))) {
    free(a);
    a = NULL;
  }
  return a;
}

static void k6_scratch_free(void *p) {
  arena *a = p;
  if (!a) return;
  free(a->base);
  free(a);
}

static void *k6_create(const pmk_tk_args *a) {
  k6 *s = malloc(sizeof *s);
  if (!s) {
    snprintf(a->err, a->errlen, "out of memory");
    return NULL;
  }
  s->runs = a->size == PMK_SIZE_BURST ? 2 : 32;
  return s;
}

static size_t k6_ntasks(const void *p) { return ((const k6 *)p)->runs; }
static uint64_t k6_task_work(const void *p, size_t i) {
  (void)p;
  (void)i;
  return 1;
}

static const luaL_Reg LIBS[] = {{LUA_GNAME, luaopen_base},          {LUA_COLIBNAME, luaopen_coroutine},
                                {LUA_TABLIBNAME, luaopen_table},    {LUA_STRLIBNAME, luaopen_string},
                                {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
                                {NULL, NULL}};

static uint64_t k6_task(const void *p, void *sp, size_t i) {
  (void)p;
  arena *a = sp;
  a->used = 0;
  memset(a->free_list, 0, sizeof a->free_list);
  lua_State *L = lua_newstate(arena_alloc, a);
  if (!L) return 0;
  for (const luaL_Reg *lib = LIBS; lib->func; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
  uint64_t h = 0;
  if (luaL_loadbufferx(L, SCRIPT, sizeof SCRIPT - 1, "=k6", "t") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK &&
      lua_isinteger(L, -1))
    h = pmk_mix64((uint64_t)lua_tointeger(L, -1) ^ (uint64_t)i);
  lua_close(L);
  return h;
}

static uint64_t k6_input_hash(const void *p) {
  const k6 *s = p;
  return pmk_hash_bytes(SCRIPT, sizeof SCRIPT - 1, s->runs);
}

const pmk_tk PMK_SYM(pmk_k6_tk) = {
    .id = "K6",
    .name = "Lua start and script",
    .unit = "runs/s",
    .unit_div = 1,
    .create = k6_create,
    .destroy = free,
    .ntasks = k6_ntasks,
    .task_work = k6_task_work,
    .scratch_new = k6_scratch_new,
    .scratch_free = k6_scratch_free,
    .task = k6_task,
    .input_hash = k6_input_hash,
};
