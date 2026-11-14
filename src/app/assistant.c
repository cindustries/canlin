/* CanLin assistant session: the built-in assistant module, the model view, the
 * response gate and the routing between them. See include/canlin/app.h.
 *
 * Isolation (module roles spec §3): the loader owns the session. The assistant module
 * gets canlin.llm, the assistant-view binding and the session table; every other
 * module gets neither table (the demo module sees canlin.llm == nil and
 * canlin.view == nil). The gate acts on the tool calls of a response and only on a
 * turn that ended ok, so the module cannot set the model view, cannot read a
 * credential, cannot reach another module's view and cannot reach the network except
 * through the provider request.
 *
 * The tool result goes back to the module through the session table: the loader holds
 * the recorded result in C (the gate) and hands it out read-only. The module never
 * sees a set_view intake: a module-facing set_view would let a module forge a model
 * view (module roles spec §3).
 */
#include "app_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/* The built-in assistant module: the conversation, the provider request and the
 * set_view tool loop (LLM providers spec §4, LLM/UI design §4).
 *
 * The loader owns the model view and the gate: this module asks the provider, applies
 * the tool results the loader hands it and never sets the model view itself. */
static const char ASSISTANT_SRC[] =
    "local M = {}\n"
    "local history = {}\n"      /* the normalised conversation */
    "local answer = ''\n"       /* the text the user sees (the last or the running one) */
    "local buf = ''\n"          /* the text of the running response */
    "local calls = {}\n"        /* the tool calls of the running answer */
    "local note = ''\n"         /* the one short line of the last turn */
    "local mark = ''\n"         /* 'abgebrochen' or 'gekürzt' */
    "local rounds = 0\n"        /* the requests of the running user turn */
    "local question = ''\n"     /* the user's question */
    "local handle = 0\n"        /* the request in flight */
    "local MAX_ROUNDS = canlin.session.rounds_max\n"
    "local MAX_SHOW = 16000\n"  /* the view shows at most CANLIN_UI_MAX_TEXT bytes */
    "\n"
    "local function push(m)\n"
    "  history[#history + 1] = m\n"
    "end\n"
    "\n"
    "-- One JSON text of a string (the model's form values go back to the provider).\n"
    "local function jstr(s)\n"
    "  local out = {}\n"
    "  for i = 1, #s do\n"
    "    local c = s:sub(i, i)\n"
    "    if c == '\"' then out[#out + 1] = '\\\\\"'\n"
    "    elseif c == '\\\\' then out[#out + 1] = '\\\\\\\\'\n"
    "    elseif c == '\\n' then out[#out + 1] = '\\\\n'\n"
    "    elseif c == '\\t' then out[#out + 1] = '\\\\t'\n"
    "    elseif c == '\\r' then out[#out + 1] = '\\\\r'\n"
    "    elseif c < ' ' then out[#out + 1] = '?'\n"
    "    else out[#out + 1] = c end\n"
    "  end\n"
    "  return '\"' .. table.concat(out) .. '\"'\n"
    "end\n"
    "\n"
    "-- One JSON text of a value of the model's form.\n"
    "local function jval(v, depth)\n"
    "  if depth > 4 then return 'null' end\n"
    "  local t = type(v)\n"
    "  if t == 'string' then return jstr(v) end\n"
    "  if t == 'boolean' then return tostring(v) end\n"
    "  if t == 'number' then return string.format('%g', v) end\n"
    "  if t ~= 'table' then return 'null' end\n"
    "  local n = #v\n"
    "  if n > 0 then\n"
    "    local out = {}\n"
    "    for i = 1, n do out[i] = jval(v[i], depth + 1) end\n"
    "    return '[' .. table.concat(out, ',') .. ']'\n"
    "  end\n"
    "  local ks = {}\n"
    "  for k in pairs(v) do ks[#ks + 1] = k end\n"
    "  table.sort(ks, function(a, b) return tostring(a) < tostring(b) end)\n"
    "  local out = {}\n"
    "  for i = 1, #ks do\n"
    "    out[i] = jstr(tostring(ks[i])) .. ':' .. jval(v[ks[i]], depth + 1)\n"
    "  end\n"
    "  return '{' .. table.concat(out, ',') .. '}'\n"
    "end\n"
    "\n"
    "-- The text the view can show: the tail, cut at a UTF-8 boundary.\n"
    "local function show(s)\n"
    "  if #s <= MAX_SHOW then return s end\n"
    "  local cut = #s - MAX_SHOW\n"
    "  local b = s:byte(cut + 1)\n"
    "  while b ~= nil and b >= 0x80 and b < 0xc0 do\n"
    "    cut = cut + 1\n"
    "    b = s:byte(cut + 1)\n"
    "  end\n"
    "  return s:sub(cut + 1)\n"
    "end\n"
    "\n"
    "-- The provider request: the conversation and the core-provided tools.\n"
    "local function ask()\n"
    "  local pid = canlin.session.profile\n"
    "  if pid == nil or pid == '' then\n"
    "    local ps = canlin.llm.profiles()\n"
    "    if ps[1] ~= nil then pid = ps[1].id end\n"
    "  end\n"
    "  if pid == nil or pid == '' then\n"
    "    note = 'Kein Provider-Profil'\n"
    "    print('note ' .. note)\n"
    "    return\n"
    "  end\n"
    "  if rounds >= MAX_ROUNDS then\n"
    "    note = 'Keine weitere Runde'\n"
    "    print('note ' .. note)\n"
    "    return\n"
    "  end\n"
    "  rounds = rounds + 1\n"
    "  buf = ''\n"               /* the text of the new response starts empty */
    "  local h, code, msg = canlin.llm.request(pid, {messages = history, tools = {'set_view'}})\n"
    "  if h == nil then\n"
    "    note = 'Anfrage abgelehnt: ' .. tostring(code)\n"
    "    print('note ' .. note)\n"
    "    return\n"
    "  end\n"
    "  handle = h\n"
    "  print('req ' .. tostring(rounds))\n"
    "end\n"
    "\n"
    "-- A new user turn: the text is the user's message.\n"
    "local function turn(text)\n"
    "  rounds = 0\n"
    "  answer = ''\n"
    "  buf = ''\n"
    "  calls = {}\n"
    "  note = ''\n"
    "  mark = ''\n"
    "  push({role = 'user', text = text})\n"
    "  ask()\n"
    "end\n"
    "\n"
    "-- The provider's answer is done (LLM/UI design §4).\n"
    "local function done(p)\n"
    "  local stop = ''\n"
    "  local dropped = 0\n"
    "  if p.ok then\n"
    "    stop = p.result.stop\n"
    "    if stop == 'length' then dropped = p.result.dropped_tools or 0 end\n"
    "  end\n"
    "  if not p.ok then\n"
    "    -- A failed answer: the text stays and is marked, the dropped tool calls never\n"
    "    -- go back to the provider and the view is unchanged.\n"
    "    mark = 'abgebrochen'\n"
    "    answer = buf\n"
    "    if answer ~= '' then answer = answer .. ' (' .. mark .. ')' end\n"
    "    buf = ''\n"
    "    note = 'Ansicht nicht geändert'\n"
    "    if answer ~= '' then push({role = 'assistant', text = answer}) end\n"
    "    print('turn failed ' .. tostring(p.code) .. ' ' .. tostring(p.message) .. ' ' ..\n"
    "          mark .. ' ' .. note)\n"
    "    calls = {}\n"
    "    return\n"
    "  end\n"
    "  local mtext = buf\n"
    "  if stop == 'length' then\n"
    "    mark = 'gekürzt'\n"
    "    if mtext ~= '' then mtext = mtext .. ' (' .. mark .. ')' end\n"
    "    if dropped > 0 then note = 'Ansicht nicht geändert' end\n"
    "  end\n"
    "  -- The assistant message with the complete tool calls.\n"
    "  local msg = {role = 'assistant', text = mtext}\n"
    "  if #calls > 0 then msg.tool_calls = calls end\n"
    "  push(msg)\n"
    "  -- The tool results: the loader holds them and hands them to the module.\n"
    "  local n = 0\n"
    "  for i = 1, #calls do\n"
    "    local res = canlin.session.result(calls[i].id)\n"
    "    if res ~= nil then\n"
    "      push({role = 'tool', tool_call_id = calls[i].id, text = res})\n"
    "      n = n + 1\n"
    "    end\n"
    "  end\n"
    "  print('turn ok ' .. stop .. ' ' .. mark .. ' calls ' .. tostring(#calls) ..\n"
    "        ' results ' .. tostring(n) .. ' ' .. note)\n"
    "  answer = mtext\n"
    "  buf = ''\n"
    "  calls = {}\n"
    "  if n > 0 then ask() end\n"
    "end\n"
    "\n"
    "function M.init()\n"
    "end\n"
    "\n"
    "function M.event(name, p)\n"
    "  if name == 'llm' then\n"
    "    if p.handle ~= handle then return end\n"
    "    if p.kind == 'text' then\n"
    "      buf = buf .. p.text\n"
    "      answer = buf\n"
    "    elseif p.kind == 'tool' then\n"
    "      calls[#calls + 1] = {id = p.id, name = p.name, args = p.args}\n"
    "    end\n"
    "  elseif name == 'done' then\n"
    "    if p.handle ~= handle then return end\n"
    "    done(p)\n"
    "  end\n"
    "end\n"
    "\n"
    "function M.draw(ui)\n"
    "  -- The model view: the loader owns it, this module never sets it.\n"
    "  local rev = canlin.view.draw()\n"
    "  if rev == nil then return end\n"
    "  -- The answer of the last turn.\n"
    "  if answer ~= '' then ui:text(show(answer)) end\n"
    "  if note ~= '' then ui:text(note, {style = 'muted'}) end\n"
    "  -- The user's question.\n"
    "  question = ui:field('Frage', question, {placeholder = 'Frage stellen'})\n"
    "  if ui:button('Fragen') and question ~= '' then\n"
    "    turn(question)\n"
    "  end\n"
    "  -- The submission of the model's form goes back to the provider.\n"
    "  local sub = canlin.view.submission()\n"
    "  if sub ~= nil then\n"
    "    turn('action=' .. tostring(sub.action) .. ' values=' .. jval(sub.values, 1))\n"
    "  end\n"
    "end\n"
    "\n"
    "return M\n";

