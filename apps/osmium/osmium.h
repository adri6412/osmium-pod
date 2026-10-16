/* Osmium Sound UI for Rockbox (FiiO M3K) — public entry points */
#ifndef OSMIUM_H
#define OSMIUM_H
#include <stdbool.h>
#include "gcc_extensions.h"
#include "osm_strings.h"

/* boot */
void osm_boot_splash(void);
/* a diagnostic build (-DOSM_BOOT_TRACE) prints each boot step on screen,
 * with the backlight forced on, to see where a device stops */
#ifdef OSM_BOOT_TRACE
void osm_boot_trace(const char *step);
#define OSM_TRACE(step) osm_boot_trace(step)
#else
#define OSM_TRACE(step) do { } while (0)
#endif
void osm_boot_message(const char *msg, int step, int steps);
void osm_fatal(const char *msg, int code);

/* the interface: never returns */
void osm_ui_run(void) NORETURN_ATTR;

/* USB mass storage screen (blocks until unplugged) */
void osm_usb_screen(void);

/* ── hooks used by the Rockbox core (misc.c, playlist.c) ──────────── */
/* true if the user pressed Back while a long operation runs */
bool osm_user_abort(void);
/* a modal yes/no question; true = yes */
bool osm_confirm(const char *question);
/* a new rockbox.m3k was copied over USB: start it now? */
bool osm_confirm_reboot_new_firmware(void);
/* the files may have changed (USB, card swap): refresh the library */
void osm_library_changed(void);
/* last screen before the power goes */
void osm_shutdown_screen(bool reboot, bool battery_low);

#endif
