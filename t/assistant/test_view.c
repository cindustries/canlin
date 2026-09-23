/* Assistant view contract tests (UI spec §4, §5, §7, §8; LLM/UI design §5; event runtime
 * §4): set_view validation and JSON paths, split-input identity, tool-call identity and
 * scope, revision-scoped input, submit contents, graph round trip, rendering offscreen in
 * the shell's model-view frame. Built-in theme and font. Exits 0 on success. */
#include "canlin/assistant_view.h"
#include "canlin/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);  \
            failures++;                                                            \
        }                                                                          \
    } while (0)

#define CHECK_STR(a, b)                                                            \
    do {                                                                           \
        checks++;                                                                  \
        if (strcmp((a), (b)) != 0) {                                               \
            fprintf(stderr, "%s:%d: got  %s\n%*swant %s\n", __FILE__, __LINE__, (a), \
                    (int)strlen(__FILE__) + 6, "", (b));                           \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/* ---- validator helpers ---- */

static int parse_split(const char *json, size_t len, size_t step, canlin_av_model **m,
                       canlin_av_error *e)
{
    canlin_av_parser *p = canlin_av_parser_new();
    size_t off = 0;
    int rc;
    if (p == NULL)
        return CANLIN_AV_ENOMEM;
    while (off < len) {
        size_t n = step ? step : len;
        if (n > len - off)
            n = len - off;
        canlin_av_parser_feed(p, json + off, n);
        off += n;
    }
    rc = canlin_av_parser_finish(p, m, e);
    canlin_av_parser_free(p);
    return rc;
}

static int str_eq(canlin_av_str a, canlin_av_str b)
{
    return a.len == b.len && memcmp(a.s, b.s, a.len) == 0 && a.s[a.len] == 0 && b.s[b.len] == 0;
}

static int widgets_eq(const canlin_av_widget *a, const canlin_av_widget *b, unsigned n);

static int widget_eq(const canlin_av_widget *a, const canlin_av_widget *b)
{
    unsigned i, k;
    if (a->type != b->type || a->slot != b->slot || !str_eq(a->id, b->id) ||
        !str_eq(a->text, b->text) || !str_eq(a->label, b->label) || !str_eq(a->title, b->title) ||
        !str_eq(a->value, b->value) || !str_eq(a->placeholder, b->placeholder) ||
        a->level != b->level || a->style != b->style || a->role != b->role ||
        a->rows != b->rows || a->on != b->on || a->open != b->open || a->max != b->max ||
        a->index != b->index || a->meter != b->meter || a->height != b->height ||
        a->noptions != b->noptions || a->nchild != b->nchild || a->nchild2 != b->nchild2 ||
        a->nnodes != b->nnodes || a->nedges != b->nedges)
        return 0;
    for (i = 0; i < a->noptions; i++)
        if (!str_eq(a->options[i], b->options[i]))
            return 0;
    for (i = 0; i < a->nnodes; i++) {
        const canlin_av_node *x = &a->nodes[i], *y = &b->nodes[i];
        if (!str_eq(x->id, y->id) || !str_eq(x->title, y->title) || x->x != y->x ||
            x->y != y->y || x->nin != y->nin || x->nout != y->nout || x->nbody != y->nbody)
            return 0;
        for (k = 0; k < x->nin + x->nout; k++)
            if (!str_eq(x->ports[k].id, y->ports[k].id) ||
                !str_eq(x->ports[k].label, y->ports[k].label) ||
                !str_eq(x->ports[k].type, y->ports[k].type))
                return 0;
        for (k = 0; k < x->nbody; k++)
            if (x->body[k].meter != y->body[k].meter || x->body[k].value != y->body[k].value ||
                !str_eq(x->body[k].text, y->body[k].text))
                return 0;
    }
    for (i = 0; i < a->nedges; i++)
        if (memcmp(&a->edges[i], &b->edges[i], sizeof a->edges[i]) != 0)
            return 0;
    return widgets_eq(a->child, b->child, a->nchild) &&
           widgets_eq(a->child2, b->child2, a->nchild2);
}

static int widgets_eq(const canlin_av_widget *a, const canlin_av_widget *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (!widget_eq(&a[i], &b[i]))
            return 0;
    return 1;
}

static int model_eq(const canlin_av_model *a, const canlin_av_model *b)
{
    return str_eq(a->title, b->title) && a->nwidgets == b->nwidgets && a->total == b->total &&
           widgets_eq(a->widgets, b->widgets, a->nwidgets);
}

/* Whole, byte-by-byte and 7-byte splits give the identical outcome. */
static void check_splits(const char *json, size_t len)
{
    static const size_t steps[] = {1, 7, 64};
    canlin_av_model *m0 = NULL;
    canlin_av_error e0;
    int rc0 = parse_split(json, len, 0, &m0, &e0);
    size_t s;
    for (s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        canlin_av_model *m = NULL;
        canlin_av_error e;
        int rc = parse_split(json, len, steps[s], &m, &e);
        CHECK(rc == rc0);
        if (rc0 == CANLIN_AV_OK && rc == CANLIN_AV_OK) {
            CHECK(model_eq(m0, m));
        } else {
            CHECK(strcmp(e.path, e0.path) == 0);
            CHECK(strcmp(e.message, e0.message) == 0);
            CHECK(e.status == e0.status);
        }
        canlin_av_model_free(m);
    }
    canlin_av_model_free(m0);
}

/* ---- set_view helpers ---- */

static canlin_av_call call_of(uint64_t gen, uint64_t turn, const char *id)
{
    canlin_av_call c;
    c.gen = gen;
    c.turn = turn;
    c.id = id;
    c.id_len = strlen(id);
    return c;
}

static int setv(canlin_assistant_view *av, const char *id, const char *json, canlin_av_result *r)
{
    canlin_av_call c = call_of(1, 1, id);
    return canlin_assistant_view_set_view(av, &c, json, strlen(json), r);
}

static const char *VALID =
    "{\"title\":\"Einkaufsliste\",\"widgets\":["
    "{\"type\":\"heading\",\"text\":\"Was fehlt?\",\"level\":2},"
    "{\"type\":\"field\",\"id\":\"item\",\"label\":\"Artikel\",\"placeholder\":\"z. B. Milch\","
    "\"max\":64},"
    "{\"type\":\"choice\",\"id\":\"shop\",\"label\":\"Laden\",\"options\":[\"Markt\","
    "\"Drogerie\"]},"
    "{\"type\":\"graph\",\"id\":\"plan\",\"nodes\":[{\"id\":\"n1\",\"title\":\"HTTP\",\"x\":2,"
    "\"y\":3.5,\"outputs\":[{\"id\":\"out\",\"label\":\"body\",\"type\":\"bytes\"}],"
    "\"body\":[{\"kind\":\"text\",\"text\":\"ok\"},{\"kind\":\"meter\",\"value\":0.25}]},"
    "{\"id\":\"n2\",\"x\":16,\"y\":3,\"inputs\":[{\"id\":\"in\",\"type\":\"bytes\"}]}],"
    "\"edges\":[{\"from\":[\"n1\",\"out\"],\"to\":[\"n2\",\"in\"]}]},"
    "{\"type\":\"section\",\"title\":\"Mehr\",\"open\":false,\"widgets\":["
    "{\"type\":\"columns\",\"left\":[{\"type\":\"toggle\",\"id\":\"eilig\",\"label\":\"Eilig\","
    "\"value\":true}],\"right\":[{\"type\":\"meter\",\"label\":\"Budget\",\"value\":0.5}]}]},"
    "{\"type\":\"text\",\"text\":\"Hinweis \\u00e4\",\"style\":\"muted\"},"
    "{\"type\":\"bubble\",\"role\":\"assistant\",\"text\":\"Hallo\"},"
    "{\"type\":\"area\",\"id\":\"note\",\"label\":\"Notiz\",\"rows\":3},"
    "{\"type\":\"separator\"},"
    "{\"type\":\"button\",\"id\":\"ok\",\"label\":\"Hinzuf\\u00fcgen\",\"action\":\"submit\"}]}";

static void test_valid(void)
{
    canlin_assistant_view *av = canlin_assistant_view_new();
    canlin_av_result r;
    const canlin_av_model *m;
    const canlin_av_widget *w;
    char out[256];
    canlin_assistant_view_set_scope(av, 1, 1);
    CHECK(canlin_assistant_view_revision(av) == 0 && canlin_assistant_view_model(av) == NULL);
    CHECK(setv(av, "c1", VALID, &r) == CANLIN_AV_OK);
    CHECK(r.status == CANLIN_AV_OK && r.revision == 1 && !r.duplicate);
    CHECK(canlin_av_result_json(&r, out, sizeof out) > 0);
    CHECK_STR(out, "{\"revision\":1}");
    m = canlin_assistant_view_model(av);
    CHECK(m != NULL && m->nwidgets == 10 && m->total == 13);
    CHECK(m->title.len == 13 && memcmp(m->title.s, "Einkaufsliste", 13) == 0);
    w = m->widgets;
    CHECK(w[0].type == CANLIN_AV_HEADING && w[0].level == 2);
    CHECK(w[1].type == CANLIN_AV_FIELD && w[1].max == 64 && w[1].value.len == 0);
    CHECK(w[2].type == CANLIN_AV_CHOICE && w[2].noptions == 2 && w[2].index == 1);
    CHECK(w[3].type == CANLIN_AV_GRAPH && w[3].nnodes == 2 && w[3].nedges == 1);
    CHECK(w[3].nodes[0].y == 3.5f && w[3].nodes[0].nbody == 2 && w[3].nodes[0].body[1].meter);
    CHECK(w[3].edges[0].fn == 0 && w[3].edges[0].fp == 0 && w[3].edges[0].tn == 1 &&
          w[3].edges[0].tp == 0);
    CHECK(w[4].type == CANLIN_AV_SECTION && !w[4].open && w[4].nchild == 1);
    CHECK(w[4].child[0].type == CANLIN_AV_COLUMNS && w[4].child[0].child[0].on == 1);
    CHECK(w[5].style == 1 && w[5].text.len == 10); /* "Hinweis ä": \u00e4 decoded */
    CHECK(w[9].type == CANLIN_AV_BUTTON && w[9].slot == 12);
    check_splits(VALID, strlen(VALID));
    canlin_assistant_view_free(av);
}

/* ---- every invalid case of §7 leaves the old view and names the path ---- */

typedef struct bad_case {
    const char *json;
    const char *path;
} bad_case;

static const bad_case BAD[] = {
    {"[]", "$"},
    {"{}", "$.widgets"},
    {"{\"widgets\":{}}", "$.widgets"},
    {"{\"widgets\":[],\"script\":\"x\"}", "$.script"},
    {"{\"widgets\":[1]}", "$.widgets[0]"},
    {"{\"widgets\":[{\"text\":\"a\"}]}", "$.widgets[0].type"},
    {"{\"widgets\":[{\"type\":\"html\"}]}", "$.widgets[0].type"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\",\"onclick\":\"x\"}]}",
     "$.widgets[0].onclick"},
    {"{\"widgets\":[{\"type\":\"text\",\"type\":\"text\"}]}", "$.widgets[0].type"},
    {"{\"widgets\":[{\"type\":\"separator\"},{\"type\":\"field\",\"label\":\"a\"}]}",
     "$.widgets[1].id"},
    {"{\"widgets\":[{\"type\":\"field\",\"id\":\"Item\",\"label\":\"a\"}]}", "$.widgets[0].id"},
    {"{\"widgets\":[{\"type\":\"field\",\"id\":\"a b\",\"label\":\"a\"}]}", "$.widgets[0].id"},
    {"{\"widgets\":[{\"type\":\"field\",\"id\":\"abcdefghijklmnopqrstuvwxyz0123456\","
     "\"label\":\"a\"}]}",
     "$.widgets[0].id"},
    {"{\"widgets\":[{\"type\":\"button\",\"id\":\"ok\",\"label\":\"a\"}]}",
     "$.widgets[0].action"},
    {"{\"widgets\":[{\"type\":\"button\",\"id\":\"ok\",\"label\":\"a\",\"action\":\"delete\"}]}",
     "$.widgets[0].action"},
    {"{\"widgets\":[{\"type\":\"button\",\"id\":\"ok\",\"label\":\"a\",\"action\":\"submit\","
     "\"url\":\"http://x\"}]}",
     "$.widgets[0].url"},
    {"{\"widgets\":[{\"type\":\"heading\",\"text\":\"a\",\"level\":4}]}", "$.widgets[0].level"},
    {"{\"widgets\":[{\"type\":\"heading\",\"text\":\"a\",\"level\":1.5}]}", "$.widgets[0].level"},
    {"{\"widgets\":[{\"type\":\"heading\",\"text\":7}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\",\"style\":\"blink\"}]}",
     "$.widgets[0].style"},
    {"{\"widgets\":[{\"type\":\"bubble\",\"role\":\"settings\",\"text\":\"a\"}]}",
     "$.widgets[0].role"},
    {"{\"widgets\":[{\"type\":\"meter\",\"label\":\"a\",\"value\":2}]}", "$.widgets[0].value"},
    {"{\"widgets\":[{\"type\":\"meter\",\"label\":\"a\"}]}", "$.widgets[0].value"},
    {"{\"widgets\":[{\"type\":\"choice\",\"id\":\"c\",\"label\":\"a\",\"options\":[]}]}",
     "$.widgets[0].options"},
    {"{\"widgets\":[{\"type\":\"choice\",\"id\":\"c\",\"label\":\"a\",\"options\":[\"x\",3]}]}",
     "$.widgets[0].options[1]"},
    {"{\"widgets\":[{\"type\":\"choice\",\"id\":\"c\",\"label\":\"a\",\"options\":[\"x\"],"
     "\"index\":2}]}",
     "$.widgets[0].index"},
    {"{\"widgets\":[{\"type\":\"field\",\"id\":\"f\",\"label\":\"a\",\"max\":2,\"value\":\"abc\"}]}",
     "$.widgets[0].value"},
    {"{\"widgets\":[{\"type\":\"area\",\"id\":\"f\",\"label\":\"a\",\"rows\":21}]}",
     "$.widgets[0].rows"},
    {"{\"widgets\":[{\"type\":\"toggle\",\"id\":\"t\",\"label\":\"a\",\"value\":1}]}",
     "$.widgets[0].value"},
    {"{\"widgets\":[{\"type\":\"section\",\"title\":\"s\"}]}", "$.widgets[0].widgets"},
    {"{\"widgets\":[{\"type\":\"columns\",\"left\":[]}]}", "$.widgets[0].right"},
    {"{\"widgets\":[{\"type\":\"columns\",\"left\":[{\"type\":\"field\",\"id\":\"x\","
     "\"label\":\"a\"}],\"right\":[{\"type\":\"toggle\",\"id\":\"x\",\"label\":\"b\"}]}]}",
     "$.widgets[0].right[0].id"},
    {"{\"widgets\":[{\"type\":\"icon\",\"name\":\"logo\"}]}", "$.widgets[0].name"},
    /* control characters in any string: C0, DEL, C1, also in labels and titles */
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\\u0007b\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\\nb\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\\u007fb\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\\u0085b\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\\u0000b\"}]}", "$.widgets[0].text"},
    {"{\"title\":\"a\\tb\",\"widgets\":[]}", "$.title"},
    {"{\"widgets\":[{\"type\":\"choice\",\"id\":\"c\",\"label\":\"a\",\"options\":[\"x\\ry\"]}]}",
     "$.widgets[0].options[0]"},
    /* malformed UTF-8 (raw bytes) and lone surrogate escapes */
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"a\xff\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"\xc0\xaf\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[{\"type\":\"text\",\"text\":\"\\ud800\"}]}", "$.widgets[0].text"},
    {"{\"widgets\":[]} x", "$"},
    {"{\"widgets\":[", "$.widgets[0]"},
    /* graph rules (§5, §8) */
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0},"
     "{\"id\":\"a\",\"x\":0,\"y\":0}],\"edges\":[]}]}",
     "$.widgets[0].nodes[1].id"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0}],"
     "\"edges\":[]}]}",
     "$.widgets[0].nodes[0].y"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":5000,"
     "\"y\":0}],\"edges\":[]}]}",
     "$.widgets[0].nodes[0].x"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[],\"edges\":[],\"height\":2}]}",
     "$.widgets[0].height"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"inputs\":[{\"id\":\"p\"},{\"id\":\"p\"}]}],\"edges\":[]}]}",
     "$.widgets[0].nodes[0].inputs[1].id"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"body\":[{\"kind\":\"image\"}]}],\"edges\":[]}]}",
     "$.widgets[0].nodes[0].body[0].kind"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"outputs\":[{\"id\":\"o\"}]}],\"edges\":[{\"from\":[\"a\",\"o\"],\"to\":[\"b\",\"i\"]}]}]}",
     "$.widgets[0].edges[0].to[0]"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"outputs\":[{\"id\":\"o\"}]},{\"id\":\"b\",\"x\":0,\"y\":0,\"inputs\":[{\"id\":\"i\"}]}],"
     "\"edges\":[{\"from\":[\"a\",\"x\"],\"to\":[\"b\",\"i\"]}]}]}",
     "$.widgets[0].edges[0].from[1]"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"outputs\":[{\"id\":\"o\"}]},{\"id\":\"b\",\"x\":0,\"y\":0,\"inputs\":[{\"id\":\"i\"}]}],"
     "\"edges\":[{\"from\":[\"b\",\"i\"],\"to\":[\"a\",\"o\"]}]}]}",
     "$.widgets[0].edges[0].from[1]"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"inputs\":[{\"id\":\"i\"}],\"outputs\":[{\"id\":\"o\"}]}],"
     "\"edges\":[{\"from\":[\"a\",\"o\"],\"to\":[\"a\",\"i\"]}]}]}",
     "$.widgets[0].edges[0]"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"outputs\":[{\"id\":\"o\"}]},{\"id\":\"b\",\"x\":0,\"y\":0,\"inputs\":[{\"id\":\"i\"}]}],"
     "\"edges\":[{\"from\":[\"a\",\"o\"],\"to\":[\"b\",\"i\"]},"
     "{\"from\":[\"a\",\"o\"],\"to\":[\"b\",\"i\"]}]}]}",
     "$.widgets[0].edges[1]"},
    {"{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,\"y\":0,"
     "\"outputs\":[{\"id\":\"o\",\"type\":\"num\"}]},{\"id\":\"b\",\"x\":0,\"y\":0,"
     "\"inputs\":[{\"id\":\"i\",\"type\":\"text\"}]}],"
     "\"edges\":[{\"from\":[\"a\",\"o\"],\"to\":[\"b\",\"i\"]}]}]}",
     "$.widgets[0].edges[0]"},
};