/* ---- values of a module event payload ---- */

static const canlin_value *value_get(const canlin_value *t, const char *key)
{
    size_t i, kl = strlen(key);
    if (t == NULL || t->type != CANLIN_VALUE_TABLE)
        return NULL;
    for (i = 0; i < t->n; i++)
        if (t->pairs[i].key.type == CANLIN_VALUE_STR && t->pairs[i].key.len == kl &&
            memcmp(t->pairs[i].key.s, key, kl) == 0)
            return &t->pairs[i].val;
    return NULL;
}

static const char *value_str(const canlin_value *t, const char *key, size_t *len)
{
    const canlin_value *v = value_get(t, key);
    *len = 0;
    if (v == NULL || v->type != CANLIN_VALUE_STR)
        return NULL;
    *len = v->len;
    return v->s ? v->s : "";
}

static int64_t value_int(const canlin_value *t, const char *key, int64_t def)
{
    const canlin_value *v = value_get(t, key);
    if (v == NULL || v->type != CANLIN_VALUE_INT)
        return def;
    return v->i;
}

/* ---- the module-facing session table ---- */

/* canlin.session.result(id): the recorded result of the tool call (the loader holds it
 * in C; the module cannot make one). nil when the call is unknown or was discarded. */
static int l_result(lua_State *L)
{
    canlin_app_session *s = lua_touserdata(L, lua_upvalueindex(1));
    size_t id_len, n, i;
    const char *id = luaL_checklstring(L, 1, &id_len);
    char buf[1024];

    if (s == NULL || s->gate == NULL) {
        lua_pushnil(L);
        return 1;
    }
    n = canlin_assistant_response_ncalls(s->gate);
    for (i = 0; i < n; i++) {
        const canlin_ar_call *c = canlin_assistant_response_call(s->gate, (unsigned)i);
        size_t len;
        if (c == NULL || c->id_len != id_len || memcmp(c->id, id, id_len) != 0)
            continue;
        len = canlin_av_result_json(&c->res, buf, sizeof buf);
        if (len == 0) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, buf, len);
        return 1;
    }
    lua_pushnil(L); /* a discarded call gets no result and must not go back */
    return 1;
}

