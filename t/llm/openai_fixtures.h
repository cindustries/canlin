/* openai-chat fixtures shared by t/llm/test_llm.c (fixture transport) and
 * t/llm/test_llm_http.c (the same bytes through the native HTTP transport, canlin#41):
 * the assistant test module, request setups and recorded-style SSE streams. */
#ifndef CANLIN_T_OPENAI_FIXTURES_H
#define CANLIN_T_OPENAI_FIXTURES_H

/* Assistant module: `setup` runs in init (it sets REQ and may call request), events are
 * printed. Every payload field is printed, so check_no_key covers them. */
static const char mod_tpl[] =
    "local M = {}\n"
    "H = nil\n"
    "local T = ''\n"
    "local function show(v, pre)\n"
    "  if type(v) ~= 'table' then print(pre, tostring(v)) return end\n"
    "  local ks = {} for k in pairs(v) do ks[#ks + 1] = tostring(k) end table.sort(ks)\n"
    "  for _, k in ipairs(ks) do show(v[k] == nil and v[tonumber(k)] or v[k], pre .. '.' .. k) end\n"
    "end\n"
    "function go(req, pid)\n"
    "  local h, code, msg = canlin.llm.request(pid or 'p', req)\n"
    "  if h then H = h print('req ok') else print('req', code, msg) end\n"
    "  return h\n"
    "end\n"
    "function M.init()\n"
    "  print('llm', type(canlin.llm))\n"
    "  if canlin.llm then show(canlin.llm.profiles(), 'prof') end\n"
    "%s\n"
    "end\n"
    "function M.event(name, p)\n"
    "  if name == 'cancel' then print('cancel', canlin.cancel(H)) return end\n"
    "  if name == 'llm' then\n"
    "    if p.handle ~= H and not MULTI then print('BADHANDLE') end\n"
    "    if p.kind == 'text' then T = T .. p.text print('frag', #p.text)\n"
    "    elseif p.kind == 'tool' then print('tool', p.id, p.name, p.args)\n"
    "    elseif p.kind == 'usage' then print('usage', p.input, p.output)\n"
    "    else print('BADKIND') end\n"
    "    return\n"
    "  end\n"
    "  if name == 'done' then\n"
    "    if #T <= 512 then print('T=' .. T) end\n"
    "    print('textlen', #T)\n"
    "    if p.ok and p.result.dropped_tools ~= nil then\n"
    "      print('done ok', p.result.stop, p.result.dropped_tools)\n"
    "    elseif p.ok then print('done ok', p.result.stop)\n"
    "    else print('done fail', p.code, p.message) end\n"
    "    T = ''\n"
    "  end\n"
    "end\n"
    "return M\n";

static const char req_simple[] =
    "go({messages = {{role = 'user', text = 'hi'}}})";
static const char req_tools[] =
    "go({messages = {{role = 'user', text = 'hi'}}, tools = {'set_view'}})";

static const char text_stream[] =
    ": keep-alive comment\r\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
    "\"content\":\"\"}}]}\r\n\r\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Gr\\u00fc\xc3\x9f"
    "e \"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\xf0\x9f\x91\x8b\\n\\\"ok\\\"\"},"
    "\"finish_reason\":null}]}\r\r"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}\n\n"
    "data: [DONE]\n\n";

static const char text_expect[] = "T=Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x91\x8b\n\"ok\"";

static const char tool_stream_head[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"tool_calls\":"
    "[{\"index\":0,\"id\":\"call_A\",\"type\":\"function\",\"function\":{\"name\":\"set_view\","
    "\"arguments\":\"\"}}]}}]}\n\n";

static const char two_tools_stream[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Moment.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"wid\"}},"
    "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"set_view\",\"arguments\":\"{\\\"x\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":1,\"function\":{\"arguments\":\"\\\":1}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":0,\"function\":{\"arguments\":\"gets\\\":[]}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";

#endif
