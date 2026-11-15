/* The built-in assistant module and the app wiring that drives it (k109): the loader
 * sets the trusted assistant hook for the built-in assistant only, owns the session
 * (the model view, the gate), routes the module's llm and done events into the gate and
 * hands the tool results back to the module.
 *
 * The assistant module runs in the real sandbox and the real scheduler against the
 * fixture transport (no network, no key beyond the fixture's): the user asks, the model
 * answers with a set_view tool call, the gate sets the model view, the provider's tool
 * loop continues with the tool result and the user's submission of the model's form
 * reaches the provider. A failed answer keeps the text, marks it "abgebrochen" and
 * leaves the view unchanged. A module without the capability sees neither canlin.llm
 * nor canlin.view.
 *
 * Exits 0 on success. */
#include "canlin/app.h"
#include "canlin/llm.h"
#include "canlin/module.h"
#include "canlin/shell.h"
#include "canlin/ui.h"

/* The session the loader owns: the gate, the view and the assistant module. The gate is
 * the app's own judgement of a completion, so it is where a refused payload shows. */
#include "../../src/app/app_int.h"

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

#define W 800
/* The assistant's pane holds two views now: the model's own in the frame and the
 * module's own below it (half of the pane each). The module's half must fit the
 * chat content and the on-screen keyboard above it, so the output is taller than a
 * 480 px screen: on a short pane the keyboard panel lies over the content and
 * covers the assistant's own button (the UI lane's card). */
#define H 800
#define LOGMAX 16384

typedef struct harness {
    canlin_ev_fake_clock clock;
    canlin_loop_fake_wait fw;
    canlin_app *a;
    canlin_loop *l;
    canlin_ev *ev; /* the app's scheduler (the loop config hands it out) */
    canlin_llm *llm;
    canlin_llm_fixture *fx;
    char log[LOGMAX];
    size_t loglen;
    int errors;
} harness;

static void log_fn(void *ud, int level, const char *line)
{
    harness *h = ud;
    size_t n = strlen(line);
    if (level == CANLIN_APP_LOG_ERROR)
        h->errors++;
    if (h->loglen + n + 2 < sizeof h->log) {
        memcpy(h->log + h->loglen, line, n);
        h->loglen += n;
        h->log[h->loglen++] = '\n';
        h->log[h->loglen] = 0;
    }
}

static int has(const harness *h, const char *needle)
{
    return strstr(h->log, needle) != NULL;
}

/* The fixture transport is a loop source: one callback per iteration. */
static void fx_step(void *ctx, canlin_ev_time now)
{
    (void)now;
    canlin_llm_fixture_step(ctx);
}

static void h_init(harness *h)
{
    canlin_app_config ac;
    canlin_loop_config lc;
    canlin_loop_source src;
    canlin_llm_profile_config pc;
    int err = 0;

    memset(h, 0, sizeof *h);
    h->clock.now = 1000;
    canlin_loop_fake_wait_init(&h->fw, &h->clock);
    h->fx = canlin_llm_fixture_new();
    CHECK(h->fx != NULL);
    if (h->fx == NULL)
        exit(1);

    memset(&ac, 0, sizeof ac);
    ac.clock = canlin_ev_fake_clock_bind(&h->clock);
    ac.output_id = "test:assistant";
    ac.w = W;
    ac.h = H;
    ac.dpi = 1.0f;
    ac.input = CANLIN_SHELL_IN_TOUCH;
    ac.log = log_fn;
    ac.log_ud = h;
    ac.llm_transport = canlin_llm_fixture_transport(h->fx);
    ac.assistant_profile = "p";
    h->a = canlin_app_new(&ac, &err);
    CHECK(h->a != NULL && err == CANLIN_APP_OK);
    if (h->a == NULL)
        exit(1);

    h->llm = canlin_app_llm(h->a);
    CHECK(h->llm != NULL);
    memset(&pc, 0, sizeof pc);
    pc.id = "p";
    pc.preset = "openai";
    pc.base_url = "https://llm.example/v1";
    pc.model = "m-1";
    pc.tools = 1;
    CHECK(canlin_llm_profile_create(h->llm, &pc) == CANLIN_LLM_OK);
    CHECK(canlin_llm_profile_set_key(h->llm, "p", "sk-dummy", 8) == CANLIN_LLM_OK);

    memset(&lc, 0, sizeof lc);
    canlin_app_loop_config(h->a, &lc);
    h->ev = lc.ev;
    lc.wait = canlin_loop_fake_wait_poll;
    lc.wait_ctx = &h->fw;
    lc.pump = canlin_loop_llm_pump;
    lc.pump_ctx = h->llm;
    h->l = canlin_loop_new(&lc, &err);
    CHECK(h->l != NULL);
    if (h->l == NULL)
        exit(1);
    memset(&src, 0, sizeof src);
    src.step = fx_step;
    src.ctx = h->fx;
    CHECK(canlin_loop_add_source(h->l, &src) == CANLIN_LOOP_OK);
}

static void h_done(harness *h)
{
    canlin_loop_free(h->l);
    canlin_app_free(h->a);
    canlin_llm_fixture_free(h->fx);
}