/* The assistant hook of canlin_module_source: canlin.llm, the assistant-view binding
 * and the session table - for the built-in assistant only. */
static int assistant_install(lua_State *L, canlin_module *m, void *ud)
{
    canlin_app_session *s = ud;
    if (s == NULL)
        return 0;
    canlin_llm_install(L, m, s->llm);
    canlin_assistant_binding_install(L, m, &s->bind);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, s->profile);
    lua_setfield(L, -2, "profile");
    lua_pushinteger(L, (lua_Integer)CANLIN_LLM_TOOL_ROUNDS_MAX);
    lua_setfield(L, -2, "rounds_max");
    lua_pushlightuserdata(L, s);
    lua_pushcclosure(L, l_result, 1);
    lua_setfield(L, -2, "result");
    lua_setfield(L, -2, "session"); /* the `canlin` table is on top of the stack */
    return 0;
}

/* ---- the gate routing ---- */

/* A request opens a response: the scope (generation, turn) belongs to the loader and
 * is set before the turn ends, so a call of an earlier request is stale. */
static void open_response(canlin_app_session *s, canlin_ev_handle h)
{
    canlin_ev_gen gen = canlin_module_gen(s->m);
    if (gen == 0)
        return;
    if (s->have_cur && s->cur == h)
        return;
    s->turn++;
    canlin_assistant_view_set_scope(s->av, gen, s->turn);
    canlin_assistant_response_begin(s->gate, gen, s->turn);
    s->cur = h;
    s->have_cur = 1;
}

