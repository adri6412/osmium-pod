/***************************************************************************
 *   Osmium Sound for FiiO M3K
 *
 *   Boot sequence: the Rockbox core (kernel, storage, audio hardware,
 *   power management, USB, playback engine, tag database) is brought up
 *   exactly as upstream Rockbox does it, but every user-facing part is
 *   handed over to the Osmium UI (osm_ui_run()), which never returns.
 *
 *   Based on apps/main.c from Rockbox (GPL v2 or later).
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "version.h"
#include "gcc_extensions.h"
#include "storage.h"
#include "disk.h"
#include "file_internal.h"
#include "lcd.h"
#include "rtc.h"
#include "debug.h"
#include "led.h"
#include "../kernel-internal.h"
#include "button.h"
#include "panic.h"
#include "usb.h"
#include "powermgmt.h"
#include "adc.h"
#include "i2c.h"
#include "audio.h"
#include "settings.h"
#include "backlight.h"
#include "font.h"
#include "language.h"
#include "playlist.h"
#include "core_alloc.h"
#include "power.h"
#include "talk.h"
#include "misc.h"
#include "dircache.h"
#include "tagcache.h"
#include "lang.h"
#include "string.h"
#include "bootchart.h"
#include "bootdata.h"
#include "rbunicode.h"
#include "audio_thread.h"
#include "playback.h"
#include "pathfuncs.h"
#include "dsp_core.h"
#include "osmium.h"
#ifdef OSM_BOOT_TRACE
#include "backlight-target.h"
#endif

#if (CONFIG_PLATFORM & PLATFORM_SDL)
#ifdef SIMULATOR
#include "sim_tasks.h"
#endif
#include "system-sdl.h"
#define HAVE_ARGV_MAIN
#if defined(WIN32)
#undef main
#endif
#endif

#if !(CONFIG_PLATFORM & PLATFORM_HOSTED)
#define MAIN_NORETURN_ATTR NORETURN_ATTR
#else
#define MAIN_NORETURN_ATTR
#endif

static void init(void);

#ifdef HAVE_ARGV_MAIN
int main(int argc, char *argv[]) INIT_ATTR MAIN_NORETURN_ATTR;
int main(int argc, char *argv[])
{
    sys_handle_argv(argc, argv);
#else
int main(void) INIT_ATTR MAIN_NORETURN_ATTR;
int main(void)
{
#endif
    CHART(">init");
    init();
    CHART("<init");

#ifdef HAVE_USBSTACK
    /* all threads exist and public queues are registered by now */
    usb_start_monitoring();
#endif

    global_status.last_volume_change = 0;
    OSM_TRACE("usb monitoring");
    osm_ui_run();       /* never returns */
#if (CONFIG_PLATFORM & PLATFORM_HOSTED)
    return 0;
#endif
}

#ifdef HAVE_DIRCACHE
static int INIT_ATTR init_dircache(bool preinit)
{
    if (preinit)
        dircache_init(MAX(global_status.dircache_size, 0));

    if (!global_settings.dircache)
        return -1;

    int result = -1;
    if (!preinit)
    {
        result = dircache_enable();
        if (result > 0)
        {
            osm_boot_message(osm_str(S_BOOT_SCANNING), -1, -1);
            dircache_wait();
            backlight_on();
        }
        if (result >= 0)
        {
            struct dircache_info info;
            dircache_get_info(&info);
            global_status.dircache_size = info.size;
            status_save(true);
        }
    }
    return result;
}
#endif /* HAVE_DIRCACHE */

#ifdef HAVE_TAGCACHE
static void INIT_ATTR init_tagcache(void)
{
    tagcache_init();
    while (!tagcache_is_initialized())
    {
        int ret = tagcache_get_commit_step();
        if (ret > 0)
            osm_boot_message(osm_str(S_BOOT_DATABASE), ret,
                             tagcache_get_max_commit_step());
        sleep(HZ/4);
    }
}
#endif

