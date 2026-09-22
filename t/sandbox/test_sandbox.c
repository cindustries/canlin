/* Sandbox contract tests: run Lua snippets through canlin_sandbox_run and
 * assert on status, log output and scope liveness. Exit 0 on success. */
#include "canlin/sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* ---- host callbacks -------------------------------------------------- */

typedef struct {
    char log[4096];
    size_t log_len;
    const char *const *mods; /* name, source, name, source, ..., NULL */
    const char *bin;         /* binary chunk served for "bin" */
    size_t bin_len;
} host;

static void host_log(void *ud, const char *msg, size_t len)
{
    host *h = ud;
    if (len > sizeof h->log - h->log_len - 2)
        len = sizeof h->log - h->log_len - 2;
    memcpy(h->log + h->log_len, msg, len);
    h->log_len += len;
    h->log[h->log_len++] = '\n';
    h->log[h->log_len] = 0;
}

static int host_require(void *ud, const char *name, size_t name_len, const char **src,
                        size_t *len)
{
    host *h = ud;
    const char *const *m;
    if (h->bin && strcmp(name, "bin") == 0) {
        *src = h->bin;
        *len = h->bin_len;
        return 1;
    }
    for (m = h->mods; m && *m; m += 2) {
        if (strlen(m[0]) == name_len && memcmp(m[0], name, name_len) == 0) {
            *src = m[1];
            *len = strlen(m[1]);
            return 1;
        }
    }
    return 0;
}

static canlin_sandbox *mk(host *h, size_t mem, uint32_t budget)
{
    canlin_sandbox_config cfg;
    canlin_sandbox *sb;
    memset(&cfg, 0, sizeof cfg);
    cfg.mem_limit = mem;
    cfg.budget = budget;
    cfg.log = host_log;
    cfg.require = host_require;
    cfg.ud = h;
    sb = canlin_sandbox_new(&cfg);
    if (sb == NULL) {
        fprintf(stderr, "canlin_sandbox_new failed\n");
        exit(1);
    }
    return sb;
}

static canlin_sandbox_status run(canlin_sandbox *sb, const char *src)
{
    return canlin_sandbox_run(sb, src, strlen(src), "=test");
}

static const char *st_name(canlin_sandbox_status st)
{
    switch (st) {
    case CANLIN_SANDBOX_OK: return "OK";
    case CANLIN_SANDBOX_ERROR: return "ERROR";
    case CANLIN_SANDBOX_FAULT_BUDGET: return "FAULT_BUDGET";
    case CANLIN_SANDBOX_FAULT_MEMORY: return "FAULT_MEMORY";
    case CANLIN_SANDBOX_DEAD: return "DEAD";
    }
    return "?";
}

/* Run src in a fresh sandbox, expect status; returns the host for log checks. */
static void expect(const char *what, size_t mem, uint32_t budget, const char *src,
                   canlin_sandbox_status want, host *h)
{
    canlin_sandbox *sb = mk(h, mem, budget);
    canlin_sandbox_status st = run(sb, src);
    CHECK(st == want, "%s: got %s (%s), want %s", what, st_name(st),
          canlin_sandbox_errmsg(sb), st_name(want));
    if (want == CANLIN_SANDBOX_FAULT_BUDGET || want == CANLIN_SANDBOX_FAULT_MEMORY) {
        CHECK(canlin_sandbox_is_dead(sb), "%s: faulted scope not dead", what);
        st = run(sb, "print('re-entered')");
        CHECK(st == CANLIN_SANDBOX_DEAD, "%s: dead scope ran again: %s", what, st_name(st));
        CHECK(strstr(h->log, "re-entered") == NULL, "%s: dead scope executed Lua", what);
    }
    canlin_sandbox_free(sb);
}

#define SMALL_BUDGET 200000u
#define MEM (2u * 1024u * 1024u)

/* ---- tests ----------------------------------------------------------- */