/* The module's "llm" event: the text and the tool calls of the running answer. */
static void route_event(canlin_app_session *s, const canlin_ev_delivery *d)
{
    const canlin_module_event *e = d->payload;
    const char *kind;
    size_t kind_len, n;

    if (d->source != CANLIN_MODULE_SOURCE_EVENT || e == NULL || e->name == NULL ||
        strcmp(e->name, "llm") != 0)
        return;
    open_response(s, (canlin_ev_handle)value_int(e->value, "handle", 0));
    kind = value_str(e->value, "kind", &kind_len);
    if (kind == NULL)
        return;
    if (kind_len == 4 && memcmp(kind, "text", 4) == 0) {
        (void)value_str(e->value, "text", &n);
        canlin_assistant_response_text(s->gate, n);
    } else if (kind_len == 4 && memcmp(kind, "tool", 4) == 0) {
        const char *id = value_str(e->value, "id", &n);
        size_t id_len = n;
        const char *name = value_str(e->value, "name", &n);
        size_t name_len = n;
        const char *args = value_str(e->value, "args", &n);
        size_t args_len = n;
        if (id == NULL || name == NULL)
            return;
        canlin_assistant_response_tool(s->gate, id, id_len, name, name_len,
                                       args ? args : "", args_len);
    }
}

/* The payload of an OP completion is opaque bytes: the tag says whether they are a
 * module result (module.h). The module layer checks the tag and the size before a
 * single field of the body is read; the app reads the same payload and does the same:
 * a completion of another shape is refused, its words are never followed as pointers. */
static int op_result_ok(const canlin_ev_delivery *d)
{
    uint32_t tag;
    if (d->payload == NULL || d->bytes < sizeof(canlin_module_result))
        return 0;
    memcpy(&tag, d->payload, sizeof tag);
    return tag == CANLIN_MODULE_RESULT_TAG;
}

/* The module's "done": the turn ends. The gate dispatches the held tool calls only
 * when the turn ended ok. */
static void route_done(canlin_app_session *s, const canlin_ev_delivery *d)
{
    const canlin_module_result *r = NULL;
    const char *stop;
    size_t stop_len;
    int typed = op_result_ok(d);

    open_response(s, d->handle);
    if (d->payload != NULL && !typed) {
        /* A completion of another shape: the turn fails the way the module layer
         * fails it, the words of a foreign payload are not read. */
        canlin_assistant_response_finish(s->gate, s->av, 0, "unavailable",
                                          "result is not a module result");
        return;
    }
    if (typed)
        r = d->payload; /* the payload is a module result: its fields may be read */
    if (d->status == CANLIN_EV_SUCCESS) {
        stop = r ? value_str(r->value, "stop", &stop_len) : NULL;
        if (stop != NULL && stop_len == 6 && memcmp(stop, "length", 6) == 0) {
            int64_t dropped = r ? value_int(r->value, "dropped_tools", 0) : 0;
            if (dropped < 0)
                dropped = 0;
            canlin_assistant_response_finish_length(s->gate, s->av, (unsigned)dropped);
        } else {
            canlin_assistant_response_finish(s->gate, s->av, 1, NULL, NULL);
        }
    } else {
        canlin_assistant_response_finish(s->gate, s->av, 0,
                                          r && r->code ? r->code : "unavailable",
                                          r && r->message ? r->message : "");
    }
}

