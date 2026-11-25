/* rauth.c - Discord remote-auth (QR login) message handling. See rauth.h. */
#include "rauth.h"
#include "jsonlite.h"

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RA_RSA_BYTES (RAUTH_KEY_BITS / 8)

void rauth_init(rauth_t *r){
    if(!r) return;
    memset(r, 0, sizeof *r);
    mbedtls_pk_init(&r->pk);
}

void rauth_free(rauth_t *r){
    if(!r) return;
    mbedtls_pk_free(&r->pk);
    /* ticket / user / fingerprint are not secrets once the login ends, but
     * wipe everything anyway so nothing lingers in a reused struct. */
    memset(r, 0, sizeof *r);
}

/* ---- small helpers ------------------------------------------------------ */

/* Characters allowed in fingerprints / tickets we copy into JSON or URLs.
 * Rejecting anything else keeps a hostile frame from breaking out of the
 * strings we build with snprintf. */
static int safe_token_char(char c){
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '+' || c == '/' || c == '=';
}

static int copy_safe(char *dst, size_t cap, const char *src, size_t len){
    if(!src || len == 0 || len >= cap) return -1;
    for(size_t i = 0; i < len; i++)
        if(!safe_token_char(src[i])) return -1;
    memcpy(dst, src, len);
    dst[len] = 0;
    return 0;
}

static const jl_val_t *get_str(const jl_val_t *obj, const char *key){
    const jl_val_t *v = jl_obj_get(obj, key);
    if(!v || v->type != JL_STRING || !v->str) return NULL;
    return v;
}

static int b64_decode(const char *in, size_t inlen, unsigned char *out,
                      size_t cap, size_t *olen){
    return mbedtls_base64_decode(out, cap, olen,
                                 (const unsigned char *)in, inlen);
}

/* Standard base64 text -> unpadded base64url. */
static void to_b64url(char *s){
    size_t w = 0;
    for(size_t i = 0; s[i]; i++){
        char c = s[i];
        if(c == '=') continue;
        if(c == '+') c = '-';
        else if(c == '/') c = '_';
        s[w++] = c;
    }
    s[w] = 0;
}

static int oaep_decrypt(rauth_t *r, rauth_rng_fn rng, void *rng_ctx,
                        const unsigned char *in, size_t inlen,
                        unsigned char *out, size_t cap, size_t *olen){
    if(!r->have_key || inlen != RA_RSA_BYTES) return -1;
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(r->pk);
    if(mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21,
                               MBEDTLS_MD_SHA256) != 0)
        return -1;
    return mbedtls_rsa_rsaes_oaep_decrypt(rsa, rng, rng_ctx, NULL, 0,
                                          olen, in, out, cap) == 0 ? 0 : -1;
}

/* ---- key handling ------------------------------------------------------- */

