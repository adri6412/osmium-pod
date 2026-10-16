/* Osmium Sound: settings helpers (see osm_settings.c) */
#ifndef OSM_SETTINGS_H
#define OSM_SETTINGS_H

enum { OSM_LANG_ENGLISH = 0, OSM_LANG_ITALIAN, OSM_LANG_COUNT };
enum { OSM_NP_COVER = 0, OSM_NP_VU, OSM_NP_VIEW_COUNT };

/* push the DSP chain (replaygain, EQ, crossfeed...) to the engine */
void osm_dsp_settings_apply(void);
/* push backlight / brightness / button light */
void osm_display_settings_apply(void);

#endif
