/* Osmium Sound: the notification API the Rockbox core calls into.
 * The old splash screens are drawn by the Osmium UI as toasts / overlays
 * (osmium/osm_overlay.c). */
#ifndef OSM_SPLASH_H
#define OSM_SPLASH_H
#include <stdbool.h>
#include "gcc_extensions.h"

/* show a message for `ticks` (0: until something else is drawn) */
void splash(int ticks, const char *str);
void splashf(int ticks, const char *fmt, ...) ATTRIBUTE_PRINTF(2, 3);

/* a progress bar with a message; shown after the delay set below */
void splash_progress(int current, int total, const char *fmt, ...) ATTRIBUTE_PRINTF(3, 4);
void splash_progress_set_delay(long delay_ticks);

#endif