/* Iterate until the loop would wait without a timeout (idle), at most 128 times. */
static void settle(harness *h)
{
    unsigned k = 0;
    /* The fixture transport delivers its answer in the loop's source step, so the
     * loop is idle while a stream is still open: wait for the request, not for the
     * clock. */
    do
        CHECK(canlin_loop_iterate(h->l) == CANLIN_LOOP_OK);
    while (++k < 256 && (h->fw.last_timeout != -1 || canlin_llm_inflight(h->llm) > 0 ||
                         canlin_llm_fixture_open_streams(h->fx) > 0));
}

/* The n-th (from 0) record entry of kind/part of the assistant module's own view. */
static const canlin_ui_rect *find(harness *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r =
        canlin_ui_view_record(canlin_module_view(canlin_app_assistant(h->a)), &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
            return &r[i];
    return NULL;
}

/* The n-th (from 0) record entry of kind/part of the model's own view. */
static const canlin_ui_rect *find_model(harness *h, int kind, int part, int nth)
{
    size_t n, i;
    const canlin_ui_rect *r = canlin_ui_view_record(canlin_app_model_view(h->a), &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == kind && r[i].part == part && nth-- == 0)
            return &r[i];
    return NULL;
}

/* The on-screen keyboard key of one code point (the field must be focused). */
static const canlin_ui_rect *key(harness *h, uint32_t cp)
{
    size_t n, i;
    const canlin_ui_rect *r =
        canlin_ui_view_record(canlin_module_view(canlin_app_assistant(h->a)), &n, NULL);
    for (i = 0; i < n; i++)
        if (r[i].kind == CANLIN_UI_W_KEYBOARD && r[i].part == CANLIN_UI_P_KEY &&
            r[i].len == CANLIN_UI_KBD_CHAR && r[i].off == cp)
            return &r[i];
    return NULL;
}

static void press(harness *h, const canlin_ui_rect *r)
{
    canlin_ui_input in;
    int x, y;
    if (r == NULL) {
        CHECK(0);
        return;
    }
    x = r->x + r->w / 2;
    y = r->y + r->h / 2;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    in.x = x;
    in.y = y;
    canlin_app_input(h->a, &in);
    settle(h);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = x;
    in.y = y;
    in.down = 1;
    canlin_app_input(h->a, &in);
    settle(h);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = x;
    in.y = y;
    in.down = 0;
    canlin_app_input(h->a, &in);
    settle(h);
}

/* The press without the release: the press edge is applied in the next frame and the
 * button asks. No edge follows, so nothing draws after the answer arrives - the test
 * sees the redraw the app asks for and not one the input causes. */
static void press_down(harness *h, const canlin_ui_rect *r)
{
    canlin_ui_input in;
    int x, y, i;
    if (r == NULL) {
        CHECK(0);
        return;
    }
    x = r->x + r->w / 2;
    y = r->y + r->h / 2;
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    in.x = x;
    in.y = y;
    canlin_app_input(h->a, &in);
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = x;
    in.y = y;
    in.down = 1;
    canlin_app_input(h->a, &in);
    /* the press edge is applied in the next frame: the button asks the provider */
    for (i = 0; i < 16 && canlin_llm_fixture_records(h->fx) == 0; i++)
        CHECK(canlin_loop_iterate(h->l) == CANLIN_LOOP_OK);
    CHECK(canlin_llm_fixture_records(h->fx) > 0);
}

static void push_body(harness *h, const char *body)
{
    canlin_llm_fixture_response r;
    memset(&r, 0, sizeof r);
    r.status = 200;
    r.body = body;
    r.body_len = strlen(body);
    r.chunk = 64;
    CHECK(canlin_llm_fixture_push(h->fx, &r) == CANLIN_LLM_OK);
}

static const char *record_body(harness *h, size_t i)
{
    const canlin_llm_fixture_record *r = canlin_llm_fixture_record_at(h->fx, i);
    return r ? r->body : "";
}

/* ---- the fixture answers ---- */

/* The model answers with a text and one complete set_view call (the form). */
static const char BODY_FORM[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_A\","
    "\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"arguments\":"
    "\"{\\\"title\\\":\\\"Form\\\",\\\"widgets\\\":[{\\\"type\\\":\\\"heading\\\",\\\"text\\\":"
    "\\\"Einkaufsliste\\\"},{\\\"type\\\":\\\"field\\\",\\\"id\\\":\\\"item\\\",\\\"label\\\":"
    "\\\"Artikel\\\",\\\"value\\\":\\\"Milch\\\"},{\\\"type\\\":\\\"button\\\",\\\"id\\\":\\\"ok\\\","
    "\\\"label\\\":\\\"OK\\\",\\\"action\\\":\\\"submit\\\"}]}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";

/* A plain text answer: the turn ends. */
static const char BODY_TEXT[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Alles klar.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

/* A text, a tool call and then a provider error: the call is discarded. */
static const char BODY_FAIL[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_A\","
    "\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"arguments\":"
    "\"{\\\"widgets\\\":[{\\\"type\\\":\\\"heading\\\",\\\"text\\\":\\\"Hi\\\"}]}\"}}]}}]}\n\n"
    "data: {\"error\":{\"message\":\"overloaded\",\"type\":\"server_error\"}}\n\n";

/* A cut-off answer: the tool call is unfinished, the provider stops on the length. */
static const char BODY_LENGTH[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
    "\"call_B\",\"type\":\"function\",\"function\":{\"name\":\"set_view\",\"arguments\":"
    "\"{\\\"widgets\\\":[{\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
    "data: [DONE]\n\n";

/* ---- the conversation ---- */

static void test_conversation(void)
{
    harness h;
    const canlin_ui_rect *f, *b;
    h_init(&h);
    push_body(&h, BODY_FORM);
    push_body(&h, BODY_TEXT);
    push_body(&h, BODY_TEXT);

    /* the assistant is opened by the shell */
    CHECK(canlin_app_open_assistant(h.a) == CANLIN_APP_OK);
    settle(&h);

    /* the module without the capability sees neither table */
    CHECK(has(&h, "caps nil nil"));
    /* the assistant module runs */
    CHECK(canlin_module_get_state(canlin_app_assistant(h.a)) == CANLIN_MODULE_RUNNING);

    /* the question field of the assistant */
    f = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
    CHECK(f != NULL);
    if (f == NULL) {
        h_done(&h);
        return;
    }
    /* the user types the question and asks */
    press(&h, f);
    CHECK(key(&h, 'h') != NULL);
    if (key(&h, 'h') == NULL) {
        h_done(&h);
        return;
    }
    press(&h, key(&h, 'h'));
    press(&h, key(&h, 'i'));
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    press(&h, b);
    settle(&h);

    /* the request went out with the question */
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    CHECK(strstr(record_body(&h, 0), "hi") != NULL);
    CHECK(strstr(record_body(&h, 0), "set_view") != NULL);

    /* the model view is rendered: the model's field and the assistant's own field */
    CHECK(find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(find_model(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) != NULL);
    CHECK(find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) != NULL);

    /* the provider's tool loop continues with the tool result */
    CHECK(strstr(record_body(&h, 1), "revision") != NULL);
    CHECK(strstr(record_body(&h, 1), "call_A") != NULL);

    /* the assistant module got the done event with the value of the result: the stop
     * reason of the answer, once for the turn with the tool call and once for the
     * plain answer that ended it */
    CHECK(has(&h, "turn ok tool"));
    CHECK(has(&h, "turn ok end"));

    /* the user's submission of the model's form reaches the provider */
    b = find_model(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    press(&h, b);
    settle(&h);
    CHECK(canlin_llm_fixture_records(h.fx) == 3);
    CHECK(strstr(record_body(&h, 2), "Milch") != NULL);
    /* the two answers are two messages, not one text */
    CHECK(strstr(record_body(&h, 2), "\"Alles klar.\"") != NULL);
    CHECK(strstr(record_body(&h, 2), "Moment.Alles klar.") == NULL);
    CHECK(strstr(record_body(&h, 2), "ok") != NULL);

    CHECK(h.errors == 0);
    h_done(&h);
}

/* A failed answer: the text stays and is marked, the view is unchanged, the dropped
 * tool call never goes back to the provider. */
static void test_failed_answer(void)
{
    harness h;
    h_init(&h);
    push_body(&h, BODY_FAIL);

    CHECK(canlin_app_open_assistant(h.a) == CANLIN_APP_OK);
    settle(&h);
    press(&h, find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0));
    press(&h, key(&h, 'h'));
    press(&h, find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0));
    settle(&h);

    /* the request went out */
    CHECK(canlin_llm_fixture_records(h.fx) == 1);
    /* the text is kept and marked, the view is unchanged, the note says so */
    CHECK(has(&h, "abgebrochen"));
    CHECK(has(&h, "Ansicht nicht ge"));
    /* the model view was never set: the assistant's view holds only its own field */
    CHECK(find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 1) == NULL);
    /* the discarded call never went back to the provider */
    CHECK(canlin_llm_fixture_records(h.fx) == 1);
    CHECK(!has(&h, "turn ok"));
    /* the done event carried the failure the producer reported: its code and its
     * message reached the assistant module */
    CHECK(has(&h, "turn failed unavailable provider error: overloaded"));

    CHECK(h.errors == 0);
    if (h.errors)
        fprintf(stderr, "log:\n%s", h.log);
    h_done(&h);
}

/* A cut-off answer: the text is kept and marked "gekürzt", the dropped tool call never
 * goes back to the provider and the view stays as it was. */
static void test_cut_off_answer(void)
{
    harness h;
    h_init(&h);
    push_body(&h, BODY_LENGTH);

    CHECK(canlin_app_open_assistant(h.a) == CANLIN_APP_OK);
    settle(&h);
    press(&h, find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0));
    press(&h, key(&h, 'h'));
    press(&h, find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0));
    settle(&h);

    /* the request went out, the dropped call never came back */
    CHECK(canlin_llm_fixture_records(h.fx) == 1);
    /* the text is kept and marked, the view is unchanged, the note says so */
    CHECK(has(&h, "gekürzt"));
    CHECK(has(&h, "Ansicht nicht ge"));
    CHECK(find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 1) == NULL);

    CHECK(h.errors == 0);
    if (h.errors)
        fprintf(stderr, "log:\n%s", h.log);
    h_done(&h);
}

