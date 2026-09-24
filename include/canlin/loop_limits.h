/* CanLin native main loop — numeric limits.
 *
 * v1 values from docs/superpowers/specs/2026-09-24-canlin-native-loop-design.md. Nothing
 * outside this header may hard-code them. Values marked "not in the spec" were chosen by
 * the canlin#61 implementation where the spec is silent.
 */
#ifndef CANLIN_LOOP_LIMITS_H
#define CANLIN_LOOP_LIMITS_H

/* Registered sources (spec §2), fixed at start-up. */
#define CANLIN_LOOP_SOURCES_MAX      8u

/* Not in the spec (it names the constant, not its value): scheduler turns per
 * iteration (step 1). Each turn makes at most CANLIN_EV_TURN_MAX_DISPATCH callbacks. */
#define CANLIN_LOOP_TURNS_MAX        4u

/* SDL2 wait (spec §4): cap of the SDL wait timeout while any source has descriptors.
 * Used by the SDL2 backend ticket; the native POSIX wait never caps. */
#define CANLIN_LOOP_SDL_NET_TICK_MS  10u

/* Not in the spec: descriptors of one wait, the wake pipe included. More wanted by the
 * sources than fit makes the iteration not wait (timeout 0) and counts an overflow, so
 * readiness is never lost; 32 is far above today's need (canlin_http: at most
 * CANLIN_HTTP_POLLFDS_MAX = 5). */
#define CANLIN_LOOP_POLLFDS_MAX      32u

/* Not in the spec: outputs of one display (the shell adapter); equals
 * CANLIN_SHELL_OUTPUTS_MAX. */
#define CANLIN_LOOP_OUTPUTS_MAX      4u

#endif
