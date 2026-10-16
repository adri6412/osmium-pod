# Architecture

Osmium Pod is the Rockbox core with its user interface replaced. Everything
below the interface (kernel, drivers, codecs, DSP, buffering, playback,
playlists, tag database) is Rockbox; everything the user sees is the
Osmium Sound interface in `apps/osmium/`. This document explains how the
two halves are joined, so that both can change without breaking the other.

```
┌──────────────────────────────────────────────────────────────────────┐
│ apps/osmium/          Osmium Sound interface                         │
│   scr_*.c             screens: home, browse/album, now playing,      │
│                       settings                                       │
│   osm_ui.c            screen stack, keys, overlays, the UI thread    │
│   osm_gfx.c           drawing into a private RGB565 frame            │
│   osm_player.c        player model over the playback engine          │
│   osm_library.c       library views over the tag database, covers   │
│   osm_settings.c      settings storage (replaces apps/settings.c)    │
│   osm_system.c        boot, USB, shutdown and library screens        │
│   main.c              boot sequence (replaces apps/main.c)           │
├───────────── hooks: osmium.h, splash.h ◄──── core calls up ──────────┤
│ apps/                 Rockbox core kept from upstream                │
│   playback.c buffering.c codecs.c pcmbuf.c audio_thread.c            │
│   playlist.c tagcache.c cuesheet.c misc.c talk.c recorder/*          │
├──────────────────────────────────────────────────────────────────────┤
│ lib/rbcodec  lib/fixedpoint  lib/utf8proc  …   codecs, DSP, metadata │
├──────────────────────────────────────────────────────────────────────┤
│ firmware/             kernel, drivers, X1000 SoC, FiiO M3K target    │
└──────────────────────────────────────────────────────────────────────┘
```

## What was removed from Rockbox

The whole Rockbox GUI is gone from the tree, not just disabled:
`apps/gui/` (lists, skin engine, WPS, viewports), `apps/menus/`,
`apps/plugins/`, `apps/keymaps/`, `apps/hosted/`, `wps/`, `backdrops/`,
the file browser (`tree.c`, `filetree.c`), the quick screen, hotkeys and
the root menu. `tools/configure` builds the M3K with `plugins="no"`.

