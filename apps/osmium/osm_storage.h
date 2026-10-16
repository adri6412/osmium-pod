/* Osmium Sound for Rockbox: the system (.rockbox) on the internal storage */
#ifndef OSM_STORAGE_H
#define OSM_STORAGE_H
#include <stdbool.h>

/* boot, after the disks are mounted: point /.rockbox at the internal
 * storage if the system lives there (before the settings are read) */
void osm_storage_mount(void);
/* boot, after the settings are read: carry out a move asked for in the
 * settings, take a release copied to the card, update the firmware in
 * flash. May restart the player. */
void osm_storage_boot(void);

/* the player has internal storage */
bool osm_storage_available(void);
/* the system lives on the internal storage */
bool osm_storage_system_internal(void);
/* a card with a file system is in */
bool osm_storage_card_present(void);
/* "x free of y" for the internal storage, or why it has none */
void osm_storage_info(char *buf, int len);
/* the disks changed: restarts if a card came in while the internal
 * storage was the root */
void osm_storage_fs_changed(void);
/* ask for the system to move at the next start, and restart */
void osm_storage_request_move(bool to_internal);

#endif