static void test_invalid(void)
{
    canlin_assistant_view *av = canlin_assistant_view_new();
    canlin_av_result r;
    const canlin_av_model *before;
    char id[16], out[512];
    size_t i;
    canlin_assistant_view_set_scope(av, 1, 1);
    CHECK(setv(av, "base", VALID, &r) == CANLIN_AV_OK);
    before = canlin_assistant_view_model(av);
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        /* a fresh scope per case keeps the call cache small; the view persists */
        canlin_av_call c = call_of(1, 100 + i, "bad");
        canlin_assistant_view_set_scope(av, 1, 100 + i);
        snprintf(id, sizeof id, "bad%u", (unsigned)i);
        (void)id;
        CHECK(canlin_assistant_view_set_view(av, &c, BAD[i].json, strlen(BAD[i].json), &r) ==
              CANLIN_AV_EINVALID);
        CHECK(r.status == CANLIN_AV_EINVALID && r.revision == 0);
        if (strcmp(r.err.path, BAD[i].path) != 0)
            fprintf(stderr, "case %u: %s\n", (unsigned)i, BAD[i].json);
        CHECK_STR(r.err.path, BAD[i].path);
        CHECK(r.err.message[0] != 0);
        CHECK(canlin_assistant_view_revision(av) == 1);
        CHECK(canlin_assistant_view_model(av) == before);
        CHECK(canlin_av_result_json(&r, out, sizeof out) > 0);
        CHECK(strstr(out, "\"code\":\"invalid\"") != NULL);
        check_splits(BAD[i].json, strlen(BAD[i].json));
    }
    canlin_assistant_view_free(av);
}

