/* CanLin software rasterizer — numeric limits.
 *
 * Not in the UI spec §8 table; chosen here so every per-command loop is bounded.
 * Curve subdivision uses CANLIN_UI_GRAPH_CURVE_SEGMENTS (ui_limits.h).
 */
#ifndef CANLIN_RENDER_LIMITS_H
#define CANLIN_RENDER_LIMITS_H

/* Line/stroke thickness in px; larger values are clamped. */
#define CANLIN_RENDER_MAX_THICKNESS     16
/* Points of one polygon/polyline command; larger commands are skipped whole. */
#define CANLIN_RENDER_MAX_POLY_POINTS   64
/* Font sizes baked into one atlas (body plus heading levels). */
#define CANLIN_RENDER_FONT_SIZES        4

#endif
