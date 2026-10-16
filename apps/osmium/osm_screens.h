/***************************************************************************
 *   Osmium Sound for FiiO M3K — the screens
 ****************************************************************************/
#ifndef OSM_SCREENS_H
#define OSM_SCREENS_H

#include "osm_ui.h"
#include "osm_library.h"

/* scr_home.c: the library home (tiles), Discover and Settings tabs */
extern const struct osm_screen scr_home;
void home_reset(void);
/* scr_browse.c: a library list, the album grid, an album page */
extern const struct osm_screen scr_browse;
extern const struct osm_screen scr_album;
/* scr_nowplaying.c */
extern const struct osm_screen scr_nowplaying;
/* scr_settings.c: a settings page */
extern const struct osm_screen scr_settings;

/* open a library view (title shown in the crumbs) */
void browse_open(const struct lib_query *q, const char *title);
/* open the page of an album */
void album_open(const char *album, const char *artist);
/* open a settings page (index in the settings tree, see scr_settings.c) */
void settings_open(int page);
/* draw the settings home (tab of scr_home) and drive it */
void settings_home_draw(int top);
bool settings_home_key(int key, int ev);
void settings_home_enter(void);

/* the context menu for a library item (long press) */
void browse_item_menu(const struct lib_item *it, const char *title);
/* play / shuffle what a library item stands for */
void browse_play_item(const struct lib_item *it, bool shuffle);

/* osm_system.c */
void osm_ui_init_fonts(void);
bool osm_ui_tick(void);         /* periodic work; true to redraw */
bool osm_ui_idle(void);         /* when no key is pending */
void osm_resume_at_boot(void);
int  osm_library_serial(void);  /* bumps when the files may have changed */
/* the database is being built: a full screen shows the progress */
bool osm_library_preparing(void);
bool osm_library_needs_restart(void); /* built, but saved at the next start */
void osm_library_screen_draw(void);

#endif