Because the files are absent, any core code that still referred to them
had to be changed. Those changes are the contact surface with upstream and
are listed in [The core changes](#the-core-changes).

## Build integration

* `tools/configure`, target `fiiom3k`: `appextra="osmium"` and
  `plugins="no"`. Configure always prepends `gui:recorder:radio:` to
  `APPEXTRA`. The missing directories are harmless, and `apps/osmium` ends
  up on the include path. That is how the core's `#include "splash.h"`
  finds `apps/osmium/splash.h`.
* `apps/SOURCES` is the fork's own short list: the playback engine, the
  playlist, the tag database, album art, and then `osmium/*.c`. Upstream's
  `apps/SOURCES` is not used.
* Codecs link with `lib/rbcodec/codecs/codec.lds`. This is a copy of
  upstream's `apps/plugins/plugin.lds`, which went away with the plugins.
  Git tracks it as a rename, so upstream changes to `plugin.lds` merge
  into it.
* `tools/buildzip.pl` skips the plugins, themes and WPS when
  `apps/plugins` does not exist.
* Generated assets (`apps/osmium/assets/*`, `apps/osmium/osm_strings.[ch]`)
  are committed. To regenerate them, run `tools/osmium/gen_*.py`.

## Boot

`apps/osmium/main.c` has two `init()` functions: one for the device
(native) and one for the simulator (hosted). The native one follows
Rockbox's order. It brings up the filesystem, LCD, fonts, RTC, the PRNG
(seeded after the RTC), USB, buttons, power management and storage, then
mounts the card. It then loads the settings, starts dircache and the tag
database (with progress on the boot screen), and initialises the playlist
and audio.

If no partition can be mounted, the player offers USB mode so the card
can be fixed, then reboots.

At the end `osm_ui_run()` starts the interface thread. The main thread
then sleeps forever: its code lives in the init section, which is
reclaimed once the system is up.

## Threads

| Thread | Where | Stack | Priority | Job |
|---|---|---|---|---|
| `osm ui` | `osm_ui.c` | 64 KB | user interface | the whole interface |
| `osm covers` | `osm_library.c` | 16 KB | background | album grid thumbnails |
| audio, codec, buffering, voice, tagcache, dircache, usb… | core | — | — | unchanged from Rockbox |

The UI thread loop (`ui_thread()` in `osm_ui.c`) works like this:

1. Every 200 ms, or every frame while something animates, it polls the
   engine (`player_poll()`) and periodic work (`osm_ui_tick()`).
2. It redraws if anything is dirty.
3. It waits for a key with `button_get_w_tmo()`. The timeout is ~33 ms
   while animating, 50 ms while covers are arriving, 200 ms otherwise.
4. System events (`SYS_USB_CONNECTED`, `SYS_POWEROFF`, headphones…) go to
   `default_event_handler()` in `apps/misc.c`.

The interface never blocks the audio threads. It only reads engine state
and sends commands. Long operations in the core, such as building a
playlist or scanning folders, call back into the interface to check for
abort (see the hooks below).

The cover thread decodes album art for the grid into 107×107 thumbnails.
It uses a 512 KB work buffer and reads either the JPEG embedded in the
file (`clip_jpeg_fd`) or a cover file in the folder
(`search_albumart_files`, `read_jpeg_fd` / `read_bmp_fd`). Results
carry a generation number, so covers that arrive after the list changed
are dropped.

## The interface

* **Screens** (`struct osm_screen` in `osm_ui.h`) form a stack: each one
  has enter / resume / leave / draw / key callbacks. Library pages and
  settings pages are pushed and popped with slide transitions. Now Playing
  is not on the stack: it slides up over everything. Overlays (toast,
  dialog, menu sheet) are drawn last and get the keys first.
* **Keys**: the M3K buttons and the scroll wheel map to `enum osm_key`.
  Each key reports press / repeat / short / long events. The interface
  owns the key lock (`firmware/backlight.c` defers to it).
* **Drawing** (`osm_gfx.c`) goes to a private RGB565 frame (`osm_fb`)
  that is sent to the LCD in one piece (`gfx_present()`). Colours are
  ARGB, so translucent fills blend. Text uses pre-rendered anti-aliased
  DejaVu Sans. A Rockbox `.fnt` font (Unifont from the release package)
  is the fallback for scripts DejaVu lacks.
* **Strings**: the interface uses `osm_str(id)`, English or Italian, from
  `tools/osmium/strings.txt`. The core keeps Rockbox's language system
  (`LANG_*`, `apps/lang/`) for the few messages it still produces.

## The player model

`osm_player.c` mirrors the engine state in `struct osm_player`: title,
times, format chip, playlist position, covers. `player_poll()` refreshes
it and returns whether anything visible changed. Commands
(`player_toggle_play()`, `player_play_fill()`, `player_queue_files()`…)
use the public engine and playlist APIs: `audio_*`, `playlist_create`,
`playlist_insert_context_*`, `playlist_start`, `playlist_shuffle`.

Album art comes from the engine itself. `player_init()` claims two slots
with `playback_claim_aa_slot()`: the 156 px cover and a 24 px backdrop
that is stretched and dimmed behind it. `MAX_MULTIPLE_AA` in
`apps/playback.c` is set to 2 for this.

Favorites are a normal playlist, `/.rockbox/favorites.m3u8`.

## The library

`osm_library.c` builds every list from the Rockbox tag database with
`tagcache_search()` / `tagcache_get_next()`:

* artists, albums (one per album and album artist), genres, years,
  composers and tracks;
* "most / recently played", "recently added" and "never played" from the
  runtime statistics;
* folders and playlists, read straight from the card.

Only one list is held at a time (`lib_items`): going back reloads the
previous one.

The database builds and updates itself. After USB or a card swap,
`osm_library_changed()` asks the tag cache for an update. While the
database is being built from scratch, a full screen shows the progress
(`osm_library_preparing()`).

## Settings

`osm_settings.c` replaces Rockbox's `settings.c` / `settings_list.c` and
implements their entry points: `settings_reset`, `settings_load`,
`settings_save`, `status_save` and `settings_apply`. The core can keep
calling them.

* **Fields**: `struct user_settings` in `apps/settings.h` is trimmed to
  what the core and the interface use.
* **Storage**: settings are kept as `name: value` text in
  `/.rockbox/osmium.cfg`. Saving is deferred until the storage is idle,
  then written to a temporary file and renamed.
* **Applying**: `sound_settings_apply()` and `osm_dsp_settings_apply()`
  push the audio and DSP settings (EQ, crossfeed, ReplayGain…) to the
  engine.

## The core changes

These are the hooks the core calls into the interface (`apps/osmium/osmium.h`):

| Hook | Called from | Replaces |
|---|---|---|
| `osm_user_abort()` | `playlist.c` (folder scans, inserts, resume) | `action_userabort()` |
| `osm_confirm()` | `playlist.c` (save with queued tracks) | `yesno_pop()` |
| `osm_confirm_reboot_new_firmware()` | `misc.c` `check_bootfile()` | the yes/no screen |
| `osm_usb_screen(seqnum)` | `misc.c` on `SYS_USB_CONNECTED`, `main.c` | `gui_usb_screen_run()` |
| `osm_shutdown_screen()` | `misc.c` `clean_shutdown()` | the shutdown splash |
| `osm_library_changed()` | `misc.c` after USB / card change | `tree_restore()` |
| `splash()`, `splashf()`, `splash_progress()` | anywhere in the core | `apps/gui/splash.c` |

Core files the fork modified (compare with `git diff <fork point> -- <file>`):

* **`apps/misc.c`, `apps/misc.h`**: shutdown, USB and event handling go
  through the hooks above. `pause_action()` / `unpause_action()` stay
  here. The GUI helpers are removed.
* **`apps/playlist.c`**: folder scanning no longer borrows the file
  browser's cache. It has its own sorted directory list (`pl_dir`,
  guarded by a recursive mutex) for "play the folder" and folder advance.
  Random folder advance is not available.
