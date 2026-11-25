/* cloudinary.c - see cloudinary.h. Fork addition.
 * Transport follows art.c (same socket/TLS setup, same bounded response
 * read, same chunked-aware parser) so it behaves identically on-console. */
#include "cloudinary.h"
#include "jsonlite.h"
#include "log.h"
#include "updater_http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define CLD_HOST "api.cloudinary.com"
#define CLD_URL_PREFIX "https://res.cloudinary.com/"

int cld_url_ok(const char *url){
    return url && !strncmp(url, CLD_URL_PREFIX, strlen(CLD_URL_PREFIX));
}

static int safe_token(const char *s, int allow_slash){
    if(!s) return 0;
    for(; *s; s++){
        char c = *s;
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-') continue;
        if(allow_slash && c == '/') continue;
        return 0;
    }
    return 1;
}

int cld_public_id(const char *folder, const char *tid, const char *kind,
                  char *out, size_t cap){
    if(!out || !cap || !tid || !tid[0] || !kind || !kind[0]) return -1;
    if(!safe_token(tid, 0) || !safe_token(kind, 0)) return -1;
    if(folder && folder[0]){
        if(!safe_token(folder, 1) || strstr(folder, "..")) return -1;
        size_t fl = strlen(folder);
        while(fl && folder[fl-1] == '/') fl--;      /* tolerate trailing '/' */
        int n = snprintf(out, cap, "%.*s/%s_%s", (int)fl, folder, tid, kind);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    int n = snprintf(out, cap, "%s_%s", tid, kind);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int cld_multipart_head(char *out, size_t cap, const char *boundary,
                       const char *preset, const char *public_id){
    if(!out || !boundary || !preset || !public_id) return -1;
    if(!safe_token(preset, 0) || !safe_token(public_id, 1)) return -1;
    int n = snprintf(out, cap,
        "--%s\r\nContent-Disposition: form-data; name=\"upload_preset\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"public_id\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"icon0.png\"\r\n"
        "Content-Type: image/png\r\n\r\n",
        boundary, preset, boundary, public_id, boundary);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int cld_multipart_tail(char *out, size_t cap, const char *boundary){
    if(!out || !boundary) return -1;
    int n = snprintf(out, cap, "\r\n--%s--\r\n", boundary);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int cld_parse_url(const char *body, size_t len, char *out, size_t cap){
    if(!body || !out || cap < 16) return -1;
    jl_val_t *r = jl_parse(body, len);
    if(!r) return -1;
    int rc = -1;
    if(r->type == JL_OBJECT){
        const jl_val_t *u = jl_obj_get(r, "secure_url");
        if(u && u->type == JL_STRING && u->str && cld_url_ok(u->str) &&
           strlen(u->str) < cap && !strchr(u->str, '"')){
            snprintf(out, cap, "%s", u->str);
            rc = 0;
        }
    }
    jl_free(r);
    return rc;
}

#ifdef DISHEN_SDK_PAYLOAD
#include "tls.h"
#include "clock.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#endif

#define CLD_DEADLINE_S 45
#define CLD_RESP_MAX (8u*1024u)
#define CLD_CHUNK 4096

int cld_upload_png(const char *cloud, const char *preset, const char *public_id,
                   const char *path, char *out_url, size_t cap){
#ifdef DISHEN_SDK_PAYLOAD
    if(!cloud || !safe_token(cloud, 0) || !preset || !public_id || !path ||
       !out_url || cap < 16) return -1;

    int ffd = open(path, O_RDONLY);
    if(ffd < 0) return -1;
    off_t fsz = lseek(ffd, 0, SEEK_END);
    lseek(ffd, 0, SEEK_SET);
    if(fsz <= 0 || (size_t)fsz > CLD_MAX_FILE){
        log_msg("cloudinary: icon size %lld unusable", (long long)fsz);
        close(ffd); return -1;
    }

    static const char *boundary = "----dishen7d3f91c2";
    char head[512], tail[64];
    int hl = cld_multipart_head(head, sizeof head, boundary, preset, public_id);
    int tl = cld_multipart_tail(tail, sizeof tail, boundary);
    if(hl < 0 || tl < 0){ close(ffd); return -1; }
    size_t total = (size_t)hl + (size_t)fsz + (size_t)tl;

    char req[512];
    int rl = snprintf(req, sizeof req,
        "POST /v1_1/%s/image/upload HTTP/1.1\r\nHost: %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        cloud, CLD_HOST, boundary, total);
    if(rl <= 0 || (size_t)rl >= sizeof req){ close(ffd); return -1; }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if(getaddrinfo(CLD_HOST, "443", &hints, &res) != 0 || !res){ close(ffd); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){ freeaddrinfo(res); close(ffd); return -1; }
    struct timeval tv = { 20, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if(connect(fd, res->ai_addr, res->ai_addrlen) < 0){
        close(fd); freeaddrinfo(res); close(ffd); return -1;
    }
    freeaddrinfo(res);
    { int fl = fcntl(fd, F_GETFL, 0); if(fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }

    tls_ctx_t *t = tls_start(fd, CLD_HOST);
    if(!t){ close(fd); close(ffd); log_msg("cloudinary: tls handshake failed"); return -1; }

    int ok = (tls_write(t, req, (size_t)rl) >= 0 && tls_write(t, head, (size_t)hl) >= 0);
    unsigned char chunk[CLD_CHUNK];
    size_t left = (size_t)fsz;
    while(ok && left){
        ssize_t r = read(ffd, chunk, left < sizeof chunk ? left : sizeof chunk);
        if(r <= 0){ ok = 0; break; }
        if(tls_write(t, chunk, (size_t)r) < 0){ ok = 0; break; }
        left -= (size_t)r;
    }
    close(ffd);
    if(ok) ok = (tls_write(t, tail, (size_t)tl) >= 0);
    if(!ok){ tls_free(t); close(fd); log_msg("cloudinary: upload write failed"); return -1; }

    static char resp[CLD_RESP_MAX];
    size_t bl = 0;
    int64_t dl = orbis_mono_s() + CLD_DEADLINE_S;
    for(;;){
        char tmp[1024];
        int r = tls_read(t, tmp, sizeof tmp);
        if(r < 0) break;
        if(r == 0){
            if(orbis_mono_s() > dl) break;
            usleep(20000);
            continue;
        }
        if(bl + (size_t)r >= sizeof resp - 1) break;
        memcpy(resp + bl, tmp, (size_t)r);
        bl += (size_t)r;
    }
    tls_free(t);
    close(fd);
    if(!bl){ log_msg("cloudinary: empty response"); return -1; }
    resp[bl] = 0;

    int st = 0; size_t olen = 0;
    char *body = upd_parse_response(resp, bl, sizeof resp, &st, &olen, NULL, 0);
    if(!body){ log_msg("cloudinary: unparsable response"); return -1; }
    int rc = -1;
    if(st == 200) rc = cld_parse_url(body, olen, out_url, cap);
    else log_msg("cloudinary: HTTP %d (%.100s)", st, body);
    free(body);
    return rc;
#else
    /* Host test build: no transport; helpers above are what gets tested. */
    (void)cloud; (void)preset; (void)public_id; (void)path; (void)out_url; (void)cap;
    return -1;
#endif
}
