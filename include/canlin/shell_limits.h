/* CanLin shell — numeric limits.
 *
 * Shell spec §6 (docs/superpowers/specs/2026-09-23-canlin-shell-design.md). Sizes in em
 * of the output (theme font size x DPI factor x UI scale).
 */
#ifndef CANLIN_SHELL_LIMITS_H
#define CANLIN_SHELL_LIMITS_H

/* §6 table. */
#define CANLIN_SHELL_PANES_MAX          3       /* panes per output */
/* §2: "2 or 3 panes in one row or one column"; §6: "Every output keeps a tiling of 2-3
 * slots". The §6 table names only the maximum. */
#define CANLIN_SHELL_PANES_MIN          2       /* panes per output */
#define CANLIN_SHELL_PANE_MIN           30.0f   /* em, pane extent in the split direction */
#define CANLIN_SHELL_TILED_MIN          60.0f   /* em output width: tiled available */
#define CANLIN_SHELL_TILED_DEFAULT      80.0f   /* em output width: tiled by default */
#define CANLIN_SHELL_BAR_HEIGHT         2.5f    /* em, status bar */

/* §6 table, the last five rows: chosen during canlin#35 and provisional. */
#define CANLIN_SHELL_DIVIDER_HIT        2.5f    /* em, divider hit area (§2) */
#define CANLIN_SHELL_OUTPUTS_MAX        4       /* outputs one shell arranges */
#define CANLIN_SHELL_MODULES_MAX        32      /* modules known to the switcher */
#define CANLIN_SHELL_SWITCHER_WIDTH     24.0f   /* em, switcher panel (at most the output) */
#define CANLIN_SHELL_SWIPE              2.5f    /* em downwards from the bar opens the switcher */

/* Not in the §6 table: chosen here (provisional). */
/* A row of the shell: the switcher entries, the restart button and the text rows of
 * the faulted pane. */
#define CANLIN_SHELL_ROW_HEIGHT         2.5f    /* em, row height */
/* The width of the restart button of a faulted pane; the spec states no size. */
#define CANLIN_SHELL_RESTART_WIDTH      10.0f   /* em, width of the restart button */
#define CANLIN_SHELL_SWITCH_BUTTON      6.0f    /* em, switcher entry in the status bar */
#define CANLIN_SHELL_MODEL_LABEL        1.5f    /* em, "model-generated" strip of a frame */

#endif
