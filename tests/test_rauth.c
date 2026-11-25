/* test_rauth.c - drives the QR-login handshake against a simulated server.
 *
 * The "server" half is written independently of rauth.c on purpose: it parses
 * the public key we send, encrypts with RSA-OAEP(SHA-256), and checks our proof
 * the way Discord does, so a wrong padding hash or key encoding fails here
 * rather than on a console. */
#include "rauth.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host mbedTLS link leaves out entropy_poll.c (see Makefile), so supply
 * the OS entropy source it expects. Test-only. */
int mbedtls_platform_entropy_poll(void *data, unsigned char *out, size_t len,
                                  size_t *olen){
    (void)data;
    FILE *f = fopen("/dev/urandom", "rb");
    if(!f) return -1;
    size_t n = fread(out, 1, len, f);
    fclose(f);
    *olen = n;
    return n == len ? 0 : -1;
}

static int g_fail;
#define CHECK(c, name) do { if(!(c)){ printf("FAIL %s\n", name); g_fail++; } } while(0)

static mbedtls_ctr_drbg_context g_drbg;

static int rng(void *ctx, unsigned char *out, size_t len){
    return mbedtls_ctr_drbg_random(ctx, out, len);
}

/* server-side: RSA-OAEP(SHA-256) encrypt to the pubkey we advertised */
static int server_encrypt(const char *pub_b64, const unsigned char *plain,
                          size_t plen, char *out_b64, size_t cap){
    unsigned char der[512];
    size_t dlen = 0;
    if(mbedtls_base64_decode(der, sizeof der, &dlen,
                             (const unsigned char *)pub_b64, strlen(pub_b64)))
        return -1;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if(mbedtls_pk_parse_public_key(&pk, der, dlen)){ mbedtls_pk_free(&pk); return -1; }
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
    unsigned char ct[256];
    int rc = mbedtls_rsa_rsaes_oaep_encrypt(rsa, rng, &g_drbg, NULL, 0, plen,
                                            plain, ct);
    mbedtls_pk_free(&pk);
    if(rc) return -1;
    size_t ol = 0;
    if(mbedtls_base64_encode((unsigned char *)out_b64, cap, &ol, ct, sizeof ct))
        return -1;
    out_b64[ol] = 0;
    return 0;
}

static const char *find_str(const char *json, const char *key, char *out, size_t cap){
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if(!p) return NULL;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if(!e || (size_t)(e - p) >= cap) return NULL;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return out;
}