/* ---- the model's own view ---- */

/* The app creates the model's own view and hands it to the shell: the pane holds two
 * views then, the model's own in the frame and the module's own below it. The view
 * belongs to the app: the shell holds it and never frees it. */
static void test_model_view(void)
{
    harness h;
    canlin_shell *sh;
    canlin_ui_view *mv;
    canlin_shell_rect mr, vr;
    canlin_ui_input in;
    int pane = -1, i, np;

    h_init(&h);
    CHECK(canlin_app_open_assistant(h.a) == CANLIN_APP_OK);
    settle(&h);

    /* the app owns a view for the model, and it is not the module's own view */
    mv = canlin_app_model_view(h.a);
    CHECK(mv != NULL);
    CHECK(mv != canlin_module_view(canlin_app_assistant(h.a)));

    /* the shell holds it: the pane shows the model's own view in the frame */
    sh = canlin_app_shell(h.a);
    CHECK(sh != NULL);
    np = canlin_shell_panes(sh, canlin_app_output(h.a));
    for (i = 0; i < np; i++)
        if (canlin_shell_pane_module(sh, canlin_app_output(h.a), i) == canlin_app_assistant(h.a))
            pane = i;
    CHECK(pane >= 0);
    if (pane < 0) {
        h_done(&h);
        return;
    }
    CHECK(canlin_shell_part_rect(sh, canlin_app_output(h.a), CANLIN_SHELL_P_MODEL_VIEW, pane,
                                 &mr) == 1);
    CHECK(canlin_shell_part_rect(sh, canlin_app_output(h.a), CANLIN_SHELL_P_VIEW, pane, &vr) == 1);
    CHECK(mr.w > 0 && mr.h > 0);
    CHECK(mr.y < vr.y); /* the model's own view is above the module's own */

    /* a click in the model's block reaches the model's own view */
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_BUTTON;
    in.x = mr.x + 2;
    in.y = mr.y + 2;
    in.down = 1;
    CHECK(canlin_app_input(h.a, &in) == CANLIN_SHELL_R_VIEW);
    CHECK(canlin_ui_view_pending(mv) > 0);

    h_done(&h);
}