static void test_environment(void)
{
    host h;
    memset(&h, 0, sizeof h);
    expect("absent globals", MEM, SMALL_BUDGET,
           "assert(os == nil and io == nil and package == nil and debug == nil)\n"
           "assert(dofile == nil and loadfile == nil and warn == nil)\n"
           "assert(coroutine == nil and string.dump == nil)\n"
           "assert(getmetatable('').__index.dump == nil)\n"
           "assert(type(string.rep) == 'function' and type(table.move) == 'function')\n"
           "assert(math and utf8 and table and string)\n",
           CANLIN_SANDBOX_OK, &h);

    memset(&h, 0, sizeof h);
    expect("print goes to host log", MEM, SMALL_BUDGET, "print('hello', 42, nil)",
           CANLIN_SANDBOX_OK, &h);
    CHECK(strcmp(h.log, "hello\t42\tnil\n") == 0, "log was '%s'", h.log);

    memset(&h, 0, sizeof h);
    expect("print truncates", MEM, SMALL_BUDGET, "print(string.rep('y', 5000))",
           CANLIN_SANDBOX_OK, &h);
    CHECK(h.log_len == 1024 + 1, "log length %zu", h.log_len);

    memset(&h, 0, sizeof h);
    expect("restricted base functions", MEM, SMALL_BUDGET,
           "assert(not pcall(collectgarbage, 'collect'))\n"
           "assert(not pcall(collectgarbage, 'stop'))\n"
           "assert(type(collectgarbage('count')) == 'number')\n"
           "assert(not pcall(setmetatable, {}, {__gc = function() end}))\n"
           "assert(getmetatable(setmetatable({}, {__index = {}})))\n"
           "local e = {}\n"
           "local f = assert(load('x = 1; return x', 'c', 't', e))\n"
           "assert(f() == 1 and e.x == 1 and x == nil)\n"
           "local g, err = load('syntax error here')\n"
           "assert(g == nil and err)\n"
           "local ok, st = pcall(error, 'boom')\n"
           "assert(not ok and st:find('boom'))\n"
           "local ok2, v = xpcall(function() error('e1') end, function(m) return 'h:' .. m end)\n"
           "assert(not ok2 and v:find('h:'))\n"
           "assert(select('#', pcall(function() return 1, 2, 3 end)) == 4)\n"
           "assert(string.rep('', 2^62) == '' and string.rep('ab', 3, ',') == 'ab,ab,ab')\n"
           "assert(not pcall(string.rep, 'x', 2^40))\n",
           CANLIN_SANDBOX_OK, &h);

    memset(&h, 0, sizeof h);
    expect("ordinary error keeps scope alive", MEM, SMALL_BUDGET, "error('plain')",
           CANLIN_SANDBOX_ERROR, &h);
}

typedef struct {
    char buf[4096];
    size_t n;
} dumpbuf;

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    dumpbuf *w = ud;
    (void)L;
    if (sz > sizeof w->buf - w->n)
        return 1;
    memcpy(w->buf + w->n, p, sz);
    w->n += sz;
    return 0;
}

static void test_binary_chunks(void)
{
    static dumpbuf w;
    host h;
    lua_State *P;
    canlin_sandbox *sb;
    canlin_sandbox_status st;

    /* A genuine binary chunk, produced by a plain (unsandboxed) host state. */
    P = luaL_newstate();
    CHECK(luaL_loadstring(P, "print('binary ran') return 1") == LUA_OK, "host load");
    CHECK(lua_dump(P, dump_writer, &w, 0) == 0, "host dump");
    lua_close(P);
    CHECK(w.n > 4 && w.buf[0] == 0x1b, "dump did not produce a binary chunk");

    memset(&h, 0, sizeof h);
    sb = mk(&h, MEM, SMALL_BUDGET);
    st = canlin_sandbox_run(sb, w.buf, w.n, "=bin");
    CHECK(st == CANLIN_SANDBOX_ERROR && strstr(canlin_sandbox_errmsg(sb), "binary"),
          "host run of binary chunk: %s (%s)", st_name(st), canlin_sandbox_errmsg(sb));
    h.bin = w.buf;
    h.bin_len = w.n;
    st = run(sb, "local ok, e = pcall(require, 'bin')\n"
                 "assert(not ok and e:find('binary'), e)\n"
                 "local f, e2 = load('\\27Lua')\n"
                 "assert(f == nil and e2:find('binary'), e2)\n"
                 "assert(not pcall(load, 'return 1', 'x', 'b'))\n"
                 "assert(not pcall(load, 'return 1', 'x', 'bt'))\n"
                 "assert(not pcall(load, function() return nil end))\n");
    CHECK(st == CANLIN_SANDBOX_OK, "binary rejection inside Lua: %s (%s)", st_name(st),
          canlin_sandbox_errmsg(sb));
    CHECK(strstr(h.log, "binary ran") == NULL, "binary chunk executed");
    canlin_sandbox_free(sb);
}

