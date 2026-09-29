/*
 * ui_theme.h - dark theme tokens for the whole UI (spec 51/52/54/107).
 *
 * Colour is never the only carrier of state: every coloured element also has
 * text or an icon saying the same thing (spec 51).
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* palette: dark background, bright digits, clear separation (spec 52) */
#define UI_COL_BG      0x0E1116u   /* near-black blue               */
#define UI_COL_SURFACE 0x1A2029u   /* cards / bars                  */
#define UI_COL_SURFACE2 0x232B36u  /* nested elements               */
#define UI_COL_TXT     0xE8ECF1u   /* primary text                  */
#define UI_COL_DIM     0x8A94A3u   /* secondary text                */
#define UI_COL_OK      0x34C77Bu   /* normal (green family)         */
#define UI_COL_WARN    0xF5B944u   /* warning (amber family)        */
#define UI_COL_CRIT    0xF5475Bu   /* severe (red family)           */
#define UI_COL_INFO    0x4D9FFFu   /* information (blue family)     */
#define UI_COL_ACCENT  0x2FA4FFu   /* interactive accent            */

/* font hierarchy (spec 54): 1st speed/core state, 2nd secondary values,
 * 3rd descriptions, 4th diagnostics */
#define F_SM   (&lv_font_montserrat_14)
#define F_MD   (&lv_font_montserrat_16)
#define F_LG   (&lv_font_montserrat_20)
#define F_XL   (&lv_font_montserrat_28)
#define F_XXL  (&lv_font_montserrat_48)

void ui_theme_apply_screen(lv_obj_t *scr);

/* Card: rounded surface container. */
lv_obj_t *ui_card(lv_obj_t *parent);

/* Simple full-width header for sub pages: BACK button + centred title. */
lv_obj_t *ui_header(lv_obj_t *parent, const char *title, lv_event_cb_t on_back);

/* Key/value row inside a card; returns the value label (right aligned). */
lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *name, lv_obj_t **val_out);

/* Primary / danger button factories (large touch targets, spec 2.3). */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg,
                    lv_event_cb_t cb, void *user_data);

/* Colour for a link/vehicle status. */
lv_color_t ui_col_for_state(bool ok, bool warn, bool crit);

/*
 * Cached label writes: identical text does not touch the object, so the 10 Hz
 * refresh only invalidates what actually changed.  Unconditional setText at
 * 10 Hz keeps the RGB bounce-buffer feed busy and shows up as screen jitter.
 */
void ui_label_set_text(lv_obj_t *lbl, const char *txt);
void ui_label_set_fmt(lv_obj_t *lbl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Colour twin of the cached setters: same colour does not invalidate. */
void ui_label_set_color(lv_obj_t *obj, lv_color_t col);

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