/* The answer replaces the model: the app asks for a redraw, so the new model is on
 * screen without waiting for the next input. And a set_view drops the queued input of
 * the model's own view: the assistant view is attached to it. */
static void test_model_redraw(void)
{
    harness h;
    canlin_ui_view *mv, *modv;
    canlin_ui_input in;
    const canlin_ui_rect *f, *b;
    uint32_t frames;
    int i;

    h_init(&h);
    push_body(&h, BODY_FORM);
    push_body(&h, BODY_TEXT);

    CHECK(canlin_app_open_assistant(h.a) == CANLIN_APP_OK);
    settle(&h);
    mv = canlin_app_model_view(h.a);
    modv = canlin_module_view(canlin_app_assistant(h.a));
    CHECK(mv != NULL && modv != NULL);
    if (mv == NULL || modv == NULL) {
        h_done(&h);
        return;
    }

    /* the user asks */
    f = find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0);
    CHECK(f != NULL);
    if (f == NULL) {
        h_done(&h);
        return;
    }
    press(&h, f);
    press(&h, key(&h, 'h'));
    press(&h, key(&h, 'i'));
    b = find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0);
    CHECK(b != NULL);
    press_down(&h, b);

    /* a click waits in the model's own view and in the module's own view, where the
     * model's widgets are until the binding draws them into the model's own view */
    memset(&in, 0, sizeof in);
    in.type = CANLIN_UI_IN_MOTION;
    in.x = f->x + 1;
    in.y = f->y + 1;
    CHECK(canlin_ui_view_input(mv, &in) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_input(modv, &in) == CANLIN_UI_OK);
    CHECK(canlin_ui_view_pending(mv) == 1);
    CHECK(canlin_ui_view_pending(modv) == 1);

    /* the answer arrives and replaces the model */
    for (i = 0; i < 64 && !has(&h, "turn ok tool"); i++)
        CHECK(canlin_loop_iterate(h.l) == CANLIN_LOOP_OK);
    CHECK(has(&h, "turn ok tool"));

    /* the set_view dropped the queued input of the model's own view: the assistant
     * view is attached to it */
    CHECK(canlin_ui_view_pending(mv) == 0);
    /* k130: the model is drawn into the model's own view, so the queued click in the
     * module's own view cannot act on a model widget any more: the module's own draw
     * takes it, and the model's own view holds no queued input (the set_view dropped
     * it). */
    CHECK(canlin_ui_view_pending(modv) == 0);

    /* the app asked for a redraw: the new model is on screen without a new input */
    frames = canlin_ui_view_frames(modv);
    for (i = 0; i < 8 && canlin_ui_view_frames(modv) == frames; i++)
        CHECK(canlin_loop_iterate(h.l) == CANLIN_LOOP_OK);
    CHECK(canlin_ui_view_frames(modv) > frames);
    /* the model is drawn: its button and the assistant's own */
    CHECK(find_model(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0) != NULL);

    h_done(&h);
}

/* ---- the session the loader owns ---- */

/* The test drives the session the way src/app/app.c wires it: the scheduler delivers
 * to the session, the session routes the delivery into the gate and into the module.
 * The gate is the app's own judgement of a completion, so it is where a refused
 * payload shows. */
