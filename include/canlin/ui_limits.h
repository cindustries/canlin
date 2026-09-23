/* CanLin UI and theme — numeric limits.
 *
 * Initial values from docs/superpowers/specs/2026-09-23-canlin-ui-design.md §8, to be
 * checked on P2 hardware. Nothing outside this header may hard-code them.
 */
#ifndef CANLIN_UI_LIMITS_H
#define CANLIN_UI_LIMITS_H

/* Widgets per draw. */
#define CANLIN_UI_MAX_WIDGETS           512u
/* Nesting of section/columns. */
#define CANLIN_UI_MAX_DEPTH             4u
/* One text/bubble/area string, bytes. */
#define CANLIN_UI_MAX_TEXT              (16u * 1024u)
/* Labels, titles, options, bytes. */
#define CANLIN_UI_MAX_LABEL             256u
/* Choice options. */
#define CANLIN_UI_MAX_OPTIONS           64u
/* Graph editor. Ports are per node, inputs plus outputs. */
#define CANLIN_UI_GRAPH_MAX_NODES       256u
#define CANLIN_UI_GRAPH_MAX_PORTS       16u
#define CANLIN_UI_GRAPH_MAX_EDGES       1024u
/* Line segments per cubic Bezier edge. src/render uses it for every
 * NK_COMMAND_CURVE (graph edges are the only curves the UI emits). */
#define CANLIN_UI_GRAPH_CURVE_SEGMENTS  16u
/* set_view: JSON argument bytes; widgets in one model view (graph nodes separate). */
#define CANLIN_UI_TOOL_MAX_BYTES        (64u * 1024u)
#define CANLIN_UI_TOOL_MAX_WIDGETS      128u
/* Theme blobs, bytes: per font, and the logo PNG. */
#define CANLIN_THEME_FONT_MAX           (2u * 1024u * 1024u)
#define CANLIN_THEME_LOGO_MAX           (256u * 1024u)
/* Logo decoded dimensions, px per side (§6.1 "<= 1024x1024 decoded"). */
#define CANLIN_THEME_LOGO_MAX_SIDE      1024u

/* Not in the §8 table: format bounds of the §6.1 values, checked before parsing.
 * "#RRGGBB" is exactly 7 bytes; font size 10-40 fits 2 digits; spacing/radius are
 * short decimal strings ("0.25", "1.5"). */
#define CANLIN_THEME_COLOR_LEN          7u
#define CANLIN_THEME_SIZE_MAXLEN        2u
#define CANLIN_THEME_DECIMAL_MAXLEN     8u

/* Not in the §8 table: flow layout (UI spec §2, system design §5), in em. */
#define CANLIN_UI_MIN_TOUCH_EM          2.5f   /* interactive element height */
#define CANLIN_UI_COLUMNS_EM            40.0f  /* ui:columns side by side at >= this */
#define CANLIN_UI_LINE_EM               1.25f  /* text line height per font size */
/* Heading font sizes per level 1-3, relative to em (level 3 uses the body font). */
#define CANLIN_UI_HEADING1_SCALE        1.5f
#define CANLIN_UI_HEADING2_SCALE        1.25f

/* Not in the §8 table: per-view bounds of src/ui, chosen here (provisional).
 * Queued input events between two frames; overflow drops the queue. */
#define CANLIN_UI_INPUT_MAX             256u
/* Layout record entries per frame (widgets plus wrapped lines); more are not recorded. */
#define CANLIN_UI_RECORD_MAX            4096u
/* Identity hash table slots per view; power of two, >= 4 x CANLIN_UI_MAX_WIDGETS (the
 * previous and the current frame can each hold MAX_WIDGETS identities; load <= 50%). */
#define CANLIN_UI_STATE_SLOTS           2048u

/* §6.2: text/background contrast below this ratio logs a warning (WCAG AA). */
#define CANLIN_THEME_MIN_CONTRAST       4.5f

#endif