/* Widget count, container depth, argument size and graph counts at and over the limit. */
static char big[CANLIN_UI_TOOL_MAX_BYTES + 64];

static void seps(unsigned n)
{
    size_t len;
    unsigned i;
    strcpy(big, "{\"widgets\":[");
    len = strlen(big);
    for (i = 0; i < n; i++) {
        memcpy(big + len, i ? ",{\"type\":\"separator\"}" : "{\"type\":\"separator\"}",
               i ? 21 : 20);
        len += i ? 21 : 20;
    }
    memcpy(big + len, "]}", 3);
}

static void nest(unsigned depth)
{
    unsigned i;
    strcpy(big, "{\"widgets\":[");
    for (i = 0; i < depth; i++)
        strcat(big, i & 1 ? "{\"type\":\"columns\",\"right\":[],\"left\":["
                          : "{\"type\":\"section\",\"title\":\"s\",\"widgets\":[");
    strcat(big, "{\"type\":\"separator\"}");
    for (i = 0; i < depth; i++)
        strcat(big, "]}");
    strcat(big, "]}");
}

static void test_limits(void)
{
    canlin_assistant_view *av = canlin_assistant_view_new();
    canlin_av_result r;
    canlin_av_call c;
    size_t len, i;
    canlin_assistant_view_set_scope(av, 1, 1);

    seps(CANLIN_UI_TOOL_MAX_WIDGETS);
    CHECK(setv(av, "w128", big, &r) == CANLIN_AV_OK && r.revision == 1);
    seps(CANLIN_UI_TOOL_MAX_WIDGETS + 1);
    CHECK(setv(av, "w129", big, &r) == CANLIN_AV_EINVALID);
    CHECK_STR(r.err.path, "$.widgets[128]");
    check_splits(big, strlen(big));

    nest(CANLIN_UI_MAX_DEPTH);
    CHECK(setv(av, "d4", big, &r) == CANLIN_AV_OK && r.revision == 2);
    nest(CANLIN_UI_MAX_DEPTH + 1);
    CHECK(setv(av, "d5", big, &r) == CANLIN_AV_EINVALID);
    CHECK_STR(r.err.path, "$.widgets[0].widgets[0].left[0].widgets[0].left[0]");
    check_splits(big, strlen(big));

    /* oversize: rejected before parsing, and by the tokenizer when pushed */
    memset(big, ' ', sizeof big);
    memcpy(big, "{\"widgets\":[]}", 14);
    len = CANLIN_UI_TOOL_MAX_BYTES + 1;
    c = call_of(1, 1, "big");
    CHECK(canlin_assistant_view_set_view(av, &c, big, len, &r) == CANLIN_AV_EINVALID);
    CHECK_STR(r.err.path, "$");
    CHECK(canlin_assistant_view_revision(av) == 2);
    {
        canlin_av_model *m = NULL;
        canlin_av_error e;
        CHECK(parse_split(big, len, 4096, &m, &e) == CANLIN_AV_EINVALID && m == NULL);
        CHECK(strstr(e.message, "exceeds") != NULL);
        CHECK(parse_split(big, len - 1, 4096, &m, &e) == CANLIN_AV_OK && m != NULL);
        canlin_av_model_free(m);
    }

    /* more graphs than one draw may hold */
    strcpy(big, "{\"widgets\":[");
    for (i = 0; i <= CANLIN_UI_GRAPH_MAX_PER_DRAW; i++) {
        char w[96];
        snprintf(w, sizeof w, "%s{\"type\":\"graph\",\"id\":\"g%u\",\"nodes\":[],\"edges\":[]}",
                 i ? "," : "", (unsigned)i);
        strcat(big, w);
    }
    strcat(big, "]}");
    CHECK(setv(av, "g9", big, &r) == CANLIN_AV_EINVALID);
    CHECK_STR(r.err.path, "$.widgets[8]");

    /* too many ports on one node */
    strcpy(big, "{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[{\"id\":\"a\",\"x\":0,"
                "\"y\":0,\"inputs\":[");
    for (i = 0; i <= CANLIN_UI_GRAPH_MAX_PORTS; i++) {
        char w[32];
        snprintf(w, sizeof w, "%s{\"id\":\"p%u\"}", i ? "," : "", (unsigned)i);
        strcat(big, w);
    }
    strcat(big, "]}],\"edges\":[]}]}");
    CHECK(setv(av, "ports", big, &r) == CANLIN_AV_EINVALID);
    CHECK_STR(r.err.path, "$.widgets[0].nodes[0]");

    /* too many nodes */
    strcpy(big, "{\"widgets\":[{\"type\":\"graph\",\"id\":\"g\",\"nodes\":[");
    for (i = 0; i <= CANLIN_UI_GRAPH_MAX_NODES; i++) {
        char w[48];
        snprintf(w, sizeof w, "%s{\"id\":\"n%u\",\"x\":0,\"y\":0}", i ? "," : "", (unsigned)i);
        strcat(big, w);
    }
    strcat(big, "],\"edges\":[]}]}");
    CHECK(setv(av, "nodes", big, &r) == CANLIN_AV_EINVALID);
    CHECK_STR(r.err.path, "$.widgets[0].nodes");
    CHECK(canlin_assistant_view_revision(av) == 2);
    canlin_assistant_view_free(av);
}

