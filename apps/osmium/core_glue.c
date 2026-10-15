#include "osmium.h"
#include <stdarg.h>
#include "splash.h"
#include "system.h"
#include "kernel.h"
void osm_boot_splash(void){}
void osm_boot_message(const char *msg, int step, int steps){(void)msg;(void)step;(void)steps;}
void osm_fatal(const char *msg, int code){(void)msg;(void)code;}
void osm_ui_run(void){ while(1) sleep(HZ); }
void osm_usb_screen(void){}
bool osm_user_abort(void){return false;}
bool osm_confirm(const char *q){(void)q;return true;}
bool osm_confirm_reboot_new_firmware(void){return false;}
void osm_library_changed(void){}
void osm_shutdown_screen(bool r, bool b){(void)r;(void)b;}
void splash(int t, const char *s){(void)t;(void)s;}
void splashf(int t, const char *f, ...){(void)t;(void)f;}
void splash_progress(int c, int t, const char *f, ...){(void)c;(void)t;(void)f;}
void splash_progress_set_delay(long d){(void)d;}
const char *osm_str(enum osm_str_id id){(void)id;return "";}
#include "lcd.h"
void viewport_set_buffer(struct viewport *vp, struct frame_buffer_t *buffer, const enum screen_type screen){(void)screen; if(!vp) vp=lcd_current_viewport; vp->buffer=(buffer&&buffer->elems==0)?NULL:buffer; lcd_init_viewport(vp);}
bool is_keys_locked(void){return false;}