typedef struct session {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_llm_fixture *fx;
    canlin_app_session *s;
    char log[LOGMAX];
    size_t loglen;
} session;

static void session_log(void *ud, const char *msg, size_t len)
{
    session *h = ud;
    if (h->loglen + len + 2 < sizeof h->log) {
        memcpy(h->log + h->loglen, msg, len);
        h->loglen += len;
        h->log[h->loglen++] = '\n';
        h->log[h->loglen] = 0;
    }
}

static int session_has(const session *h, const char *needle)
{
    return strstr(h->log, needle) != NULL;
}

static void session_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    session *h = ud;
    (void)ev;
    if (h->s)
        canlin_app_assistant_deliver(h->s, d);
}

static void session_init(session *h)
{
    canlin_ev_callbacks cb;
    canlin_ev_clock clk;
    canlin_app_config ac;
    canlin_ev_turn_result tr;
    int err = 0;

    memset(h, 0, sizeof *h);
    h->clock.now = 1000;
    clk = canlin_ev_fake_clock_bind(&h->clock);
    cb.deliver = session_deliver;
    h->ev = canlin_ev_new(&clk, &cb, h);
    CHECK(h->ev != NULL);
    h->fx = canlin_llm_fixture_new();
    CHECK(h->fx != NULL);
    if (h->ev == NULL || h->fx == NULL)
        exit(1);
    memset(&ac, 0, sizeof ac);
    ac.llm_transport = canlin_llm_fixture_transport(h->fx);
    ac.assistant_profile = "p";
    h->s = canlin_app_assistant_new(h->ev, &ac, session_log, h, &err);
    CHECK(h->s != NULL && err == CANLIN_APP_OK);
    if (h->s == NULL)
        exit(1);
    canlin_ev_turn(h->ev, &tr); /* the module runs: its init */
}

static void session_done(session *h)
{
    canlin_app_assistant_free(h->s);
    canlin_ev_free(h->ev);
    canlin_llm_fixture_free(h->fx);
}

/* Another producer's payload: the layout of a module result, another tag and its own
 * words. A reader that takes it for a result gets the foreign words instead of the
 * refusal. */
static void foreign_payload(canlin_module_result *r, canlin_value *value,
                            canlin_value_pair *pair)
{
    pair->key.type = CANLIN_VALUE_STR;
    pair->key.s = "stop";
    pair->key.len = 4;
    pair->val.type = CANLIN_VALUE_STR;
    pair->val.s = "stop";
    pair->val.len = 4;
    value->type = CANLIN_VALUE_TABLE;
    value->pairs = pair;
    value->n = 1;
    r->tag = 0x444f4e45u; /* "DONE": not CANLIN_MODULE_RESULT_TAG */
    r->value = value;
    r->code = "foreign-code";
    r->message = "a foreign payload is not a result";
}

/* A completion of another shape: the assistant refuses it. The words of the payload
 * are never read: the gate holds the module layer's refusal and the assistant
 * survives it. */
static void test_foreign_payload(void)
{
    session h;
    canlin_ev_gen gen;
    canlin_ev_handle oh;
    canlin_ev_turn_result tr;
    canlin_module_result foreign;
    canlin_value value;
    canlin_value_pair pair;
    const canlin_ar_outcome *o;

    session_init(&h);
    foreign_payload(&foreign, &value, &pair);

    /* the completion reaches the session the way a producer's does */
    gen = canlin_module_gen(h.s->m);
    CHECK(gen != 0);
    CHECK(canlin_ev_op_begin(h.ev, gen, sizeof foreign, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_SUCCESS, &foreign, sizeof foreign,
                                 NULL) == CANLIN_EV_OK);
    canlin_ev_turn(h.ev, &tr);

    /* the gate refused it: the turn failed with the module layer's refusal */
    o = canlin_assistant_response_outcome(h.s->gate);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->state == CANLIN_AR_FAILED);
        CHECK(strcmp(o->code, "unavailable") == 0);
        CHECK(strcmp(o->message, "result is not a module result") == 0);
        /* the foreign words were not taken for the result */
        CHECK(strstr(o->code, "foreign") == NULL);
        CHECK(strstr(o->message, "foreign") == NULL);
        CHECK(o->dispatched == 0);
    }
    /* the module layer refused the same completion */
    CHECK(session_has(&h, "refused: result is not a module result"));
    /* the assistant survives: the module runs and has no error */
    CHECK(canlin_module_get_state(h.s->m) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_errors(h.s->m) == 0);

    session_done(&h);
}

/* A completion of another shape reaches the app's scheduler: the assistant refuses it,
 * survives it, and the next turn still works. */