/* ---- tool-call identity and scope ---- */

static void test_calls(void)
{
    canlin_assistant_view *av = canlin_assistant_view_new();
    canlin_av_result r;
    canlin_av_call c;
    const canlin_av_model *m1;
    const char *v2 = "{\"widgets\":[{\"type\":\"text\",\"text\":\"two\"}]}";
    char out[256];
    unsigned i;

    /* no turn yet: stale, not recorded */
    c = call_of(1, 1, "c1");
    CHECK(canlin_assistant_view_set_view(av, &c, VALID, strlen(VALID), &r) == CANLIN_AV_ESTALE);
    CHECK(r.status == CANLIN_AV_ESTALE && r.revision == 0);
    CHECK(canlin_av_result_json(&r, out, sizeof out) > 0 && strstr(out, "\"stale\"") != NULL);

    canlin_assistant_view_set_scope(av, 1, 1);
    CHECK(canlin_assistant_view_set_view(av, &c, VALID, strlen(VALID), &r) == CANLIN_AV_OK);
    CHECK(r.revision == 1 && !r.duplicate);
    m1 = canlin_assistant_view_model(av);

    /* duplicate receipt: recorded result, not applied again */
    CHECK(canlin_assistant_view_set_view(av, &c, VALID, strlen(VALID), &r) == CANLIN_AV_OK);
    CHECK(r.revision == 1 && r.duplicate == 1);
    CHECK(canlin_assistant_view_revision(av) == 1 && canlin_assistant_view_model(av) == m1);

    /* same id, other arguments: refused, not applied */
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_EINVALID);
    CHECK(canlin_assistant_view_revision(av) == 1);

    /* a failed call is recorded too: its duplicate returns the same error */
    c = call_of(1, 1, "c2");
    CHECK(canlin_assistant_view_set_view(av, &c, "{}", 2, &r) == CANLIN_AV_EINVALID);
    CHECK(!r.duplicate);
    CHECK(canlin_assistant_view_set_view(av, &c, "{}", 2, &r) == CANLIN_AV_EINVALID);
    CHECK(r.duplicate == 1);
    CHECK_STR(r.err.path, "$.widgets");

    /* other generation or turn: stale, never success, nothing applied */
    c = call_of(2, 1, "c3");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_ESTALE);
    c = call_of(1, 0, "c3");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_ESTALE);
    c = call_of(1, 2, "c3");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_ESTALE);
    CHECK(canlin_assistant_view_revision(av) == 1 && canlin_assistant_view_model(av) == m1);

    /* the turn moves on: calls of the old turn are stale (a cancelled turn delivers
     * nothing later), and the new turn has its own identity scope */
    canlin_assistant_view_set_scope(av, 1, 2);
    c = call_of(1, 1, "c9");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_ESTALE);
    c = call_of(1, 2, "c1");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_OK);
    CHECK(r.revision == 2 && !r.duplicate);

    /* bounded retention: CANLIN_AV_CALLS_MAX results per turn, then "full" */
    canlin_assistant_view_set_scope(av, 1, 3);
    for (i = 0; i < CANLIN_AV_CALLS_MAX; i++) {
        char id[16];
        snprintf(id, sizeof id, "k%u", i);
        c = call_of(1, 3, id);
        CHECK(canlin_assistant_view_set_view(av, &c, "{}", 2, &r) == CANLIN_AV_EINVALID);
    }
    c = call_of(1, 3, "one-more");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_EFULL);
    CHECK(canlin_assistant_view_revision(av) == 2);
    c = call_of(1, 3, "k0"); /* recorded ones still answer */
    CHECK(canlin_assistant_view_set_view(av, &c, "{}", 2, &r) == CANLIN_AV_EINVALID &&
          r.duplicate);

    /* call id bounds */
    c = call_of(1, 3, "");
    CHECK(canlin_assistant_view_set_view(av, &c, v2, strlen(v2), &r) == CANLIN_AV_EINVALID);
    canlin_assistant_view_free(av);
}

