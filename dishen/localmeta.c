/* localmeta.c - see localmeta.h. Fork addition. */
#include "localmeta.h"
#include "sfo.h"
#include "cfg.h"
#include "cloudinary.h"
#include "gamecache.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define SFO_MAX_BYTES (32u * 1024u)

int localmeta_tid_valid(const char *tid){
    if(!tid || strlen(tid) != 9) return 0;
    for(const char *p = tid; *p; p++)
        if(!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return 0;
    return 1;
}

int localmeta_join(const char *base, const char *tid, const char *file,
                   char *out, size_t cap){
    if(!base || !file || !out || !cap || !localmeta_tid_valid(tid)) return -1;
    int n = snprintf(out, cap, "%s/%s/%s", base, tid, file);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int read_file(const char *path, unsigned char *buf, size_t cap, size_t *out_n){
    int fd = open(path, O_RDONLY);
    if(fd < 0) return -1;
    size_t got = 0;
    while(got < cap){
        ssize_t r = read(fd, buf + got, cap - got);
        if(r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if(got == 0) return -1;
    *out_n = got;
    return 0;
}

static int sfo_from(const char *base, const char *tid, char *out, size_t cap){
    char path[256];
    if(localmeta_join(base, tid, "param.sfo", path, sizeof path) != 0) return -1;
    unsigned char *buf = (unsigned char *)malloc(SFO_MAX_BYTES);
    if(!buf) return -1;
    size_t n = 0;
    int rc = -1;
    if(read_file(path, buf, SFO_MAX_BYTES, &n) == 0 && sfo_title(buf, n, out, cap) == 0)
        rc = 0;
    free(buf);
    return rc;
}

/* Apps on external / extended storage keep their metadata somewhere other
 * than the internal appmeta folders, and the exact place differs by firmware
 * and by how the app was installed. Rather than guess one, try each known
 * spot after the configured folders. A missing folder just fails access(). */
static const char *const kSfoFallbacks[] = {
    "/system_data/priv/appmeta/external/%s/param.sfo",
    "/user/appmeta/external/%s/param.sfo",
    "/user/appmeta/ext/%s/param.sfo",
    "/system_data/priv/appmeta/ext/%s/param.sfo",
    "/mnt/ext0/user/appmeta/%s/param.sfo",
    "/mnt/ext1/user/appmeta/%s/param.sfo",
    "/user/app/%s/sce_sys/param.sfo",
    "/mnt/ext0/user/app/%s/sce_sys/param.sfo",
    "/mnt/ext1/user/app/%s/sce_sys/param.sfo",
    NULL
};
static const char *const kIconFallbacks[] = {
    "/system_data/priv/appmeta/external/%s/icon0.png",
    "/user/appmeta/external/%s/icon0.png",
    "/user/appmeta/ext/%s/icon0.png",
    "/system_data/priv/appmeta/ext/%s/icon0.png",
    "/mnt/ext0/user/appmeta/%s/icon0.png",
    "/mnt/ext1/user/appmeta/%s/icon0.png",
    "/user/app/%s/sce_sys/icon0.png",
    "/mnt/ext0/user/app/%s/sce_sys/icon0.png",
    "/mnt/ext1/user/app/%s/sce_sys/icon0.png",
    NULL
};

static int fallback_path(const char *const *list, const char *tid,
                         char *out, size_t cap){
    if(!localmeta_tid_valid(tid)) return -1;
    for(int i = 0; list[i]; i++){
        int n = snprintf(out, cap, list[i], tid);
        if(n > 0 && (size_t)n < cap && access(out, R_OK) == 0) return 0;
    }
    out[0] = 0;
    return -1;
}

static int sfo_at(const char *path, char *out, size_t cap){
    unsigned char *buf = (unsigned char *)malloc(SFO_MAX_BYTES);
    if(!buf) return -1;
    size_t n = 0;
    int rc = -1;
    if(read_file(path, buf, SFO_MAX_BYTES, &n) == 0 && sfo_title(buf, n, out, cap) == 0)
        rc = 0;
    free(buf);
    return rc;
}

int localmeta_title(const char *sfo_dir, const char *img_dir,
                    const char *tid, char *out, size_t cap){
    if(!out || !cap) return -1;
    out[0] = 0;
    if(!sfo_dir || !sfo_dir[0]) sfo_dir = LM_DEFAULT_SFO_DIR;
    if(!img_dir || !img_dir[0]) img_dir = LM_DEFAULT_IMG_DIR;
    /* Primary location first, then the other one ("or else"). */
    if(sfo_from(sfo_dir, tid, out, cap) == 0) return 0;
    if(sfo_from(img_dir, tid, out, cap) == 0) return 0;
    char fp[256];
    if(fallback_path(kSfoFallbacks, tid, fp, sizeof fp) == 0 &&
       sfo_at(fp, out, cap) == 0){
        log_msg("name: %s found in %s", tid, fp);
        return 0;
    }
    out[0] = 0;
    return -1;
}

static int icon_in(const char *base, const char *tid, char *out, size_t cap){
    static const char *names[] = { "icon0.png", "ICON0.PNG", NULL };
    for(int i = 0; names[i]; i++){
        if(localmeta_join(base, tid, names[i], out, cap) != 0) return -1;
        if(access(out, R_OK) == 0) return 0;
    }
    return -1;
}

int localmeta_icon_path(const char *sfo_dir, const char *img_dir,
                        const char *tid, char *out, size_t cap){
    if(!out || !cap) return -1;
    out[0] = 0;
    if(!sfo_dir || !sfo_dir[0]) sfo_dir = LM_DEFAULT_SFO_DIR;
    if(!img_dir || !img_dir[0]) img_dir = LM_DEFAULT_IMG_DIR;
    if(icon_in(img_dir, tid, out, cap) == 0) return 0;
    if(icon_in(sfo_dir, tid, out, cap) == 0) return 0;
    if(fallback_path(kIconFallbacks, tid, out, cap) == 0){
        log_msg("art: %s icon found in %s", tid, out);
        return 0;
    }
    out[0] = 0;
    return -1;
}

/* ---- memoised resolver ---- */
static char s_tid[16];
static int  s_mask;
static char s_name[128];
static char s_art[256];

int localmeta_resolve(const char *tid, char *name_out, size_t name_cap,
                      char *art_out, size_t art_cap){
    if(!tid || !localmeta_tid_valid(tid)) return 0;
    if(strcmp(tid, s_tid) != 0){
        /* New title: do the work once, remember the outcome (even a miss). */
        snprintf(s_tid, sizeof s_tid, "%s", tid);
        s_mask = 0; s_name[0] = 0; s_art[0] = 0;

        char name[128] = "";
        if(localmeta_title(g_cfg.local_sfo_dir, g_cfg.local_img_dir,
                           tid, name, sizeof name) == 0){
            snprintf(s_name, sizeof s_name, "%s", name);
            s_mask |= LM_NAME;
            log_msg("name: %s via local appmeta", s_name);
        }

        /* Art: a Cloudinary URL already stored for this title wins (no
         * network, survives reboots); otherwise upload icon0.png once. */
        char cname[128] = "", cart[256] = "";
        if(gamecache_get(tid, cname, sizeof cname, cart, sizeof cart) &&
           cld_url_ok(cart)){
            snprintf(s_art, sizeof s_art, "%s", cart);
            s_mask |= LM_ART;
        } else if(g_cfg.cloud_name[0] && g_cfg.cloud_preset[0]){
            char path[256], pid[160], url[256];
            if(localmeta_icon_path(g_cfg.local_sfo_dir, g_cfg.local_img_dir,
                                   tid, path, sizeof path) == 0 &&
               cld_public_id(g_cfg.cloud_folder, tid, "icon0", pid, sizeof pid) == 0 &&
               cld_upload_png(g_cfg.cloud_name, g_cfg.cloud_preset, pid, path,
                              url, sizeof url) == 0){
                snprintf(s_art, sizeof s_art, "%s", url);
                s_mask |= LM_ART;
                log_msg("art: local icon0 -> cloudinary ok");
            }
        }
        /* Persist name+art together so the next boot needs neither the SFO
         * read nor the network. gamecache needs a real name. */
        if((s_mask & LM_ART) && s_name[0])
            gamecache_put(tid, s_name, s_art, "local");
    }
    if(s_mask & LM_NAME && name_out && name_cap)
        snprintf(name_out, name_cap, "%s", s_name);
    if(s_mask & LM_ART && art_out && art_cap)
        snprintf(art_out, art_cap, "%s", s_art);
    return s_mask;
}