static void test_foreign_payload_survives(void)
{
    harness h;
    canlin_ev_gen gen;
    canlin_ev_handle oh;
    canlin_module_result foreign;
    canlin_value value;
    canlin_value_pair pair;

    h_init(&h);
    foreign_payload(&foreign, &value, &pair);

    /* the assistant is opened by the shell */
    CHECK(canlin_app_open_assistant(h.a) == CANLIN_APP_OK);
    settle(&h);

    /* the completion reaches the app the way a producer's does */
    gen = canlin_module_gen(canlin_app_assistant(h.a));
    CHECK(gen != 0);
    CHECK(canlin_ev_op_begin(h.ev, gen, sizeof foreign, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_SUCCESS, &foreign, sizeof foreign,
                                 NULL) == CANLIN_EV_OK);
    settle(&h);

    /* the assistant survives the refusal */
    CHECK(canlin_module_get_state(canlin_app_assistant(h.a)) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_errors(canlin_app_assistant(h.a)) == 0);
    CHECK(has(&h, "refused: result is not a module result"));

    /* the next turn still works: the provider is asked and the model view is set */
    push_body(&h, BODY_FORM);
    push_body(&h, BODY_TEXT);
    press(&h, find(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0));
    press(&h, key(&h, 'h'));
    press(&h, key(&h, 'i'));
    press(&h, find(&h, CANLIN_UI_W_BUTTON, CANLIN_UI_P_BOX, 0));
    settle(&h);
    /* the question went out and the tool loop continued with the tool result */
    CHECK(canlin_llm_fixture_records(h.fx) == 2);
    CHECK(strstr(record_body(&h, 0), "hi") != NULL);
    CHECK(strstr(record_body(&h, 1), "call_A") != NULL);
    /* the model's form is in the model's own view: the tool call was acted on */
    CHECK(find_model(&h, CANLIN_UI_W_FIELD, CANLIN_UI_P_BOX, 0) != NULL);

    CHECK(h.errors == 0);
    if (h.errors)
        fprintf(stderr, "log:\n%s", h.log);
    h_done(&h);
}

/* The payload of another producer's bytes: the words of it are not pointers and must
 * not be followed as such. The assistant refuses the completion and lives. */
static void test_foreign_bytes(void)
{
    session h;
    canlin_ev_gen gen;
    canlin_ev_handle oh;
    canlin_ev_turn_result tr;
    unsigned char foreign[64];
    const canlin_ar_outcome *o;

    session_init(&h);
    memset(foreign, 0xff, sizeof foreign);

    /* the completion reaches the session the way a producer's does */
    gen = canlin_module_gen(h.s->m);
    CHECK(gen != 0);
    CHECK(canlin_ev_op_begin(h.ev, gen, sizeof foreign, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_SUCCESS, foreign, sizeof foreign,
                                 NULL) == CANLIN_EV_OK);
    canlin_ev_turn(h.ev, &tr);

    /* the gate refused it and the assistant lives */
    o = canlin_assistant_response_outcome(h.s->gate);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->state == CANLIN_AR_FAILED);
        CHECK(strcmp(o->message, "result is not a module result") == 0);
    }
    CHECK(canlin_module_get_state(h.s->m) == CANLIN_MODULE_RUNNING);
    CHECK(canlin_module_errors(h.s->m) == 0);

    session_done(&h);
}

/* Without a transport the app has no assistant: the module without the capability is
 * the only one. */
static void test_no_transport(void)
{
    canlin_app_config ac;
    canlin_app *a;
    int err = 0;
    memset(&ac, 0, sizeof ac);
    ac.clock = canlin_loop_clock_monotonic();
    ac.output_id = "test:none";
    ac.w = 100;
    ac.h = 100;
    ac.dpi = 1.0f;
    a = canlin_app_new(&ac, &err);
    CHECK(a != NULL && err == CANLIN_APP_OK);
    CHECK(canlin_app_llm(a) == NULL);
    CHECK(canlin_app_assistant(a) == NULL);
    CHECK(canlin_app_open_assistant(a) == CANLIN_APP_EINVAL);
    canlin_app_free(a);
}

/* ---- the module layer, the other half of the comparison ---- */

/* A witness module in a scheduler of its own: it prints what the module layer tells a
 * module about a completion. The assistant's gate and the module's own "done" event
 * are two readings of one delivery, so the witness shows the second reading: both must
 * judge the same completion the same way. */
typedef struct witness {
    canlin_ev_fake_clock clock;
    canlin_ev *ev;
    canlin_module *m;
    char log[LOGMAX];
    size_t loglen;
} witness;

static void witness_log(void *ud, const char *msg, size_t len)
{
    witness *w = ud;
    if (w->loglen + len + 2 < sizeof w->log) {
        memcpy(w->log + w->loglen, msg, len);
        w->loglen += len;
        w->log[w->loglen++] = '\n';
        w->log[w->loglen] = 0;
    }
}

static int witness_has(const witness *w, const char *needle)
{
    return strstr(w->log, needle) != NULL;
}

static void witness_deliver(void *ud, canlin_ev *ev, const canlin_ev_delivery *d)
{
    witness *w = ud;
    (void)ev;
    if (w->m)
        canlin_module_deliver(w->m, d);
}

static const char witness_src[] =
    "local M = {}\n"
    "function M.event(name, p)\n"
    "  if name == 'done' then\n"
    "    if p.ok then print('done ok', p.handle)\n"
    "    else print('done fail', p.handle, p.code, p.message) end\n"
    "  end\n"
    "end\n"
    "return M\n";

