/* notify.c - see notify.h. */
#include "notify.h"
#include "log.h"
#include "clock.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef DISHEN_SDK_PAYLOAD
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#pragma pack(push, 1)
/* Layout verified against a working payload: the size is not derivable from
 * the fields, so it is asserted rather than assumed. */
typedef struct notify_request {
    char reserved[16];
    int32_t target_id;
    char reserved_after_target[24];
    uint8_t use_icon_image_uri;
    char message[1024];
    char icon_uri[2048];
    char padding[3];
} notify_request_t;
#pragma pack(pop)

_Static_assert(sizeof(notify_request_t) == 0xc30,
               "notify_request layout changed");

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

/* /user/data/Dis-HEN, not /data/Dis-HEN -- same mount, but this is the path
 * the notification service is expected to resolve. */
#define NOTIFY_ICON_PATH "(none)"

/* Once per boot, keyed by a short tag so callers can name what fired. */
#define ONCE_MAX 8
static char s_once_keys[ONCE_MAX][24];
static int s_once_n = 0;

/* Rate limiters, same keying. */
#define THROTTLE_MAX 8
static char s_throttle_keys[THROTTLE_MAX][24];
static int64_t s_throttle_at[THROTTLE_MAX];
static int s_throttle_n = 0;

/* fork: no logo. Notifications post with the system icon; nothing is written to disk. */
static int ensure_icon(void){ return 0; }

void notify_show(const char *message){
    if(!message || !message[0]) return;
    notify_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s", message);
    int have_icon = ensure_icon();
    if(have_icon){
        /* target_id must be -1 for a custom icon to be honoured. Left at 0
         * it names the system app and the notification renders with the
         * default icon regardless of use_icon_image_uri. */
        req.target_id = -1;
        req.use_icon_image_uri = 1;
        snprintf(req.icon_uri, sizeof req.icon_uri, "%s", NOTIFY_ICON_PATH);
    }
    int rc = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    /* A rejected notification is otherwise invisible: the message still pops
     * with the default icon and nothing says why. Log both the return code
     * and whether an icon was attached. */
    if(rc != 0){
        log_msg("notify: send failed rc=0x%08x msg=\"%s\" icon=%s",
                (unsigned)rc, message, have_icon ? NOTIFY_ICON_PATH : "none");
    } else if(!have_icon){
        log_msg("notify: sent without icon: \"%s\"", message);
    }
}

/* A tiny key->seen map. Keys are compile-time literals at every call site, so
 * linear search over 8 slots is free and avoids allocating. */
static int once_seen(const char *key){
    for(int i = 0; i < s_once_n; i++)
        if(!strcmp(s_once_keys[i], key)) return 1;
    if(s_once_n < ONCE_MAX){
        snprintf(s_once_keys[s_once_n], sizeof s_once_keys[0], "%s", key);
        s_once_n++;
    }
    return 0;
}

void notify_once_boot(const char *key, const char *message){
    if(!key || once_seen(key)) return;
    notify_show(message);
}

void notify_throttled(const char *key, const char *message, int interval_s){
    if(!key || !message) return;
    if(interval_s < 1) interval_s = 1;
    int64_t now = orbis_mono_s();
    for(int i = 0; i < s_throttle_n; i++){
        if(strcmp(s_throttle_keys[i], key)) continue;
        if(now - s_throttle_at[i] < interval_s) return;
        s_throttle_at[i] = now;
        notify_show(message);
        return;
    }
    if(s_throttle_n < THROTTLE_MAX){
        snprintf(s_throttle_keys[s_throttle_n], sizeof s_throttle_keys[0],
                 "%s", key);
        s_throttle_at[s_throttle_n] = now;
        s_throttle_n++;
    }
    notify_show(message);
}

#else /* app build: no notification API wired here */

void notify_show(const char *message){ (void)message; }
void notify_once_boot(const char *key, const char *message){ (void)key; (void)message; }
void notify_throttled(const char *key, const char *message, int interval_s){
    (void)key; (void)message; (void)interval_s;
}

#endif