void canlin_app_assistant_deliver(canlin_app_session *s, const canlin_ev_delivery *d)
{
    if (s == NULL || s->m == NULL || d == NULL)
        return;
    switch (d->kind) {
    case CANLIN_EV_D_EVENT:
        route_event(s, d);
        break;
    case CANLIN_EV_D_OP:
        route_done(s, d);
        break;
    default:
        break;
    }
    /* The module sees the event after the gate: its done handler finds the tool
     * results in the session table. */
    canlin_module_deliver(s->m, d);
}

void canlin_app_assistant_update(canlin_app_session *s)
{
    canlin_ui_view *v;
    if (s == NULL || s->m == NULL)
        return;
    v = canlin_module_view(s->m);
    if (v != NULL && v != s->attached) {
        /* The model view is drawn into the module's own view; the attach is what
         * resets the queued input when the model replaces the view. */
        canlin_assistant_view_attach(s->av, v);
        s->attached = v;
    }
}

/* ---- the session ---- */

canlin_app_session *canlin_app_assistant_new(canlin_ev *ev, const canlin_app_config *cfg,
                                                 canlin_sandbox_log_fn log, void *log_ud,
                                                 int *err)
{
    canlin_app_session *s;
    canlin_llm_config lc;
    canlin_sandbox_config sc;
    int rc = CANLIN_MODULE_OK;

    if (err)
        *err = CANLIN_APP_OK;
    if (ev == NULL || cfg == NULL || cfg->llm_transport.open == NULL)
        return NULL; /* no transport: the app runs without an assistant */
    s = calloc(1, sizeof *s);
    if (s == NULL) {
        if (err)
            *err = CANLIN_APP_ENOMEM;
        return NULL;
    }
    memset(&lc, 0, sizeof lc);
    lc.transport = cfg->llm_transport;
    lc.allow_insecure_dev = cfg->llm_allow_insecure_dev;
    s->llm = canlin_llm_new(ev, &lc);
    s->av = canlin_assistant_view_new();
    s->gate = canlin_assistant_response_new();
    if (s->llm == NULL || s->av == NULL || s->gate == NULL) {
        canlin_llm_free(s->llm);
        canlin_assistant_view_free(s->av);
        canlin_assistant_response_free(s->gate);
        free(s);
        if (err)
            *err = CANLIN_APP_ENOMEM;
        return NULL;
    }
    s->bind.view = s->av;
    s->bind.granted = 1;
    if (cfg->assistant_profile != NULL) {
        size_t n = strlen(cfg->assistant_profile);
        if (n > CANLIN_LLM_ID_MAX)
            n = CANLIN_LLM_ID_MAX;
        memcpy(s->profile, cfg->assistant_profile, n);
        s->profile[n] = 0;
    }
    s->src.name = "assistant";
    s->src.version = "1";
    s->src.main = ASSISTANT_SRC;
    s->src.main_len = sizeof ASSISTANT_SRC - 1;
    s->src.assistant = assistant_install;
    s->src.assistant_ud = s;
    memset(&sc, 0, sizeof sc);
    sc.log = log;
    sc.ud = log_ud;
    s->m = canlin_module_load(ev, &s->src, &sc, &rc);
    if (s->m == NULL) {
        canlin_llm_free(s->llm);
        canlin_assistant_view_free(s->av);
        canlin_assistant_response_free(s->gate);
        free(s);
        if (err)
            *err = CANLIN_APP_ESETUP;
        return NULL;
    }
    return s;
}

void canlin_app_assistant_free(canlin_app_session *s)
{
    if (s == NULL)
        return;
    canlin_module_free(s->m); /* before the LLM context: it outlives every module */
    canlin_llm_free(s->llm);
    canlin_assistant_response_free(s->gate);
    canlin_assistant_view_free(s->av);
    free(s);
}