/* ---- rendering, input and submit on an offscreen view ---- */

typedef struct vh {
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_ui_view *v;
    canlin_assistant_view *av;
    uint64_t t;
    int n;
} vh;

static void vh_init(vh *h, int w, int hh)
{
    canlin_theme t;
    canlin_ui_output_desc d;
    memset(h, 0, sizeof *h);
    canlin_theme_builtin(&t);
    h->ui = canlin_ui_new(&t);
    d.id = "av-test";
    d.w = w;
    d.h = hh;
    d.dpi = 1.0f;
    d.scale = 100;
    h->out = canlin_ui_output_new(&d, NULL);
    h->v = h->ui && h->out ? canlin_ui_view_new(h->ui, h->out, NULL) : NULL;
    h->av = canlin_assistant_view_new();
    if (!h->v || !h->av) {
        fprintf(stderr, "harness init failed\n");
        exit(1);
    }
    canlin_ui_view_set_rect(h->v, 0, 0, w, hh);
    canlin_assistant_view_attach(h->av, h->v);
    canlin_assistant_view_set_scope(h->av, 1, 1);
}

static void vh_done(vh *h)
{
    canlin_assistant_view_free(h->av);
    canlin_ui_view_free(h->v);
    CHECK(canlin_ui_output_free(h->out) == CANLIN_UI_OK);
    canlin_ui_free(h->ui);
}

static void vh_set(vh *h, const char *json)
{
    canlin_av_result r;
    char id[16];
    snprintf(id, sizeof id, "call%d", h->n++);
    CHECK(setv(h->av, id, json, &r) == CANLIN_AV_OK);
    if (r.status != CANLIN_AV_OK)
        fprintf(stderr, "set_view: %s %s\n", r.err.path, r.err.message);
}

/* Frames until the queued input is applied, plus one. */
static void pump(vh *h)
{
    int i;
    for (i = 0; i < 64; i++) {
        CHECK(canlin_assistant_view_frame(h->av) == CANLIN_AV_OK);
        if (canlin_ui_view_pending(h->v) == 0)
            break;
    }
    CHECK(canlin_assistant_view_frame(h->av) == CANLIN_AV_OK);
}

static int post(vh *h, uint32_t rev, canlin_ui_input_type type, int x, int y, int down)
{
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = type;
    in.x = x;
    in.y = y;
    in.down = down;
    h->t += 20;
    in.t = h->t;
    return canlin_assistant_view_input(h->av, rev, &in);
}

