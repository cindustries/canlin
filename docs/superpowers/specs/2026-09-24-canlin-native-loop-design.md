# CanLin — native main loop

Status: implemented for canlin#61 (2026-09-24); §3 updated to the implementation.
It implements the rawfb / native POSIX and SDL2 rows of the [event runtime design](2026-09-22-canlin-event-runtime-design.md) §3
and is where the [HTTP transport](2026-09-23-canlin-http-transport-design.md) §3 drive
API, `canlin_llm_pump` and shell rendering meet. The browser loop is a separate slice.

## 1. Why a shared loop

No backend exists yet. rawfb and SDL2 differ only in how they **wait** and where input
and pixels go; the order of work per iteration is the same. One `canlin_loop` owns that
order; a backend supplies a wait function and its own input/present hooks. Tests drive
the same loop with a fake clock and a fake wait.

## 2. Sources

A source is anything with descriptors and/or a deadline:

```c
typedef struct canlin_loop_source {
    size_t (*pollfds)(void *ctx, struct pollfd *out, size_t max, canlin_ev_time *deadline);
    void   (*step)(void *ctx, canlin_ev_time now);   /* bounded work, owner thread */
    void *ctx;
} canlin_loop_source;
```

`canlin_http` fits as is. Later sources: evdev input (rawfb), MQTT, audio status.
Registration is fixed at start-up, at most `CANLIN_LOOP_SOURCES_MAX` (8). The loop also
owns one **wake pipe** (write end usable from any thread) for future producers that live
on other threads; the HTTP DNS worker keeps its own pipe inside `canlin_http`.

## 3. One iteration

```
now = clock()
1. ev turns: canlin_ev_turn() until !more or CANLIN_LOOP_TURNS_MAX (4) turns
2. pump hook (canlin_loop_llm_pump → canlin_llm_pump; the core does not link LLM/Lua)
3. step every source once (bounded by the source)
4. render: for each output with canlin_shell_dirty(): begin / shell_render / end,
   then backend present — at most once per output per iteration
5. canlin_ev_peek(): steps 2–4 may have queued work after the last turn, so the
   step-1 result is stale; peek reads the current more/deadline without dispatching
6. wait: timeout = 0 if peek says more, else min(ev deadline, source deadlines,
   display tick from canlin_shell_next_tick_ms) - now, or infinite if none
```

Adapters (`loop_http.c`, `loop_llm.c`, `loop_shell.c`) are separate objects, so the
core links only against the scheduler. More descriptors than `CANLIN_LOOP_POLLFDS_MAX`
(32) is counted as an overflow and the loop does not wait at all rather than wait on a
subset.

- Lost wakeups: every producer that can make work runnable either runs on the owner
  thread before step 5 or writes the wake pipe; step 6 computes its timeout from the
  step-5 peek, so nothing enqueued during steps 1–4 is missed.
- Idle: no runnable work, no deadline, no ready descriptor → the wait blocks without a
  timeout. No frame loop.
- Fairness: steps 1–3 are each bounded, so a busy stream cannot starve rendering or
  module turns and vice versa.
- `canlin_loop_run` returns when a quit flag is set (a signal, the backend's close
  event, or the embedder's own rule such as "last module stopped" — the loop does not
  decide that). Quit is safe from the owner thread and signal handlers; another thread
  calls `canlin_loop_wake` and lets its source's step set quit.

## 4. Waiting per backend

- **rawfb / native POSIX:** `poll()` over the collected descriptors + wake pipe + the
  backend's input descriptors, with the computed timeout.
- **SDL2:** SDL has no portable pollable descriptor. v1 uses
  `SDL_WaitEventTimeout(timeout)`, where the timeout is capped at
  `CANLIN_LOOP_SDL_NET_TICK_MS` (10 ms) **only while any source has descriptors**; before
  waiting, a zero-timeout `poll()` on those descriptors skips the wait if one is ready.
  With no network activity the wait is exactly the deadline, so idle stays idle. Cost:
  ≤ 100 wakeups/s and ≤ 10 ms extra latency while a stream is live, on desktop/mobile
  hardware only (the P2 uses rawfb). A helper thread that polls and posts an SDL user
  event is the upgrade path if this proves too slow or too costly; not in v1.
- **Test:** a wait function that advances the fake clock to the computed timeout and
  reports which scripted descriptors are ready.

## 5. Scope of canlin#61

1. `src/loop/loop.c`, `include/canlin/loop.h`: sources, wake pipe, iteration, the poll
   wait, quit.
2. Tests with the fake clock and fake wait: timeout computation (none / deadline /
   zero with work), idle blocks without timeout, wake pipe from another thread wakes the
   wait, bounded turns per iteration, render only when dirty, quit.
3. End-to-end: `canlin_http` as a source against the loopback test server
   (`t/net/tsrv`), a streamed LLM request through `canlin_llm` + the loop until `done`,
   cancel mid-stream — with the real `poll()` wait.
4. The SDL2 wait (§4) is written down here but built with the SDL2 backend ticket, since
   SDL2 is not available in this build yet. The rawfb display/evdev backend is its own
   ticket too.