* **`apps/settings.h`**: the trimmed `struct user_settings`.
* **`apps/tagcache.c`**: commit without a confirmation dialog; a commit
  without dircache allocates its own buffer; no endless reload loop when
  there is nothing to commit.
* **`apps/playback.c`**: two album art slots. `apps/codecs.c`,
  `apps/cuesheet.*`, `apps/talk.*`, `apps/language.c` and `apps/recorder/*`
  only lose their GUI includes and calls.
* **`firmware/`**:
  * `export/config/fiiom3k.h` has `CODEC_SIZE` set to 2 MiB and no quick
    screen, hotkeys or A-B repeat.
  * `target/mips/ingenic_x1000/app.lds` keeps its sections 4-byte aligned:
    `crt0.S` copies 4 bytes at a time, and a misaligned boundary hangs the
    player at boot with a black screen. The CI checks this.
  * `backlight.c`, `usb.c`, `powermgmt.c`, `logf.c`, `drivers/axp-pmu.c`
    and `drivers/lcd-scroll.c` only lose their GUI references.
* **Build**: `tools/configure`, `tools/buildzip.pl`, `apps/SOURCES`,
  `firmware/SOURCES` (no `debug-x1000.c`, the debug screen is gone),
  `lib/rbcodec/codecs/codecs.make` (`codec.lds`).
* **Simulator**: `uisimulator/common/*` and
  `firmware/target/hosted/sdl/window-sdl.c`.

## Keeping up with Rockbox

The core follows Rockbox master (last merge: upstream `a7c11ccc78`,
2026-10-14). To update it:

```bash
git remote add upstream https://github.com/Rockbox/rockbox.git   # once
git fetch upstream master
git checkout -b merge/rockbox-upstream-<date>
git merge --no-commit upstream/master
```

To resolve the merge:

* **Modify/delete conflicts** on files the fork removed: keep them
  deleted. Also delete anything upstream *added* under the removed
  directories (`apps/gui`, `apps/plugins`, `apps/keymaps`, `apps/hosted`,
  `apps/menus`, `wps`, `backdrops`).
* **Files upstream moved out of a removed directory into the core**: keep
  them. For example, `apps/plugins/lib/strncpy.c` became
  `firmware/libc/strncpy.c`. Check that every file of `upstream/master`
  that is not under a removed directory is present after the merge.
* **Content conflicts** in the files listed above: keep the fork's side,
  then take over upstream's core changes by hand (new hooks, new fields,
  new conditions).
* **Git's directory-rename guesses**: these can move new plugin files
  into `lib/rbcodec/codecs/`. Delete them.

Then check, comparing against a build of the previous master:

1. Simulator: `configure --target=fiiom3k --type=s && make`. There
   should be no new warnings: an implicit declaration or a missing
   initializer usually means an upstream API changed. Then run it.
2. Device: `configure --target=fiiom3k --type=N --ram=0 && make && make
   zip`, plus the CI alignment check on `rockbox.elf`.
3. Look for API changes that still compile but change behaviour. Two
   from the October 2026 merge:
   * `usb_acknowledge()` gained the event's sequence number: without it
     the ack is dropped and USB mode never starts.
   * `struct eq_band_setting` gained a leading filter type: positional
     initializers shift.
4. Look at what upstream's `apps/main.c` does at boot and mirror the new
   parts in `apps/osmium/main.c` (for example `prng_init()`).

## Releases

`.github/workflows/rockbox.yml` builds the firmware on every push. On a
`v*` tag it also publishes a GitHub release with
`osmium-pod-fiiom3k-<version>.zip`: the `.rockbox` folder plus a
Unifont fallback font. It never contains a bootloader: the firmware runs
on the official Rockbox bootloader for the M3K.