int rauth_keygen(rauth_t *r, rauth_rng_fn rng, void *rng_ctx, unsigned bits){
    if(!r || !rng) return -1;
    if(r->have_key){ mbedtls_pk_free(&r->pk); mbedtls_pk_init(&r->pk); r->have_key = 0; }
    if(mbedtls_pk_setup(&r->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
        return -1;
    if(mbedtls_rsa_gen_key(mbedtls_pk_rsa(r->pk), rng, rng_ctx, bits, 65537) != 0){
        mbedtls_pk_free(&r->pk);
        mbedtls_pk_init(&r->pk);
        return -1;
    }
    r->have_key = 1;
    return 0;
}

int rauth_pubkey_b64(rauth_t *r, char *out, size_t cap){
    if(!r || !r->have_key || !out) return -1;
    unsigned char der[512];
    int n = mbedtls_pk_write_pubkey_der(&r->pk, der, sizeof der);
    if(n <= 0) return -1;
    size_t olen = 0;
    if(mbedtls_base64_encode((unsigned char *)out, cap, &olen,
                             der + sizeof der - n, (size_t)n) != 0)
        return -1;
    if(olen >= cap) return -1;
    out[olen] = 0;
    return 0;
}

/* ---- protocol ----------------------------------------------------------- */

int rauth_on_message(rauth_t *r, rauth_rng_fn rng, void *rng_ctx,
                     const char *json, size_t len,
                     char *reply, size_t reply_cap){
    if(!r || !json || !reply || reply_cap == 0) return RA_EV_ERROR;
    reply[0] = 0;
    jl_val_t *root = jl_parse(json, len);
    if(!root || root->type != JL_OBJECT){ jl_free(root); return RA_EV_ERROR; }
    const jl_val_t *op = get_str(root, "op");
    if(!op){ jl_free(root); return RA_EV_ERROR; }
    int ev = RA_EV_NONE;

    if(strcmp(op->str, "hello") == 0){
        const jl_val_t *hb = jl_obj_get(root, "heartbeat_interval");
        const jl_val_t *to = jl_obj_get(root, "timeout_ms");
        r->heartbeat_ms = (hb && hb->type == JL_NUMBER && hb->num >= 1000 &&
                           hb->num <= 600000) ? (int)hb->num : 41250;
        r->timeout_ms   = (to && to->type == JL_NUMBER && to->num >= 1000 &&
                           to->num <= 3600000) ? (int)to->num : 120000;
        char pub[640];
        int n = -1;
        if(rauth_pubkey_b64(r, pub, sizeof pub) == 0)
            n = snprintf(reply, reply_cap,
                         "{\"op\":\"init\",\"encoded_public_key\":\"%s\"}", pub);
        ev = (n > 0 && (size_t)n < reply_cap) ? RA_EV_SEND : RA_EV_ERROR;
    }
    else if(strcmp(op->str, "nonce_proof") == 0){
        const jl_val_t *en = get_str(root, "encrypted_nonce");
        unsigned char enc[RA_RSA_BYTES + 16], nonce[RA_RSA_BYTES];
        size_t elen = 0, nlen = 0;
        ev = RA_EV_ERROR;
        if(en && b64_decode(en->str, en->strlen, enc, sizeof enc, &elen) == 0 &&
           oaep_decrypt(r, rng, rng_ctx, enc, elen, nonce, sizeof nonce, &nlen) == 0){
            unsigned char dig[32];
            char b[64];
            size_t bl = 0;
            if(mbedtls_sha256(nonce, nlen, dig, 0) == 0 &&
               mbedtls_base64_encode((unsigned char *)b, sizeof b, &bl, dig, 32) == 0){
                b[bl] = 0;
                to_b64url(b);
                int n = snprintf(reply, reply_cap,
                    "{\"op\":\"nonce_proof\",\"nonce_proof\":\"%s\"}", b);
                if(n > 0 && (size_t)n < reply_cap) ev = RA_EV_SEND;
            }
            memset(nonce, 0, sizeof nonce);
        }
    }
    else if(strcmp(op->str, "pending_remote_init") == 0){
        const jl_val_t *fp = get_str(root, "fingerprint");
        ev = (fp && copy_safe(r->fingerprint, sizeof r->fingerprint,
                              fp->str, fp->strlen) == 0) ? RA_EV_QR : RA_EV_ERROR;
    }
    else if(strcmp(op->str, "pending_ticket") == 0){
        /* encrypted "id:discriminator:avatar:username". Only the name is
         * shown ("Scanned by X"); a failure to decrypt is not fatal. */
        r->user[0] = 0;
        const jl_val_t *eu = get_str(root, "encrypted_user_payload");
        unsigned char enc[RA_RSA_BYTES + 16], plain[RA_RSA_BYTES];
        size_t elen = 0, plen = 0;
        if(eu && b64_decode(eu->str, eu->strlen, enc, sizeof enc, &elen) == 0 &&
           oaep_decrypt(r, rng, rng_ctx, enc, elen, plain, sizeof plain - 1, &plen) == 0){
            plain[plen] = 0;
            const char *p = (const char *)plain;
            for(int colons = 0; colons < 3 && p; colons++){
                p = strchr(p, ':');
                if(p) p++;
            }
            if(p){
                size_t w = 0;
                for(; *p && w < sizeof r->user - 1; p++){
                    unsigned char c = (unsigned char)*p;
                    /* printable ASCII only; the console font has no emoji */
                    r->user[w++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
                                   ? (char)c : '?';
                }
                r->user[w] = 0;
            }
        }
        memset(plain, 0, sizeof plain);
        ev = RA_EV_SCANNED;
    }
    else if(strcmp(op->str, "pending_login") == 0){
        const jl_val_t *tk = get_str(root, "ticket");
        ev = (tk && copy_safe(r->ticket, sizeof r->ticket,
                              tk->str, tk->strlen) == 0) ? RA_EV_TICKET : RA_EV_ERROR;
    }
    else if(strcmp(op->str, "cancel") == 0)            ev = RA_EV_CANCEL;
    else if(strcmp(op->str, "heartbeat_ack") == 0)     ev = RA_EV_ACK;

    jl_free(root);
    return ev;
}

int rauth_qr_url(const rauth_t *r, char *out, size_t cap){
    if(!r || !r->fingerprint[0] || !out) return -1;
    int n = snprintf(out, cap, "https://discord.com/ra/%s", r->fingerprint);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int rauth_login_body(const rauth_t *r, char *out, size_t cap){
    if(!r || !r->ticket[0] || !out) return -1;
    int n = snprintf(out, cap, "{\"ticket\":\"%s\"}", r->ticket);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

const char *rauth_heartbeat_msg(void){ return "{\"op\":\"heartbeat\"}"; }

int rauth_decrypt_token(rauth_t *r, rauth_rng_fn rng, void *rng_ctx,
                        const char *b64, char *out, size_t cap){
    if(!r || !b64 || !out || cap < 8) return -1;
    unsigned char enc[RA_RSA_BYTES + 16], plain[RA_RSA_BYTES];
    size_t elen = 0, plen = 0;
    if(b64_decode(b64, strlen(b64), enc, sizeof enc, &elen) != 0) return -1;
    if(oaep_decrypt(r, rng, rng_ctx, enc, elen, plain, sizeof plain, &plen) != 0)
        return -1;
    int rc = -1;
    /* A user token is a printable, dotted string. Anything else means we
     * decrypted garbage, and storing it would just break login. */
    if(plen >= 20 && plen < cap){
        rc = 0;
        for(size_t i = 0; i < plen; i++)
            if(plain[i] < 0x21 || plain[i] > 0x7e || plain[i] == '"' ||
               plain[i] == '\\'){ rc = -1; break; }
        if(rc == 0){ memcpy(out, plain, plen); out[plen] = 0; }
    }
    memset(plain, 0, sizeof plain);
    return rc;
}

int rauth_parse_login_response(const char *body, size_t len,
                               char *enc_token, size_t cap,
                               char *why, size_t why_cap){
    if(why && why_cap) why[0] = 0;
    jl_val_t *root = jl_parse(body, len);
    if(!root || root->type != JL_OBJECT){ jl_free(root); return -1; }
    int rc = -1;
    const jl_val_t *et = get_str(root, "encrypted_token");
    if(et && et->strlen < cap){
        memcpy(enc_token, et->str, et->strlen);
        enc_token[et->strlen] = 0;
        rc = 0;
    } else if(why && why_cap){
        /* A captcha demand is the one failure worth naming to the user. */
        if(jl_obj_get(root, "captcha_key") || jl_obj_get(root, "captcha_sitekey"))
            snprintf(why, why_cap, "captcha");
        else {
            const jl_val_t *m = get_str(root, "message");
            if(m) snprintf(why, why_cap, "%.60s", m->str);
        }
    }
    jl_free(root);
    return rc;
}