static void test_budget(void)
{
    static const struct {
        const char *what;
        const char *src;
    } cases[] = {
        {"plain loop", "while true do end"},
        {"pcall loop", "while true do pcall(function() while true do end end) end"},
        {"pcall(pcall(pcall))", "while true do pcall(pcall, pcall, function() while true do end end) end"},
        {"xpcall with looping handler",
         "while true do xpcall(function() while true do end end, function(e) while true do end end) end"},
        {"xpcall handler loops on ordinary error",
         "while true do xpcall(function() error('x') end, function(e) while true do end end) end"},
        {"xpcall handler swallows and retries",
         "local n = 0\n"
         "while true do xpcall(function() while true do end end, function(e) n = n + 1; return e end) end"},
        {"nested pcall/xpcall/error",
         "while true do pcall(function() xpcall(function() pcall(error, 'e'); while true do end end,"
         " function() return pcall(function() while true do end end) end) end) end"},
        {"error() inside handler",
         "while true do xpcall(function() while true do end end, function(e) error(e) end) end"},
        {"__close loop", "do local x <close> = setmetatable({}, {__close = function() while true do end end})\n"
                         "while true do end end"},
        {"__tostring loop via print",
         "while true do pcall(print, setmetatable({}, {__tostring = function() while true do end end})) end"},
        {"__index loop", "local t = setmetatable({}, {__index = function() while true do end end})\n"
                         "while true do pcall(function() return t.x end) end"},
        {"load loop", "while true do pcall(load('while true do end')) end"},
        {"table.move over empty range", "table.move({}, 1, math.maxinteger - 1, 2)"},
        {"table.move in pcall", "while true do pcall(table.move, {}, 1, 1 << 40, 2) end"},
        {"string.rep charged", "while true do pcall(string.rep, 'x', 100000) end"},
        {"sort comparator loop",
         "while true do pcall(table.sort, {3, 2, 1}, function() while true do end end) end"},
        {"gsub callback loop", "while true do pcall(string.gsub, 'aaa', 'a', function() while true do end end) end"},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        host h;
        memset(&h, 0, sizeof h);
        expect(cases[i].what, MEM, SMALL_BUDGET, cases[i].src, CANLIN_SANDBOX_FAULT_BUDGET, &h);
    }

    /* A caught fault must never let code after the pcall run. */
    {
        host h;
        memset(&h, 0, sizeof h);
        expect("no code after caught fault", MEM, SMALL_BUDGET,
               "local ok = pcall(function() while true do end end)\n"
               "print('escaped', ok)",
               CANLIN_SANDBOX_FAULT_BUDGET, &h);
        CHECK(strstr(h.log, "escaped") == NULL, "fault was swallowed: %s", h.log);
    }

    /* The budget is per callback: a sandbox can run many bounded callbacks. */
    {
        host h;
        canlin_sandbox *sb;
        int k;
        memset(&h, 0, sizeof h);
        sb = mk(&h, MEM, SMALL_BUDGET);
        for (k = 0; k < 20; k++) {
            canlin_sandbox_status st = run(sb, "local s = 0 for i = 1, 20000 do s = s + i end");
            CHECK(st == CANLIN_SANDBOX_OK, "bounded callback %d: %s (%s)", k, st_name(st),
                  canlin_sandbox_errmsg(sb));
        }
        canlin_sandbox_free(sb);
    }
}