static void click(vh *h, int x, int y)
{
    uint32_t rev = canlin_assistant_view_revision(h->av);
    CHECK(post(h, rev, CANLIN_UI_IN_MOTION, x, y, 0) == CANLIN_AV_OK);
    CHECK(post(h, rev, CANLIN_UI_IN_BUTTON, x, y, 1) == CANLIN_AV_OK);
    pump(h);
    CHECK(post(h, rev, CANLIN_UI_IN_BUTTON, x, y, 0) == CANLIN_AV_OK);
    pump(h);
    h->t += 1000; /* the next tap is neither a double tap nor a long press */
}

static void key(vh *h, int k)
{
    uint32_t rev = canlin_assistant_view_revision(h->av);
    canlin_ui_input in;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_KEY;
    in.key = k;
    in.down = 1;
    CHECK(canlin_assistant_view_input(h->av, rev, &in) == CANLIN_AV_OK);
    pump(h);
    in.down = 0;
    CHECK(canlin_assistant_view_input(h->av, rev, &in) == CANLIN_AV_OK);
    pump(h);
}

static void type_text(vh *h, const char *s)
{
    uint32_t rev = canlin_assistant_view_revision(h->av);
    for (; *s; s++) {
        canlin_ui_input in;
        memset(&in, 0, sizeof in);
        in.type = CANLIN_UI_IN_TEXT;
        in.cp = (unsigned char)*s;
        CHECK(canlin_assistant_view_input(h->av, rev, &in) == CANLIN_AV_OK);
    }
    pump(h);
}

/* n-th (from 0) record entry of kind/part. */
static const canlin_ui_rect *find(vh *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(h->v, &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
            return &r[i];
    return NULL;
}

static const canlin_ui_rect *gfind(vh *h, int part, unsigned node, unsigned port)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(h->v, &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_GRAPH && r[i].part == part && r[i].off == node &&
            (part != CANLIN_UI_P_PORT || r[i].len == port))
            return &r[i];
    return NULL;
}

static void cx(const canlin_ui_rect *r, int *x, int *y)
{
    *x = r->x + r->w / 2;
    *y = r->y + r->h / 2;
}

static const char *BTN_A = "{\"widgets\":[{\"type\":\"button\",\"id\":\"ok\",\"label\":\"Send\","
                           "\"action\":\"submit\"}]}";
static const char *BTN_B = "{\"title\":\"B\",\"widgets\":[{\"type\":\"button\",\"id\":\"ok\","
                           "\"label\":\"Delete all\",\"action\":\"submit\"}]}";

static void test_stale_click(void)
{
    vh h;
    const canlin_ui_rect *b;
    canlin_av_submission s;
    int x, y;
    vh_init(&h, 600, 400);
    CHECK(post(&h, 0, CANLIN_UI_IN_MOTION, 1, 1, 0) == CANLIN_AV_ESTALE); /* no view yet */
    vh_set(&h, BTN_A);
    pump(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (b == NULL) {
        vh_done(&h);
        return;
    }
    cx(b, &x, &y);

    /* a click queued against revision 1 ... */
    CHECK(post(&h, 1, CANLIN_UI_IN_MOTION, x, y, 0) == CANLIN_AV_OK);
    CHECK(post(&h, 1, CANLIN_UI_IN_BUTTON, x, y, 1) == CANLIN_AV_OK);
    /* ... is overtaken by revision 2, whose button reuses id "ok" at the same place */
    vh_set(&h, BTN_B);
    CHECK(canlin_assistant_view_revision(h.av) == 2);
    pump(&h);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL && b->x + b->w / 2 == x && b->y + b->h / 2 == y);
    CHECK(!canlin_assistant_view_pending(h.av));
    /* later events of the old revision are dropped, also a whole click */
    CHECK(post(&h, 1, CANLIN_UI_IN_BUTTON, x, y, 0) == CANLIN_AV_ESTALE);
    CHECK(post(&h, 1, CANLIN_UI_IN_BUTTON, x, y, 1) == CANLIN_AV_ESTALE);
    CHECK(post(&h, 3, CANLIN_UI_IN_BUTTON, x, y, 1) == CANLIN_AV_ESTALE);
    pump(&h);
    CHECK(!canlin_assistant_view_pending(h.av));
    CHECK(canlin_assistant_view_submit_refused(h.av) == 0);

    /* a click on revision 2 submits revision 2 */
    click(&h, x, y);
    CHECK(canlin_assistant_view_pending(h.av));
    CHECK(canlin_assistant_view_take(h.av, &s) == 1);
    CHECK(s.revision == 2 && strcmp(s.action, "ok") == 0);
    CHECK_STR(s.json, "{\"revision\":2,\"action\":\"ok\",\"values\":{}}");
    canlin_av_submission_free(&s);
    CHECK(canlin_assistant_view_take(h.av, &s) == 0);
    vh_done(&h);
}

static const char *FORM =
    "{\"title\":\"Form\",\"widgets\":["
    "{\"type\":\"heading\",\"text\":\"Order\"},"
    "{\"type\":\"text\",\"text\":\"Fill in\",\"style\":\"muted\"},"
    "{\"type\":\"field\",\"id\":\"item\",\"label\":\"Item\",\"value\":\"Milk\"},"
    "{\"type\":\"toggle\",\"id\":\"urgent\",\"label\":\"Urgent\"},"
    "{\"type\":\"choice\",\"id\":\"shop\",\"label\":\"Shop\",\"options\":[\"Market\","
    "\"Drugstore\"],\"index\":2},"
    "{\"type\":\"meter\",\"label\":\"Budget\",\"value\":0.5},"
    "{\"type\":\"section\",\"title\":\"More\",\"widgets\":[{\"type\":\"area\",\"id\":\"note\","
    "\"label\":\"Note\",\"value\":\"a\\\"b\\\\c\"}]},"
    "{\"type\":\"columns\",\"left\":[{\"type\":\"separator\"}],\"right\":[{\"type\":\"bubble\","
    "\"role\":\"assistant\",\"text\":\"hi\"}]},"
    "{\"type\":\"button\",\"id\":\"ok\",\"label\":\"Add\",\"action\":\"submit\"},"
    "{\"type\":\"button\",\"id\":\"cancel\",\"label\":\"Cancel\",\"action\":\"submit\"}]}";

