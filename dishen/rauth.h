/* rauth.h - Discord "remote auth" (QR login) protocol, transport-free.
 *
 * Everything here is pure computation on strings and an RSA key, so the whole
 * handshake can be driven and tested on a PC. The payload-only half that owns
 * the socket lives in rlogin.c.
 *
 * Flow (server messages in, our replies out):
 *   hello                -> init { encoded_public_key }
 *   nonce_proof          -> nonce_proof { sha256(decrypted nonce) base64url }
 *   pending_remote_init  -> fingerprint: the QR code is
 *                           https://discord.com/ra/<fingerprint>
 *   pending_ticket       -> phone scanned; encrypted user info (name only used)
 *   pending_login        -> ticket; caller POSTs it to the login endpoint and
 *                           decrypts the returned encrypted_token
 *
 * Secrets (private key, decrypted token) are never logged here. */
#ifndef DISHEN_RAUTH_H
#define DISHEN_RAUTH_H

#include <stddef.h>
#include <mbedtls/pk.h>

#define RAUTH_KEY_BITS   2048
#define RAUTH_FP_MAX     96
#define RAUTH_TICKET_MAX 1024
#define RAUTH_USER_MAX   96

typedef int (*rauth_rng_fn)(void *ctx, unsigned char *out, size_t len);

typedef enum {
    RA_EV_NONE = 0,   /* nothing for the caller to do */
    RA_EV_SEND,       /* send `reply` back over the socket */
    RA_EV_QR,         /* fingerprint known: show the QR code */
    RA_EV_SCANNED,    /* phone scanned the code; r->user holds the name */
    RA_EV_TICKET,     /* phone confirmed; r->ticket is ready to POST */
    RA_EV_CANCEL,     /* user cancelled on the phone */
    RA_EV_ACK,        /* heartbeat acknowledged */
    RA_EV_ERROR       /* malformed / unusable message */
} ra_event_t;

typedef struct {
    mbedtls_pk_context pk;
    int  have_key;
    int  heartbeat_ms;               /* from hello; 0 until seen */
    int  timeout_ms;                 /* from hello; 0 until seen */
    char fingerprint[RAUTH_FP_MAX];
    char user[RAUTH_USER_MAX];
    char ticket[RAUTH_TICKET_MAX];
} rauth_t;

void rauth_init(rauth_t *r);
void rauth_free(rauth_t *r);

/* Generate the RSA keypair (slow on a console: tens of seconds).
 * Returns 0 on success. */
int rauth_keygen(rauth_t *r, rauth_rng_fn rng, void *rng_ctx, unsigned bits);

/* Base64 (standard, padded) of the DER SubjectPublicKeyInfo. 0 on success. */
int rauth_pubkey_b64(rauth_t *r, char *out, size_t cap);

/* Feed one text frame from the gateway. For RA_EV_SEND the frame to send back
 * is written to `reply`. Always returns one of ra_event_t. */
int rauth_on_message(rauth_t *r, rauth_rng_fn rng, void *rng_ctx,
                     const char *json, size_t len,
                     char *reply, size_t reply_cap);

/* QR payload for the fingerprint. 0 on success. */
int rauth_qr_url(const rauth_t *r, char *out, size_t cap);

/* JSON body for the ticket exchange POST. 0 on success. */
int rauth_login_body(const rauth_t *r, char *out, size_t cap);

/* Heartbeat frame text. */
const char *rauth_heartbeat_msg(void);

/* Decrypt the encrypted_token from the login response. 0 on success. */
int rauth_decrypt_token(rauth_t *r, rauth_rng_fn rng, void *rng_ctx,
                        const char *b64, char *out, size_t cap);

/* Pull encrypted_token out of the login response body (JSON). 0 on success;
 * on failure `why` (if given) says what the server wanted instead. */
int rauth_parse_login_response(const char *body, size_t len,
                               char *enc_token, size_t cap,
                               char *why, size_t why_cap);

#endif
