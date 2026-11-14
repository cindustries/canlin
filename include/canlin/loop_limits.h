/* CanLin native main loop — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-24-canlin-native-loop-design.md. Nothing
 * outside this header may hard-code them. A value marked "not in the spec" says where it
 * comes from: another spec's constant, or the implementation that chose it.
 */
#ifndef CANLIN_LOOP_LIMITS_H
#define CANLIN_LOOP_LIMITS_H

/* Registered sources (spec §2), fixed at start-up. */
#define CANLIN_LOOP_SOURCES_MAX      8u

/* Scheduler turns per iteration (spec §3, step 1): "canlin_ev_turn() until !more or
 * CANLIN_LOOP_TURNS_MAX (4) turns". Each turn makes at most
 * CANLIN_EV_TURN_MAX_DISPATCH callbacks. */
#define CANLIN_LOOP_TURNS_MAX        4u

/* SDL2 wait (spec §4): cap of the SDL wait timeout while any source has descriptors.
 * Used by the SDL2 backend ticket; the native POSIX wait never caps. */
#define CANLIN_LOOP_SDL_NET_TICK_MS  10u

/* Descriptors of one wait, the wake pipe included (spec §3): "More descriptors than
 * CANLIN_LOOP_POLLFDS_MAX (32) is counted as an overflow and the loop does not wait at
 * all rather than wait on a subset." More wanted by the sources than fit makes the
 * iteration not wait (timeout 0) and counts an overflow, so readiness is never lost;
 * 32 is far above today's need (canlin_http: at most CANLIN_HTTP_POLLFDS_MAX = 5). */
#define CANLIN_LOOP_POLLFDS_MAX      32u

/* Not in the spec: outputs of one display (the shell adapter); equals
 * CANLIN_SHELL_OUTPUTS_MAX. */
#define CANLIN_LOOP_OUTPUTS_MAX      4u

#endif
