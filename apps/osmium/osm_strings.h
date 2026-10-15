#ifndef OSM_STRINGS_H
#define OSM_STRINGS_H
enum osm_str_id {
    S_BOOT_SCANNING,
    S_BOOT_DATABASE,
    S_BOOT_NO_PARTITION,
    S_COUNT_
};
const char *osm_str(enum osm_str_id id);
#endif