static void witness_init(witness *w)
{
    canlin_ev_clock clk;
    canlin_ev_callbacks cb;
    canlin_module_source src;
    canlin_sandbox_config sc;
    int err = 0;

    memset(w, 0, sizeof *w);
    w->clock.now = 1000;
    clk = canlin_ev_fake_clock_bind(&w->clock);
    cb.deliver = witness_deliver;
    w->ev = canlin_ev_new(&clk, &cb, w);
    CHECK(w->ev != NULL);
    if (w->ev == NULL)
        exit(1);
    memset(&src, 0, sizeof src);
    src.name = "witness";
    src.version = "1";
    src.main = witness_src;
    src.main_len = sizeof witness_src - 1;
    memset(&sc, 0, sizeof sc);
    sc.log = witness_log;
    sc.ud = w;
    w->m = canlin_module_load(w->ev, &src, &sc, &err);
    CHECK(w->m != NULL && err == CANLIN_MODULE_OK);
    if (w->m == NULL)
        exit(1);
}

static void witness_done(witness *w)
{
    canlin_module_free(w->m);
    canlin_ev_free(w->ev);
}

/* An oversize completion: the result exceeded its reservation, so the scheduler
 * refuses the payload and marks the delivery OVERSIZE. The contract is the module
 * layer's: a bound refused the result, so the turn fails "full" (core API spec §5.2)
 * and not "unavailable" - a full is a bound that refuses admission now, a retry later
 * may succeed, while "unavailable" says the capability itself is not usable. The
 * assistant must fail it the same way. */
static void test_oversize_completion(void)
{
    session h;
    witness w;
    canlin_ev_gen gen;
    canlin_ev_handle oh, wh;
    canlin_ev_turn_result tr;
    canlin_module_result r;
    const canlin_ar_outcome *o;
    char line[160];

    session_init(&h);
    witness_init(&w);

    /* the completion both sides judge: a result bigger than its reservation */
    memset(&r, 0, sizeof r);
    r.tag = CANLIN_MODULE_RESULT_TAG;
    r.code = "denied";
    r.message = "not allowed";

    /* the assistant's completion */
    gen = canlin_module_gen(h.s->m);
    CHECK(gen != 0);
    CHECK(canlin_ev_op_begin(h.ev, gen, 8, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_SUCCESS, &r, sizeof r, NULL) ==
          CANLIN_EV_OVERSIZE);
    canlin_ev_turn(h.ev, &tr);

    /* the assistant says "full": the result exceeded its reserved size */
    o = canlin_assistant_response_outcome(h.s->gate);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->state == CANLIN_AR_FAILED);
        CHECK(strcmp(o->code, "full") == 0);
        CHECK(strcmp(o->message, "result exceeded its reserved size") == 0);
    }

    /* the module layer says the same about the same completion */
    CHECK(canlin_ev_op_begin(w.ev, canlin_module_gen(w.m), 8, NULL, &wh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(w.ev, wh, CANLIN_EV_SUCCESS, &r, sizeof r, NULL) ==
          CANLIN_EV_OVERSIZE);
    canlin_ev_turn(w.ev, &tr);
    snprintf(line, sizeof line, "done fail\t%lld\tfull\tresult exceeded its reserved size",
             (long long)wh);
    CHECK(witness_has(&w, line));

    witness_done(&w);
    session_done(&h);
}

/* A result whose value exceeds the marshalling limits: the module layer fails it
 * "full" (core API spec §5.2 - a result exceeding the limits is converted into
 * ok=false, code="full", never truncated silently), and the assistant must fail it
 * the same way. */
static canlin_value_pair many_pairs[CANLIN_MARSHAL_MAX_ENTRIES + 1];

static void test_oversize_value(void)
{
    session h;
    witness w;
    canlin_ev_gen gen;
    canlin_ev_handle oh, wh;
    canlin_ev_turn_result tr;
    canlin_module_result r;
    canlin_value many;
    const canlin_ar_outcome *o;
    char line[160];
    unsigned k;

    for (k = 0; k <= CANLIN_MARSHAL_MAX_ENTRIES; k++) {
        many_pairs[k].key.type = CANLIN_VALUE_INT;
        many_pairs[k].key.i = (int64_t)k + 1;
        many_pairs[k].val.type = CANLIN_VALUE_BOOL;
        many_pairs[k].val.i = 1;
    }
    memset(&many, 0, sizeof many);
    many.type = CANLIN_VALUE_TABLE;
    many.pairs = many_pairs;
    many.n = CANLIN_MARSHAL_MAX_ENTRIES + 1;
    CHECK(canlin_value_check(&many) == CANLIN_VALUE_ETOOBIG);

    session_init(&h);
    witness_init(&w);

    /* the completion both sides judge: a success whose value is too big */
    memset(&r, 0, sizeof r);
    r.tag = CANLIN_MODULE_RESULT_TAG;
    r.value = &many;

    /* the assistant's completion */
    gen = canlin_module_gen(h.s->m);
    CHECK(gen != 0);
    CHECK(canlin_ev_op_begin(h.ev, gen, sizeof r, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_SUCCESS, &r, sizeof r, NULL) ==
          CANLIN_EV_OK);
    canlin_ev_turn(h.ev, &tr);

    /* the assistant says "full": the result exceeds the marshalling limits */
    o = canlin_assistant_response_outcome(h.s->gate);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->state == CANLIN_AR_FAILED);
        CHECK(strcmp(o->code, "full") == 0);
        CHECK(strcmp(o->message, "result exceeds the marshalling limits") == 0);
    }

    /* the module layer says the same about the same completion */
    CHECK(canlin_ev_op_begin(w.ev, canlin_module_gen(w.m), sizeof r, NULL, &wh) ==
          CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(w.ev, wh, CANLIN_EV_SUCCESS, &r, sizeof r, NULL) ==
          CANLIN_EV_OK);
    canlin_ev_turn(w.ev, &tr);
    snprintf(line, sizeof line, "done fail\t%lld\tfull\tresult exceeds the marshalling limits",
             (long long)wh);
    CHECK(witness_has(&w, line));

    witness_done(&w);
    session_done(&h);
}

