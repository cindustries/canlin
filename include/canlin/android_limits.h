/* CanLin Android adapter — numeric limits.
 *
 * docs/superpowers/specs/2026-09-30-canlin-android-adapter-design.md. The spec names no
 * values for these; all are chosen by the canlin#72 implementation ("not in the spec").
 * Nothing outside this header may hard-code them.
 */
#ifndef CANLIN_ANDROID_LIMITS_H
#define CANLIN_ANDROID_LIMITS_H

/* Not in the spec: pointers of one motion event that are looked at. v1 tracks only the
 * first pointer (§6.1); the rest are copied only to find it by id. */
#define CANLIN_ANDROID_POINTERS_MAX  10u

/* Not in the spec: glue commands handled per loop iteration (§3 "drained within the
 * loop's bounded steps"). The command pipe is level-triggered, so the rest comes next. */
#define CANLIN_ANDROID_CMDS_MAX      8u

/* Not in the spec: input events taken from the AInputQueue per loop iteration. When the
 * bound is hit the source asks for another iteration without waiting. */
#define CANLIN_ANDROID_INPUTS_MAX    32u

/* Not in the spec: after a failed ANativeWindow_lock the full present is retried no
 * sooner than this (§5: a failed lock never blocks the loop; it must not spin either). */
#define CANLIN_ANDROID_LOCK_RETRY_MS 100u

#endif
