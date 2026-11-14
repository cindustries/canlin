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
/* Font sizes one atlas holds: the bound of the size array and of
 * canlin_render_font_default_sizes(). The UI bakes three faces into one atlas - body,
 * H1 and H2 (§2.1: "one atlas per size, holding body and both heading sizes"). */
#define CANLIN_RENDER_FONT_SIZES        4

#endif
