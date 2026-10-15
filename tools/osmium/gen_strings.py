#!/usr/bin/env python3
"""Osmium Sound for Rockbox: the interface strings (English, Italiano).

tools/osmium/strings.txt holds one string per line, `id|English|Italiano`;
this writes apps/osmium/osm_strings.[ch].

    python3 tools/osmium/gen_strings.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GEN = "/* Generated from tools/osmium/strings.txt by tools/osmium/gen_strings.py — do not edit. */\n"


def cs(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    rows = [l.rstrip("\n").split("|") for l in open(os.path.join(HERE, "strings.txt"), encoding="utf-8")
            if l.strip() and not l.startswith("#")]
    ids = [r[0] for r in rows]
    assert len(ids) == len(set(ids)), "duplicate string id"
    h = [GEN, "#ifndef OSM_STRINGS_H\n#define OSM_STRINGS_H\n\nenum osm_str_id {\n"]
    h += [f"    S_{i},\n" for i in ids]
    h.append("    S_COUNT_\n};\n\n/* the string in the language chosen in Settings */\n"
             "const char *osm_str(enum osm_str_id id);\n\n#endif\n")
    c = [GEN, '#include "settings.h"\n#include "osm_strings.h"\n#include "osm_settings.h"\n\n',
         "static const char *const strings[OSM_LANG_COUNT][S_COUNT_] = {\n    [OSM_LANG_ENGLISH] = {\n"]
    c += [f"        {cs(r[1])},\n" for r in rows]
    c.append("    },\n    [OSM_LANG_ITALIAN] = {\n")
    c += [f"        {cs(r[2])},\n" for r in rows]
    c.append("    },\n};\n\nconst char *osm_str(enum osm_str_id id)\n{\n"
             "    int lang = global_settings.osm_language;\n"
             "    if ((unsigned)lang >= OSM_LANG_COUNT)\n        lang = OSM_LANG_ENGLISH;\n"
             "    return (unsigned)id < S_COUNT_ ? strings[lang][id] : \"\";\n}\n")
    open(os.path.join(ROOT, "apps", "osmium", "osm_strings.h"), "w", encoding="utf-8").write("".join(h))
    open(os.path.join(ROOT, "apps", "osmium", "osm_strings.c"), "w", encoding="utf-8").write("".join(c))
    print(f"{len(ids)} strings")


if __name__ == "__main__":
    main()