/* A failure code outside the set of core API spec §3.1: the module layer takes the
 * code only when it is one of the six and falls back to "unavailable" otherwise. A
 * producer could put a code in front of the user that the module would never pass on,
 * so the assistant must refuse it the same way. */
static void test_unknown_failure_code(void)
{
    session h;
    witness w;
    canlin_ev_gen gen;
    canlin_ev_handle oh, wh;
    canlin_ev_turn_result tr;
    canlin_module_result r;
    const canlin_ar_outcome *o;
    char line[160];

    session_init(&h);
    witness_init(&w);

    /* the completion both sides judge: a failure with a code of the producer's own */
    memset(&r, 0, sizeof r);
    r.tag = CANLIN_MODULE_RESULT_TAG;
    r.code = "boom";
    r.message = "the producer made a code of its own";

    /* the assistant's completion */
    gen = canlin_module_gen(h.s->m);
    CHECK(gen != 0);
    CHECK(canlin_ev_op_begin(h.ev, gen, sizeof r, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_FAILURE, &r, sizeof r, NULL) ==
          CANLIN_EV_OK);
    canlin_ev_turn(h.ev, &tr);

    /* the assistant refuses the code: it becomes "unavailable" */
    o = canlin_assistant_response_outcome(h.s->gate);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->state == CANLIN_AR_FAILED);
        CHECK(strcmp(o->code, "unavailable") == 0);
        CHECK(strcmp(o->message, "operation failed") == 0);
        /* the code of the producer never reaches the user */
        CHECK(strstr(o->code, "boom") == NULL);
        CHECK(strstr(o->message, "boom") == NULL);
    }

    /* the module layer refuses it the same way */
    CHECK(canlin_ev_op_begin(w.ev, canlin_module_gen(w.m), sizeof r, NULL, &wh) ==
          CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(w.ev, wh, CANLIN_EV_FAILURE, &r, sizeof r, NULL) ==
          CANLIN_EV_OK);
    canlin_ev_turn(w.ev, &tr);
    snprintf(line, sizeof line, "done fail\t%lld\tunavailable\toperation failed",
             (long long)wh);
    CHECK(witness_has(&w, line));
    CHECK(!witness_has(&w, "boom"));

    witness_done(&w);
    session_done(&h);
}

/* A code of the set of core API spec §3.1 passes on unchanged, with the message of
 * the producer: both sides say "denied". */
static void test_known_failure_code(void)
{
    session h;
    witness w;
    canlin_ev_gen gen;
    canlin_ev_handle oh, wh;
    canlin_ev_turn_result tr;
    canlin_module_result r;
    const canlin_ar_outcome *o;
    char line[160];

    session_init(&h);
    witness_init(&w);

    memset(&r, 0, sizeof r);
    r.tag = CANLIN_MODULE_RESULT_TAG;
    r.code = "denied";
    r.message = "not allowed";

    /* the assistant's completion */
    gen = canlin_module_gen(h.s->m);
    CHECK(canlin_ev_op_begin(h.ev, gen, sizeof r, NULL, &oh) == CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(h.ev, oh, CANLIN_EV_FAILURE, &r, sizeof r, NULL) ==
          CANLIN_EV_OK);
    canlin_ev_turn(h.ev, &tr);

    o = canlin_assistant_response_outcome(h.s->gate);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->state == CANLIN_AR_FAILED);
        CHECK(strcmp(o->code, "denied") == 0);
        CHECK(strcmp(o->message, "not allowed") == 0);
    }

    /* the module layer says the same */
    CHECK(canlin_ev_op_begin(w.ev, canlin_module_gen(w.m), sizeof r, NULL, &wh) ==
          CANLIN_EV_OK);
    CHECK(canlin_ev_op_complete(w.ev, wh, CANLIN_EV_FAILURE, &r, sizeof r, NULL) ==
          CANLIN_EV_OK);
    canlin_ev_turn(w.ev, &tr);
    snprintf(line, sizeof line, "done fail\t%lld\tdenied\tnot allowed", (long long)wh);
    CHECK(witness_has(&w, line));

    witness_done(&w);
    session_done(&h);
}

int main(void)
{
    test_conversation();
    test_failed_answer();
    test_cut_off_answer();
    test_model_view();
    test_model_redraw();
    test_foreign_payload();
    test_foreign_payload_survives();
    test_no_transport();
    test_foreign_bytes();
    test_oversize_completion();
    test_oversize_value();
    test_unknown_failure_code();
    test_known_failure_code();
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("app assistant: %d checks passed\n", checks);
    return 0;
}
