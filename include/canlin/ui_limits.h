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
/* Baked font pixel sizes held at once (§2.1): the bound of the UI's font cache, one
 * entry per distinct em px size. The faces are not counted: one atlas per size holds
 * the body and both heading sizes. */
#define CANLIN_UI_FONT_SIZES_MAX        4u
/* Theme blobs, bytes: per font, and the logo PNG. */
#define CANLIN_THEME_FONT_MAX           (2u * 1024u * 1024u)
#define CANLIN_THEME_LOGO_MAX           (256u * 1024u)
/* Logo decoded dimensions, px per side (§6.1 "<= 1024x1024 decoded"). */
#define CANLIN_THEME_LOGO_MAX_SIDE      1024u

/* Not in the §8 table: the value ranges of the §6.1 theme keys, checked before a value
 * is stored (§6.1 table): theme#font#size 10-40 px at DPI 1.0; theme#spacing 0.25-4.0
 * (em multiplier); theme#radius 0.0-2.0 em. */
#define CANLIN_THEME_FONT_SIZE_MIN      10
#define CANLIN_THEME_FONT_SIZE_MAX      40
#define CANLIN_THEME_SPACING_MIN        0.25f
#define CANLIN_THEME_SPACING_MAX        4.0f
#define CANLIN_THEME_RADIUS_MIN         0.0f
#define CANLIN_THEME_RADIUS_MAX         2.0f

/* Not in the §8 table: format bounds of the §6.1 values, checked before parsing.
 * "#RRGGBB" is exactly 7 bytes; font size 10-40 fits 2 digits; spacing/radius are
 * short decimal strings ("0.25", "1.5"). */
#define CANLIN_THEME_COLOR_LEN          7u
#define CANLIN_THEME_SIZE_MAXLEN        2u
#define CANLIN_THEME_DECIMAL_MAXLEN     8u
/* §6.1 "decimal string": spacing and radius are read in 1/10000 em, so a value keeps
 * at most 4 fraction digits ("1.5"). */
#define CANLIN_THEME_FRAC_DIGITS_MAX    4u
/* §6.2: the colour mix: the second colour's share, in percent of this base (the base
 * alone is the second colour, 0 % the first). Its own base, apart from the scale's
 * (CANLIN_UI_SCALE_BASE_PERCENT): the same digit, a different meaning. */
#define CANLIN_THEME_MIX_BASE_PERCENT   100

/* §2: "Interactive elements are at least 2.5 em high" (system design §5); "below 40 em
 * of content width everything is one column". In em. */
#define CANLIN_UI_MIN_TOUCH_EM          2.5f   /* interactive element height */
#define CANLIN_UI_COLUMNS_EM            40.0f  /* ui:columns side by side at >= this */
/* Not in the §8 table: chosen here (provisional). The spec names no line height and no
 * heading scale (§6.3: "heading levels use scale factors of the heading font"). */
#define CANLIN_UI_LINE_EM               1.25f  /* text line height per font size */
/* Heading font sizes per level 1-3, relative to em (level 3 uses the body font). */
#define CANLIN_UI_HEADING1_SCALE        1.5f
#define CANLIN_UI_HEADING2_SCALE        1.25f

/* §2: "Spacing unit = theme#spacing x 0.25 em. View padding and the gap between widgets
 * are 2 units; the gap between columns is 4 units." */
#define CANLIN_UI_UNIT_SCALE            0.25f  /* theme#spacing x this = one unit */
#define CANLIN_UI_PAD_UNITS             2      /* view padding, in units */
#define CANLIN_UI_GAP_UNITS             2      /* gap between widgets, in units */
#define CANLIN_UI_COL_GAP_UNITS         4      /* gap between columns, in units */

/* §3 widget table: ui:area "multi-line; opts.rows 2-20"; ui:toggle "1.25 em framed box,
 * label after one widget gap"; ui:meter "value clamped to 0.0-1.0". */
#define CANLIN_UI_AREA_ROWS_MIN         2      /* ui:area opts.rows */
#define CANLIN_UI_AREA_ROWS_MAX         20
#define CANLIN_UI_TOGGLE_BOX_EM         1.25f  /* ui:toggle framed box */
#define CANLIN_UI_METER_MIN             0.0f   /* ui:meter value clamp */
#define CANLIN_UI_METER_MAX             1.0f