static void test_submit(void)
{
    vh h;
    const canlin_ui_rect *f, *t, *b;
    canlin_av_submission s;
    int x, y;
    vh_init(&h, 600, 1400);
    vh_set(&h, FORM);
    pump(&h);
    f = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
    t = find(&h, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, 0);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(f && t && b && find(&h, CANLIN_UI_W_AREA, CANLIN_UI_P_BOX, 0) &&
          find(&h, CANLIN_UI_W_HEADING, CANLIN_UI_P_LINE, 0) &&
          find(&h, CANLIN_UI_W_BUBBLE, CANLIN_UI_P_BOX, 0) &&
          find(&h, CANLIN_UI_W_METER, CANLIN_UI_P_BOX, 0));
    if (!f || !t || !b) {
        vh_done(&h);
        return;
    }
    /* local edits: focus the field at its end, type, flip the toggle */
    click(&h, f->x + f->w - 4, f->y + f->h / 2);
    key(&h, CANLIN_UI_KEY_END);
    type_text(&h, "s!");
    t = find(&h, CANLIN_UI_W_TOGGLE, CANLIN_UI_P_BOX, 0);
    cx(t, &x, &y);
    click(&h, x, y);
    CHECK(!canlin_assistant_view_pending(h.av)); /* nothing leaves before submit */

    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    cx(b, &x, &y);
    click(&h, x, y);
    CHECK(canlin_assistant_view_take(h.av, &s) == 1);
    CHECK(s.revision == 1 && s.gen == 1 && s.turn == 1 && strcmp(s.action, "ok") == 0);
    CHECK_STR(s.json, "{\"revision\":1,\"action\":\"ok\",\"values\":{\"item\":\"Milks!\","
                      "\"urgent\":true,\"shop\":2,\"note\":\"a\\\"b\\\\c\"}}");
    CHECK(s.len == strlen(s.json));
    canlin_av_submission_free(&s);

    /* exactly one submission per press: a second press while one is pending is refused */
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 1);
    cx(b, &x, &y);
    click(&h, x, y);
    click(&h, x, y);
    CHECK(canlin_assistant_view_submit_refused(h.av) == 1);
    CHECK(canlin_assistant_view_take(h.av, &s) == 1);
    CHECK(strcmp(s.action, "cancel") == 0);
    canlin_av_submission_free(&s);
    CHECK(!canlin_assistant_view_pending(h.av));
    vh_done(&h);
}

static const char *GRAPH =
    "{\"widgets\":[{\"type\":\"graph\",\"id\":\"plan\",\"height\":20,\"nodes\":["
    "{\"id\":\"a\",\"title\":\"A\",\"x\":1,\"y\":1,\"outputs\":[{\"id\":\"out\","
    "\"type\":\"num\"}]},"
    "{\"id\":\"b\",\"title\":\"B\",\"x\":20,\"y\":1,\"inputs\":[{\"id\":\"in\","
    "\"type\":\"num\"}]}],\"edges\":[]},"
    "{\"type\":\"button\",\"id\":\"go\",\"label\":\"Go\",\"action\":\"submit\"}]}";

static void drag(vh *h, int x0, int y0, int x1, int y1)
{
    uint32_t rev = canlin_assistant_view_revision(h->av);
    CHECK(post(h, rev, CANLIN_UI_IN_MOTION, x0, y0, 0) == CANLIN_AV_OK);
    CHECK(post(h, rev, CANLIN_UI_IN_BUTTON, x0, y0, 1) == CANLIN_AV_OK);
    pump(h);
    CHECK(post(h, rev, CANLIN_UI_IN_MOTION, (x0 + x1) / 2, (y0 + y1) / 2, 0) == CANLIN_AV_OK);
    pump(h);
    CHECK(post(h, rev, CANLIN_UI_IN_MOTION, x1, y1, 0) == CANLIN_AV_OK);
    pump(h);
    CHECK(post(h, rev, CANLIN_UI_IN_BUTTON, x1, y1, 0) == CANLIN_AV_OK);
    pump(h);
    h->t += 1000;
}

static void test_graph_round_trip(void)
{
    vh h;
    const canlin_ui_rect *po, *pin, *na, *b;
    canlin_av_submission s;
    int x0, y0, x1, y1;
    float em;
    vh_init(&h, 800, 700);
    vh_set(&h, GRAPH);
    pump(&h);
    em = canlin_ui_view_em(h.v);
    po = gfind(&h, CANLIN_UI_P_PORT, 0, 0);
    pin = gfind(&h, CANLIN_UI_P_PORT, 1, 0);
    CHECK(po && pin);
    if (!po || !pin) {
        vh_done(&h);
        return;
    }
    cx(po, &x0, &y0);
    cx(pin, &x1, &y1);
    drag(&h, x0, y0, x1, y1); /* connect a.out -> b.in, applied by the view itself */
    na = gfind(&h, CANLIN_UI_P_NODE, 0, 0);
    CHECK(na != NULL);
    if (na) {
        x0 = na->x + (int)(3 * em);
        y0 = na->y + (int)(em);
        drag(&h, x0, y0, x0 + (int)(2 * em), y0 + (int)(em)); /* move a by (+2, +1) em */
    }
    CHECK(!canlin_assistant_view_pending(h.av)); /* dragging sends nothing */
    na = gfind(&h, CANLIN_UI_P_NODE, 0, 0);
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    if (b) {
        cx(b, &x0, &y0);
        click(&h, x0, y0);
    }
    CHECK(canlin_assistant_view_take(h.av, &s) == 1);
    if (s.json)
        CHECK_STR(s.json,
                  "{\"revision\":1,\"action\":\"go\",\"values\":{\"plan\":{\"nodes\":["
                  "{\"id\":\"a\",\"title\":\"A\",\"x\":3,\"y\":2,\"inputs\":[],\"outputs\":["
                  "{\"id\":\"out\",\"type\":\"num\"}]},"
                  "{\"id\":\"b\",\"title\":\"B\",\"x\":20,\"y\":1,\"inputs\":[{\"id\":\"in\","
                  "\"type\":\"num\"}],\"outputs\":[]}],"
                  "\"edges\":[{\"from\":[\"a\",\"out\"],\"to\":[\"b\",\"in\"]}]}}}");
    canlin_av_submission_free(&s);
    /* the model's own data is untouched: a new revision starts from its JSON again */
    CHECK(canlin_assistant_view_model(h.av)->widgets[0].nedges == 0);
    CHECK(canlin_assistant_view_model(h.av)->widgets[0].nodes[0].x == 1.0f);
    vh_done(&h);
}

/* ---- offscreen inside the shell's model-view frame ---- */

#define SW 900
#define SH 700
static unsigned char px[SW * SH * 3];
static unsigned char dirty[SH];