static void test_memory(void)
{
    static const struct {
        const char *what;
        const char *src;
    } cases[] = {
        {"string doubling", "local s = 'x' while true do s = s .. s end"},
        {"table growth", "local t = {} for i = 1, 1e9 do t[i] = i end"},
        {"table growth in pcall",
         "while true do pcall(function() local t = {} for i = 1, 1e9 do t[i] = {} end end) end"},
        {"string.rep near limit", "local t = {} for i = 1, 100 do t[i] = string.rep('x', 512 * 1024, tostring(i)) end"},
        {"load of huge source", "local ok = pcall(load, string.rep('x = 1 ', 300000)) print('after', ok)"},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        host h;
        memset(&h, 0, sizeof h);
        expect(cases[i].what, MEM, 100000000u, cases[i].src, CANLIN_SANDBOX_FAULT_MEMORY, &h);
        CHECK(strstr(h.log, "after") == NULL, "%s: code ran after memory fault", cases[i].what);
    }
    {
        host h;
        memset(&h, 0, sizeof h);
        expect("oversized string.rep is an ordinary error", MEM, SMALL_BUDGET,
               "string.rep('x', 1 << 40)", CANLIN_SANDBOX_ERROR, &h);
    }
}

static void test_isolation(void)
{
    host ha, hb;
    canlin_sandbox *a, *b;
    canlin_sandbox_status st;
    memset(&ha, 0, sizeof ha);
    memset(&hb, 0, sizeof hb);
    a = mk(&ha, MEM, SMALL_BUDGET);
    b = mk(&hb, MEM, SMALL_BUDGET);
    st = run(b, "counter = 1");
    CHECK(st == CANLIN_SANDBOX_OK, "healthy before: %s", st_name(st));
    st = run(a, "counter = 99 while true do pcall(function() while true do end end) end");
    CHECK(st == CANLIN_SANDBOX_FAULT_BUDGET, "faulty: %s", st_name(st));
    st = run(b, "assert(counter == 1) counter = counter + 1 print(counter)");
    CHECK(st == CANLIN_SANDBOX_OK, "healthy after: %s (%s)", st_name(st), canlin_sandbox_errmsg(b));
    CHECK(strcmp(hb.log, "2\n") == 0, "healthy log '%s'", hb.log);
    st = run(a, "print('again')");
    CHECK(st == CANLIN_SANDBOX_DEAD && ha.log_len == 0, "faulty re-entered: %s", st_name(st));
    canlin_sandbox_free(a);
    canlin_sandbox_free(b);
}

static void test_require(void)
{
    static const char *const mods[] = {
        "util", "local name = ... return {x = 1, name = name}",
        "counted", "hits = (hits or 0) + 1 return {}",
        "nothing", "local y = 1",
        "loop", "return require('loop')",
        "broken", "error('broken module')",
        NULL,
    };
    host h;
    memset(&h, 0, sizeof h);
    h.mods = mods;
    expect("require", MEM, SMALL_BUDGET,
           "local u = require('util')\n"
           "assert(u.x == 1 and u.name == 'util')\n"
           "assert(require('util') == u)\n"
           "require('counted') require('counted') assert(hits == 1)\n"
           "assert(require('nothing') == true)\n"
           "local ok, e = pcall(require, 'missing') assert(not ok and e:find('not found'), e)\n"
           "ok, e = pcall(require, 'loop') assert(not ok and e:find('loading'), e)\n"
           "ok, e = pcall(require, 'broken') assert(not ok and e:find('broken module'), e)\n"
           "ok, e = pcall(require, 'broken') assert(not ok, e)\n"
           "assert(not pcall(require, ''))\n"
           "assert(not pcall(require, string.rep('a', 65)))\n"
           "assert(not pcall(require, 'ut\\0il'))\n"
           "assert(not pcall(require, 'main'))\n",
           CANLIN_SANDBOX_OK, &h);
}

int main(void)
{
    test_environment();
    test_binary_chunks();
    test_budget();
    test_memory();
    test_isolation();
    test_require();
    if (failures) {
        fprintf(stderr, "sandbox: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("sandbox: %d checks passed\n", checks);
    return 0;
}
