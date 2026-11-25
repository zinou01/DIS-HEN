/* rlogin.c - QR login driver: owns the socket, the state file and the token
 * save. The protocol itself is in rauth.c (host-tested). See rlogin.h. */
#include "rlogin.h"
#include "rauth.h"
#include "daemon.h"
#include "log.h"
#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DISHEN_SDK_PAYLOAD
#include "b64.h"
#include "clock.h"
#include "tls.h"
#include "updater_http.h"
#include "ws.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define RA_HOST        "remote-auth-gateway.discord.gg"
#define RA_ORIGIN      "https://discord.com"
#define LOGIN_HOST     "discord.com"
#define LOGIN_PATH     "/api/v9/users/@me/remote-auth/login"
#define SESSION_MAX_S  180   /* hard cap, whatever the server's hello says */

static unsigned s_tick;

static int file_exists(const char *p){
    struct stat st;
    return stat(p, &st) == 0;
}

/* Atomic: write a temp file, then rename over the state file, so the Setup
 * app never reads a half-written document. */
static void write_state(const char *state, const char *url, const char *user,
                        const char *err){
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", RL_STATE_PATH);
    FILE *f = fopen(tmp, "wb");
    if(!f) return;
    fprintf(f, "{\"v\":1,\"tick\":%u,\"state\":\"%s\",\"url\":\"%s\","
               "\"user\":\"%s\",\"err\":\"%s\"}\n",
            ++s_tick, state, url ? url : "", user ? user : "", err ? err : "");
    fclose(f);
    rename(tmp, RL_STATE_PATH);
}