static canlin_module *shell_mod;

static void on_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    (void)ud;
    (void)ev;
    if (shell_mod && canlin_module_gen(shell_mod) == d->gen)
        canlin_module_deliver(shell_mod, d);
}

static void shell_drain(canlin_ev *ev)
{
    canlin_ev_turn_result tr;
    int k;
    for (k = 0; k < 32; k++)
        CHECK(canlin_ev_turn(ev, &tr) == CANLIN_EV_OK);
}

/* The assistant view drawn into a module view the shell frames as model-generated: the
 * label is shown above it, the view's widgets lie inside the view rect below the label,
 * and the rasterized frame has pixels of both. */
static void test_shell_frame(void)
{
    static const char *SRC = "local M = {}\nfunction M.init() end\nreturn M\n";
    canlin_ev_fake_clock clock;
    canlin_ev_clock c;
    canlin_ev_callbacks cb;
    canlin_theme t;
    canlin_ui_output_desc d;
    canlin_shell_config sc;
    canlin_sandbox_config cfg;
    canlin_module_source src;
    canlin_ev *ev;
    canlin_ui *ui;
    canlin_ui_output *out;
    canlin_shell *sh;
    canlin_assistant_view *av;
    canlin_render r;
    canlin_shell_rect lab, view;
    canlin_ui_view *v;
    canlin_av_result res;
    const canlin_ui_rect *rec;
    size_t n, i;
    int err = 0, heading = 0, button = 0, changed = 0, x, y;
    unsigned char bg[3];

    c = canlin_ev_fake_clock_bind(&clock);
    memset(&cb, 0, sizeof cb);
    cb.deliver = on_deliver;
    ev = canlin_ev_new(&c, &cb, NULL);
    canlin_theme_builtin(&t);
    ui = canlin_ui_new(&t);
    d.id = "shell-av";
    d.w = SW;
    d.h = SH;
    d.dpi = 1.0f;
    d.scale = 100;
    out = canlin_ui_output_new(&d, NULL);
    memset(&sc, 0, sizeof sc);
    sc.ui = ui;
    sh = ui ? canlin_shell_new(&sc, &err) : NULL;
    if (!ev || !ui || !out || !sh) {
        fprintf(stderr, "shell harness init failed\n");
        exit(1);
    }
    CHECK(canlin_shell_add_output(sh, out, CANLIN_SHELL_IN_POINTER) == CANLIN_SHELL_OK);
    memset(&src, 0, sizeof src);
    memset(&cfg, 0, sizeof cfg);
    src.name = "assistant";
    src.version = "1";
    src.main = SRC;
    src.main_len = strlen(SRC);
    shell_mod = canlin_module_load(ev, &src, &cfg, &err);
    if (!shell_mod) {
        fprintf(stderr, "module load failed (%d)\n", err);
        exit(1);
    }
    CHECK(canlin_shell_add_module(sh, shell_mod, &src) == CANLIN_SHELL_OK);
    CHECK(canlin_shell_set_model_view(sh, shell_mod, 1) == CANLIN_SHELL_OK);
    CHECK(canlin_render_init(&r, px, SW, SH, (size_t)SW * 3, dirty, NULL) == 0);
    canlin_shell_update(sh);
    shell_drain(ev); /* the module's own (empty) draw */
    v = canlin_module_view(shell_mod);
    CHECK(v != NULL);

    av = canlin_assistant_view_new();
    canlin_assistant_view_attach(av, v);
    canlin_assistant_view_set_scope(av, 7, 1);
    {
        canlin_av_call call = call_of(7, 1, "s1");
        CHECK(canlin_assistant_view_set_view(av, &call, FORM, strlen(FORM), &res) == CANLIN_AV_OK);
    }
    CHECK(canlin_assistant_view_frame(av) == CANLIN_AV_OK);

    CHECK(canlin_shell_update(sh) == 1 || canlin_shell_dirty(sh, out));
    canlin_render_begin(&r);
    canlin_shell_render(sh, out, &r);
    canlin_render_end(&r);

    CHECK(canlin_shell_part_rect(sh, out, CANLIN_SHELL_P_MODEL_LABEL, 0, &lab) == 1);
    CHECK(canlin_shell_part_rect(sh, out, CANLIN_SHELL_P_VIEW, 0, &view) == 1);
    CHECK(lab.h > 0 && view.y >= lab.y + lab.h);
    rec = canlin_ui_view_record(v, &n, NULL);
    for (i = 0; i < n; i++) {
        if (rec[i].kind == CANLIN_UI_W_HEADING && rec[i].part == CANLIN_UI_P_LINE) {
            heading = 1;
            CHECK(rec[i].y >= view.y && rec[i].x >= view.x);
        }
        if (rec[i].kind == CANLIN_UI_W_BUTTON)
            button = 1;
    }
    CHECK(heading && button);
    /* the label strip is drawn (surface colour with text), and so is the view content */
    memcpy(bg, px + ((size_t)(view.y + view.h - 2) * SW + (size_t)(view.x + view.w - 2)) * 3, 3);
    for (y = lab.y; y < lab.y + lab.h; y++)
        for (x = lab.x; x < lab.x + lab.w; x++)
            if (memcmp(px + ((size_t)y * SW + (size_t)x) * 3, bg, 3) != 0)
                changed |= 1;
    rec = canlin_ui_view_record(v, &n, NULL);
    for (i = 0; i < n; i++)
        if (rec[i].kind == CANLIN_UI_W_BUTTON && rec[i].part == CANLIN_UI_P_BOX)
            for (y = rec[i].y; y < rec[i].y + rec[i].h && y < SH; y++)
                for (x = rec[i].x; x < rec[i].x + rec[i].w && x < SW; x++)
                    if (memcmp(px + ((size_t)y * SW + (size_t)x) * 3, bg, 3) != 0)
                        changed |= 2;
    CHECK(changed == 3);

    canlin_assistant_view_free(av);
    canlin_shell_free(sh);
    canlin_module_free(shell_mod);
    shell_mod = NULL;
    CHECK(canlin_ev_free(ev) == CANLIN_EV_OK);
    CHECK(canlin_ui_output_free(out) == CANLIN_UI_OK);
    canlin_ui_free(ui);
}

int main(void)
{
    test_valid();
    test_invalid();
    test_limits();
    test_calls();
    test_stale_click();
    test_submit();
    test_graph_round_trip();
    test_shell_frame();
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("assistant view: %d checks passed\n", checks);
    return 0;
}
