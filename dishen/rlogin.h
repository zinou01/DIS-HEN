/* rlogin.h - QR login service for the daemon (payload ELF only).
 *
 * The Setup app has no WebSocket/RSA stack, so the daemon (which already has
 * both, proven on hardware) does the Discord handshake and the Setup app only
 * draws the QR code. They talk through three small files in DATA_DIR:
 *
 *   qr_state.json   daemon -> app   {"v":1,"tick":N,"state":"...","url":...}
 *   qr_request      app -> daemon   exists = "start a login"
 *   qr_cancel       app -> daemon   exists = "abort the login in progress"
 *
 * "tick" increments on every state write, so the app can tell a live daemon
 * from a stale file without needing a shared clock. */
#ifndef DISHEN_RLOGIN_H
#define DISHEN_RLOGIN_H

#include "cfg.h"

#define RL_STATE_PATH  DATA_DIR "/qr_state.json"
#define RL_REQUEST_PATH DATA_DIR "/qr_request"
#define RL_CANCEL_PATH DATA_DIR "/qr_cancel"

/* Serve QR-login requests for up to `seconds` (returns early on a stop
 * request). Returns 1 when a new token was saved to config.json, else 0.
 * While nothing is requested it only refreshes the "idle" heartbeat. */
int rlogin_service(int seconds);

#endif