/* Entropy straight from the OS, like tls.c: no weak fallback. */
static int urandom_poll(void *data, unsigned char *out, size_t len, size_t *olen){
    (void)data;
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) return -1;
    size_t got = 0;
    while(got < len){
        long r = read(fd, (char *)out + got, len - got);
        if(r <= 0){ close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    *olen = len;
    return 0;
}

static int rl_rng(void *ctx, unsigned char *out, size_t len){
    return mbedtls_ctr_drbg_random((mbedtls_ctr_drbg_context *)ctx, out, len);
}

/* One HTTPS POST (JSON). Returns the HTTP status (0 on transport failure) and
 * the body in `out`. Same bounded pattern as art.c. */
static int https_post_json(const char *host, const char *path, const char *body,
                           char *out, size_t cap){
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if(getaddrinfo(host, "443", &hints, &res) != 0 || !res) return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){ freeaddrinfo(res); return 0; }
    struct timeval tv = { 12, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if(connect(fd, res->ai_addr, res->ai_addrlen) < 0){
        close(fd); freeaddrinfo(res); return 0;
    }
    freeaddrinfo(res);
    int fl = fcntl(fd, F_GETFL, 0);
    if(fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    tls_ctx_t *t = tls_start(fd, host);
    if(!t){ close(fd); return 0; }
    char req[1024];
    int bl = (int)strlen(body);
    int rl = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Origin: https://discord.com\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) discord/1.0.9175 Chrome/122.0.6261.112 Electron/30.0.8 "
        "Safari/537.36\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", path, host, bl);
    if(rl <= 0 || rl >= (int)sizeof req ||
       tls_write(t, req, (size_t)rl) < 0 || tls_write(t, body, (size_t)bl) < 0){
        tls_free(t); close(fd); return 0;
    }
    static char raw[16 * 1024];
    size_t n = 0;
    int64_t dl = orbis_mono_s() + 20;
    for(;;){
        char tmp[1024];
        int r = tls_read(t, tmp, sizeof tmp);
        if(r < 0) break;
        if(r == 0){ if(orbis_mono_s() > dl) break; usleep(20000); continue; }
        if(n + (size_t)r >= sizeof raw - 1) break;
        memcpy(raw + n, tmp, (size_t)r);
        n += (size_t)r;
        if(orbis_mono_s() > dl) break;
    }
    tls_free(t);
    close(fd);
    if(n == 0) return 0;
    raw[n] = 0;
    int st = 0;
    size_t olen = 0;
    char *b = upd_parse_response(raw, n, cap - 1, &st, &olen, NULL, 0);
    if(!b) return st;
    memcpy(out, b, olen);
    out[olen] = 0;
    free(b);
    memset(raw, 0, n);          /* the response held an encrypted token */
    return st;
}

static int save_token(const char *token){
    cfg_t c;
    cfg_defaults(&c);
    int lk = cfg_lock(CFG_PATH);
    if(lk < 0) log_msg("rlogin: WARN config lock unavailable; saving unlocked");
    if(cfg_load(CFG_PATH, &c) != 0) cfg_defaults(&c);
    snprintf(c.token, sizeof c.token, "%s", token);
    c.qos_token[0] = 0;        /* belongs to the previous account */
    int rc = cfg_save(CFG_PATH, &c);
    if(lk >= 0) cfg_unlock(lk);
    memset(&c, 0, sizeof c);
    return rc;
}

/* One full login attempt. Returns 1 when a token was saved. */
static int run_session(void){
    static ws_t ws;
    static char buf[4096], reply[2048], url[160];
    rauth_t ra;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    int saved = 0, connected = 0;

    rauth_init(&ra);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_add_source(&ent, urandom_poll, NULL, 64,
                               MBEDTLS_ENTROPY_SOURCE_STRONG);
    remove(RL_CANCEL_PATH);

    write_state("keygen", "", "", "");
    log_msg("rlogin: generating login key");
    if(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                             (const unsigned char *)"Dis-HEN-ra", 10) != 0 ||
       rauth_keygen(&ra, rl_rng, &drbg, RAUTH_KEY_BITS) != 0){
        log_msg("rlogin: key generation failed");
        write_state("error", "", "", "key");
        goto done;
    }
    if(daemon_stop_requested()) goto done;

    write_state("connecting", "", "", "");
    unsigned char kb[16];
    char key[40];
    if(rl_rng(&drbg, kb, sizeof kb) != 0){ write_state("error", "", "", "key"); goto done; }
    b64_encode(kb, sizeof kb, key);
    ws_set_origin(RA_ORIGIN);
    if(ws_connect(&ws, RA_HOST, 443, "/?v=2", key) != 0){
        log_msg("rlogin: gateway connect failed");
        write_state("error", "", "", "net");
        goto done;
    }
    connected = 1;
    log_msg("rlogin: connected to remote-auth gateway");

    int64_t start = orbis_mono_s();
    int64_t last_hb = start;
    int64_t deadline = start + SESSION_MAX_S;
    int hb_s = 41;
    for(;;){
        int64_t now = orbis_mono_s();
        if(daemon_stop_requested()) break;
        if(file_exists(RL_CANCEL_PATH)){
            remove(RL_CANCEL_PATH);
            log_msg("rlogin: cancelled from Setup");
            write_state("cancelled", "", "", "");
            break;
        }
        if(now > deadline){
            log_msg("rlogin: code expired");
            write_state("expired", "", "", "");
            break;
        }
        if(now - last_hb >= hb_s){
            ws_send_text(&ws, rauth_heartbeat_msg(), strlen(rauth_heartbeat_msg()));
            last_hb = now;
        }
        int op = 0, fin = 0;
        int nr = ws_recv_frame(&ws, buf, sizeof buf - 1, &op, &fin);
        if(nr == -3) continue;
        if(nr < 0){
            log_msg("rlogin: gateway closed");
            write_state("error", "", "", "net");
            break;
        }
        if(nr == 0){ usleep(50000); continue; }
        if(op == 9){ ws_pong(&ws, (const unsigned char *)buf, (size_t)nr); continue; }
        if(op == 8){
            log_msg("rlogin: gateway sent close");
            write_state("error", "", "", "net");
            break;
        }
        if(op != 1) continue;
        buf[nr] = 0;
        int ev = rauth_on_message(&ra, rl_rng, &drbg, buf, (size_t)nr,
                                  reply, sizeof reply);
        if(ra.heartbeat_ms >= 5000) hb_s = ra.heartbeat_ms / 1000 - 1;
        if(ra.timeout_ms >= 10000 && start + ra.timeout_ms / 1000 + 5 < deadline)
            deadline = start + ra.timeout_ms / 1000 + 5;
        if(ev == RA_EV_SEND){
            if(ws_send_text(&ws, reply, strlen(reply)) < 0){
                write_state("error", "", "", "net");
                break;
            }
        } else if(ev == RA_EV_QR){
            if(rauth_qr_url(&ra, url, sizeof url) == 0){
                log_msg("rlogin: QR ready");
                write_state("qr", url, "", "");
            }
        } else if(ev == RA_EV_SCANNED){
            log_msg("rlogin: code scanned");
            write_state("scanned", url, ra.user, "");
        } else if(ev == RA_EV_CANCEL){
            log_msg("rlogin: cancelled on phone");
            write_state("cancelled", "", "", "");
            break;
        } else if(ev == RA_EV_ERROR){
            log_msg("rlogin: unusable gateway message; stopping");
            write_state("error", "", "", "proto");
            break;
        } else if(ev == RA_EV_TICKET){
            write_state("finishing", "", ra.user, "");
            static char body[RAUTH_TICKET_MAX + 32], resp[8192];
            char enc[1024], why[64], token[512];
            int ok = 0;
            if(rauth_login_body(&ra, body, sizeof body) == 0){
                int st = https_post_json(LOGIN_HOST, LOGIN_PATH, body, resp, sizeof resp);
                log_msg("rlogin: ticket exchange status %d", st);
                if(rauth_parse_login_response(resp, strlen(resp), enc, sizeof enc,
                                              why, sizeof why) == 0 &&
                   rauth_decrypt_token(&ra, rl_rng, &drbg, enc, token, sizeof token) == 0){
                    if(save_token(token) == 0) ok = 1;
                    else log_msg("rlogin: could not write config.json");
                } else if(!strcmp(why, "captcha")){
                    log_msg("rlogin: Discord asked for a captcha");
                    write_state("error", "", "", "captcha");
                    memset(resp, 0, sizeof resp);
                    break;
                }
            }
            memset(token, 0, sizeof token);
            memset(enc, 0, sizeof enc);
            memset(resp, 0, sizeof resp);
            if(ok){
                saved = 1;
                log_msg("rlogin: login saved");
                write_state("done", "", ra.user, "");
                notify_show("Discord login saved - Dis-HEN is starting");
            } else {
                write_state("error", "", "", "login");
            }
            break;
        }
    }
done:
    if(connected) ws_close(&ws);
    rauth_free(&ra);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return saved;
}

int rlogin_service(int seconds){
    static int announced;
    if(!announced){ announced = 1; remove(RL_REQUEST_PATH); remove(RL_CANCEL_PATH); }
    for(int i = 0; i < seconds * 5; i++){
        if(daemon_stop_requested()) return 0;
        if(file_exists(RL_REQUEST_PATH)){
            remove(RL_REQUEST_PATH);
            if(run_session()) return 1;
            continue;
        }
        if(i % 10 == 0) write_state("idle", "", "", "");   /* every 2 s */
        usleep(200000);
    }
    return 0;
}

#else /* host build: no transport; keeps the header honest for tests */

int rlogin_service(int seconds){ (void)seconds; return 0; }

#endif