#if (CONFIG_PLATFORM & PLATFORM_HOSTED)
static void init(void)
{
    system_init();
    core_allocator_init();
    kernel_init();
    enable_irq();
    lcd_init();
    global_status.font_id[SCREEN_MAIN] = FONT_SYSFIXED;
    font_init();
    osm_boot_splash();
    button_init();
    powermgmt_init();
    backlight_init();
    unicode_init();
#ifdef HAVE_MULTIVOLUME
    init_volume_names();
#endif
#ifdef SIMULATOR
    sim_tasks_init();
#endif
    lang_init(core_language_builtin, language_strings,
              LANG_LAST_INDEX_IN_ARRAY);
    storage_init();
    pcm_init();
    dsp_init();
    settings_reset();
    settings_load();
    settings_apply(true);
    init_battery_tables();
#ifdef HAVE_DIRCACHE
    init_dircache(true);
    init_dircache(false);
#endif
#ifdef HAVE_TAGCACHE
    init_tagcache();
#endif
    playlist_init();
    audio_init();
#ifndef USB_NONE
    usb_init();
    usb_start_monitoring();
#endif
}

#else /* native */
#include "errno.h"

static void INIT_ATTR init(void)
{
    int rc;
    bool mounted = false;

    system_init();
    core_allocator_init();
    kernel_init();
#if defined(HAVE_BOOTDATA) && !defined(BOOTLOADER)
    verify_boot_data();
#endif

    /* early early early! */
    filesystem_init();

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    set_cpu_frequency(CPUFREQ_NORMAL);
    cpu_boost(true);
#endif

    i2c_init();
    power_init();
    enable_irq();

    unicode_init();
    lcd_init();
#ifdef OSM_BOOT_TRACE
    /* the earliest sign of life: a red screen, the backlight forced on */
    backlight_hw_init();
    backlight_hw_on();
    lcd_set_foreground(LCD_RGBPACK(255, 0, 0));
    lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();
    sleep(HZ);
#endif
    global_status.font_id[SCREEN_MAIN] = FONT_SYSFIXED;
    font_init();
    settings_reset();
    OSM_TRACE("lcd+font");

    CHART(">osm_boot_splash");
    osm_boot_splash();
    CHART("<osm_boot_splash");
    OSM_TRACE("splash");

    lang_init(core_language_builtin, language_strings,
              LANG_LAST_INDEX_IN_ARRAY);
    OSM_TRACE("lang");

#if CONFIG_RTC
    rtc_init();
#endif
    adc_init();
    usb_init();
    OSM_TRACE("usb_init");
    backlight_init();
    OSM_TRACE("backlight_init");
    button_init();
    OSM_TRACE("button_init");
    powermgmt_init();
    OSM_TRACE("powermgmt_init");

    rc = storage_init();
    if (rc)
        osm_fatal("Storage error", rc);
    OSM_TRACE("storage_init");

#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
    /* the card may still be settling */
    for (int tries = 0; tries < 5 && !mounted; tries++)
    {
#endif
        rc = disk_mount_all();
        if (rc > 0)
            mounted = true;
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        else
            sleep(HZ/2);
    }
#endif

    if (!mounted)
    {
        /* no usable partition: offer USB so the card can be fixed */
#ifndef USB_NONE
        osm_boot_message(osm_str(S_BOOT_NO_PARTITION), -1, -1);
        usb_start_monitoring();
        while (button_get(true) != SYS_USB_CONNECTED) {}
        osm_usb_screen();
#endif
        system_reboot();
    }

    OSM_TRACE("mounted");
    pcm_init();
    OSM_TRACE("pcm_init");
    dsp_init();

    CHART(">settings_load");
    settings_load();
    CHART("<settings_load");
    OSM_TRACE("settings_load");

    init_battery_tables();
    OSM_TRACE("battery tables");

#ifdef HAVE_DIRCACHE
    rc = init_dircache(true);
#ifdef HAVE_TAGCACHE
    if (rc < 0)
        tagcache_remove_statefile();
#endif
#endif

    OSM_TRACE("dircache pre");
    settings_apply(true);
    OSM_TRACE("settings_apply");

#ifdef HAVE_DIRCACHE
    init_dircache(false);
#endif
    OSM_TRACE("dircache");
#ifdef HAVE_TAGCACHE
    init_tagcache();
#endif
    OSM_TRACE("tagcache");

    playlist_init();
    OSM_TRACE("playlist_init");

    CHART(">audio_init");
    audio_init();
    CHART("<audio_init");
    OSM_TRACE("audio_init");

    cpu_boost(false);

#if CONFIG_CHARGING
    car_adapter_mode_init();
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
    check_bootfile(false); /* remember write time and filesize */
#endif
    OSM_TRACE("init done");
}
#endif /* CONFIG_PLATFORM & PLATFORM_HOSTED */