int main(void){
    mbedtls_entropy_context ent;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    CHECK(mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &ent,
                                (const unsigned char *)"t", 1) == 0, "drbg seed");

    rauth_t r;
    rauth_init(&r);
    CHECK(rauth_keygen(&r, rng, &g_drbg, RAUTH_KEY_BITS) == 0, "keygen");

    char reply[2048], pub[1024], tmp[1024];

    /* 1. hello -> init with our public key */
    const char *hello = "{\"op\":\"hello\",\"heartbeat_interval\":41250,\"timeout_ms\":120000}";
    CHECK(rauth_on_message(&r, rng, &g_drbg, hello, strlen(hello), reply, sizeof reply) == RA_EV_SEND, "hello -> send");
    CHECK(strstr(reply, "\"op\":\"init\"") != NULL, "init op");
    CHECK(find_str(reply, "encoded_public_key", pub, sizeof pub) != NULL, "init has key");
    CHECK(r.heartbeat_ms == 41250 && r.timeout_ms == 120000, "hello timings");
    /* SPKI of an RSA-2048 key is 294 bytes; base64 is 392 chars */
    CHECK(strlen(pub) == 392, "spki size");

    /* 2. nonce_proof: server encrypts a nonce, expects b64url(sha256(nonce)) */
    unsigned char nonce[32];
    mbedtls_ctr_drbg_random(&g_drbg, nonce, sizeof nonce);
    char enc[512];
    CHECK(server_encrypt(pub, nonce, sizeof nonce, enc, sizeof enc) == 0, "server encrypt nonce");
    snprintf(tmp, sizeof tmp, "{\"op\":\"nonce_proof\",\"encrypted_nonce\":\"%s\"}", enc);
    CHECK(rauth_on_message(&r, rng, &g_drbg, tmp, strlen(tmp), reply, sizeof reply) == RA_EV_SEND, "nonce -> send");
    unsigned char dig[32];
    mbedtls_sha256(nonce, sizeof nonce, dig, 0);
    char want[64]; size_t wl = 0;
    mbedtls_base64_encode((unsigned char *)want, sizeof want, &wl, dig, 32);
    want[wl] = 0;
    for(char *c = want; *c; c++){ if(*c == '+') *c = '-'; if(*c == '/') *c = '_'; }
    char *eq = strchr(want, '='); if(eq) *eq = 0;
    char got[128];
    CHECK(find_str(reply, "nonce_proof", got, sizeof got) != NULL, "proof present");
    CHECK(strcmp(got, want) == 0, "proof value");

    /* a nonce encrypted to a different key must not yield a proof */
    CHECK(rauth_on_message(&r, rng, &g_drbg,
        "{\"op\":\"nonce_proof\",\"encrypted_nonce\":\"AAAA\"}", 50, reply, sizeof reply) == RA_EV_ERROR, "bad nonce rejected");

    /* 3. fingerprint -> QR url */
    const char *fp = "{\"op\":\"pending_remote_init\",\"fingerprint\":\"AbC-123_xyz\"}";
    CHECK(rauth_on_message(&r, rng, &g_drbg, fp, strlen(fp), reply, sizeof reply) == RA_EV_QR, "fingerprint -> qr");
    CHECK(rauth_qr_url(&r, tmp, sizeof tmp) == 0 && strcmp(tmp, "https://discord.com/ra/AbC-123_xyz") == 0, "qr url");
    const char *badfp = "{\"op\":\"pending_remote_init\",\"fingerprint\":\"a\\\"b\"}";
    CHECK(rauth_on_message(&r, rng, &g_drbg, badfp, strlen(badfp), reply, sizeof reply) == RA_EV_ERROR, "unsafe fingerprint rejected");

    /* 4. scanned: user payload "id:discrim:avatar:name" */
    const char *up = "1234567890:0:abcdef:Test User";
    CHECK(server_encrypt(pub, (const unsigned char *)up, strlen(up), enc, sizeof enc) == 0, "encrypt user");
    snprintf(tmp, sizeof tmp, "{\"op\":\"pending_ticket\",\"encrypted_user_payload\":\"%s\"}", enc);
    CHECK(rauth_on_message(&r, rng, &g_drbg, tmp, strlen(tmp), reply, sizeof reply) == RA_EV_SCANNED, "scanned");
    CHECK(strcmp(r.user, "Test User") == 0, "scanned user name");

    /* 5. ticket + login body */
    const char *tk = "{\"op\":\"pending_login\",\"ticket\":\"tkt.ABC-def_123=\"}";
    CHECK(rauth_on_message(&r, rng, &g_drbg, tk, strlen(tk), reply, sizeof reply) == RA_EV_TICKET, "ticket");
    CHECK(rauth_login_body(&r, tmp, sizeof tmp) == 0 && strcmp(tmp, "{\"ticket\":\"tkt.ABC-def_123=\"}") == 0, "login body");

    /* 6. login response -> encrypted token -> token */
    const char *token = "MTIzNDU2Nzg5MDEyMzQ1Njc4.GabcDe.abcdefghijklmnopqrstuvwxyz0123456789";
    CHECK(server_encrypt(pub, (const unsigned char *)token, strlen(token), enc, sizeof enc) == 0, "encrypt token");
    char resp[1024], et[600], why[64];
    snprintf(resp, sizeof resp, "{\"encrypted_token\":\"%s\"}", enc);
    CHECK(rauth_parse_login_response(resp, strlen(resp), et, sizeof et, why, sizeof why) == 0, "parse login response");
    char outtok[256];
    CHECK(rauth_decrypt_token(&r, rng, &g_drbg, et, outtok, sizeof outtok) == 0 && strcmp(outtok, token) == 0, "decrypt token");
    /* captcha demand is named */
    const char *cap = "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"x\"}";
    CHECK(rauth_parse_login_response(cap, strlen(cap), et, sizeof et, why, sizeof why) != 0 && strcmp(why, "captcha") == 0, "captcha named");
    /* garbage that decrypts to non-token text is refused */
    const char *junk = "short";
    CHECK(server_encrypt(pub, (const unsigned char *)junk, strlen(junk), enc, sizeof enc) == 0, "encrypt junk");
    CHECK(rauth_decrypt_token(&r, rng, &g_drbg, enc, outtok, sizeof outtok) != 0, "junk token refused");

    /* 7. misc ops and hostile input */
    CHECK(rauth_on_message(&r, rng, &g_drbg, "{\"op\":\"cancel\"}", strlen("{\"op\":\"cancel\"}"), reply, sizeof reply) == RA_EV_CANCEL, "cancel");
    CHECK(rauth_on_message(&r, rng, &g_drbg, "{\"op\":\"heartbeat_ack\"}", strlen("{\"op\":\"heartbeat_ack\"}"), reply, sizeof reply) == RA_EV_ACK, "ack");
    CHECK(rauth_on_message(&r, rng, &g_drbg, "{\"op\":\"future_thing\"}", strlen("{\"op\":\"future_thing\"}"), reply, sizeof reply) == RA_EV_NONE, "unknown op ignored");
    CHECK(rauth_on_message(&r, rng, &g_drbg, "not json", strlen("not json"), reply, sizeof reply) == RA_EV_ERROR, "non-json");
    CHECK(rauth_on_message(&r, rng, &g_drbg, "[]", strlen("[]"), reply, sizeof reply) == RA_EV_ERROR, "non-object");
    CHECK(rauth_on_message(&r, rng, &g_drbg, "{\"op\":5}", strlen("{\"op\":5}"), reply, sizeof reply) == RA_EV_ERROR, "non-string op");
    CHECK(strcmp(rauth_heartbeat_msg(), "{\"op\":\"heartbeat\"}") == 0, "heartbeat msg");

    rauth_free(&r);
    CHECK(r.have_key == 0, "freed");
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&ent);
    if(g_fail){ printf("rauth tests FAILED (%d)\n", g_fail); return 1; }
    printf("rauth tests passed\n");
    return 0;
}