/* §2.1: "Steps: 75, 100, 125, 150, 200 %; default 100 %." Fixed by the spec. */
#define CANLIN_UI_SCALE_STEP_75         75
#define CANLIN_UI_SCALE_STEP_100        100
#define CANLIN_UI_SCALE_STEP_125        125
#define CANLIN_UI_SCALE_STEP_150        150
#define CANLIN_UI_SCALE_STEP_200        200
/* The base the scale factor is divided by: the 100 % step, where the factor is the
 * output DPI factor alone. */
#define CANLIN_UI_SCALE_BASE_PERCENT    100

/* §8 table: the output DPI factor (outside the bounds is rejected) and the stable output
 * identity, in bytes. The identity is the platform's output identity (a settings key);
 * the buffer that holds it is one byte longer, for the terminator. */
#define CANLIN_UI_DPI_MIN               0.5f
#define CANLIN_UI_DPI_MAX               4.0f
#define CANLIN_UI_OUTPUT_ID_MAX         63u
#define CANLIN_UI_OUTPUT_ID_BUF         (CANLIN_UI_OUTPUT_ID_MAX + 1u)

/* Not in the §8 table: per-view bounds of src/ui, chosen here (provisional).
 * Queued input events between two frames; overflow drops the queue. */
#define CANLIN_UI_INPUT_MAX             256u
/* Layout record entries per frame (widgets plus wrapped lines); more are not recorded. */
#define CANLIN_UI_RECORD_MAX            4096u
/* Identity hash table slots per view; power of two, >= 4 x CANLIN_UI_MAX_WIDGETS (the
 * previous and the current frame can each hold MAX_WIDGETS identities; load <= 50%). */
#define CANLIN_UI_STATE_SLOTS           2048u

/* §7: "A view has at most 8 graphs" - a model view (the assistant's set_view) holds
 * at most this many graphs. */
#define CANLIN_UI_GRAPH_MAX_PER_VIEW    8u

/* Not in the §8 table: graph editor (UI spec §5), chosen here (provisional). */
#define CANLIN_UI_GRAPH_MAX_PER_DRAW    8u      /* ui:graph calls per draw */
#define CANLIN_UI_GRAPH_MAX_BODY        4u      /* body rows per node (§5.3) */
#define CANLIN_UI_GRAPH_MAX_COORD       4096.0f /* |x|, |y| of a node and of the pan, em */
#define CANLIN_UI_GRAPH_HEIGHT_EM       24.0f   /* default opts.height */
#define CANLIN_UI_GRAPH_HEIGHT_MIN_EM   5.0f
#define CANLIN_UI_GRAPH_HEIGHT_MAX_EM   200.0f
#define CANLIN_UI_GRAPH_NODE_W_EM       12.0f   /* node panel width */
#define CANLIN_UI_GRAPH_TITLE_EM        2.0f    /* title row height */
#define CANLIN_UI_GRAPH_BODY_EM         1.5f    /* body row height */
#define CANLIN_UI_GRAPH_SLOP_EM         0.5f    /* movement below this is a tap */
#define CANLIN_UI_GRAPH_EDGE_HIT_EM     0.5f    /* tap distance that selects an edge */
/* Pointer timing, ms between input timestamps (canlin_ui_input.t). */
#define CANLIN_UI_LONG_PRESS_MS         500u    /* press held this long, released in place */
#define CANLIN_UI_DOUBLE_TAP_MS         400u    /* second tap on the same node */

/* Not in the §8 table: on-screen keyboard (canlin#45), chosen here (provisional). */
#define CANLIN_UI_KBD_KEY_EM            2.5f    /* key side when the view is wide enough */
#define CANLIN_UI_KBD_KEY_MIN_EM        1.5f    /* smallest key height (and square side) */

/* §6.2: text/background contrast below this ratio logs a warning (WCAG AA). */
#define CANLIN_THEME_MIN_CONTRAST       4.5f

#endif
