/* qrstate.h - reads the QR-login status file the daemon writes.
 *
 * Pure C, no SDL: the page that draws it lives in loginview.cpp, this file only
 * turns qr_state.json into a stage and a friendly sentence, so it can be tested
 * on a PC. The file format is defined in dishen/rlogin.h. */
#ifndef INSTALLER_QRSTATE_H
#define INSTALLER_QRSTATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	QS_UNKNOWN = 0, /* unreadable or unrecognised */
	QS_IDLE,        /* daemon is up and waiting for a request */
	QS_KEYGEN,
	QS_CONNECTING,
	QS_QR,          /* show the code */
	QS_SCANNED,     /* phone scanned, waiting for confirm */
	QS_FINISHING,
	QS_DONE,
	QS_CANCELLED,
	QS_EXPIRED,
	QS_ERROR
} qrstage_t;

typedef struct
{
	unsigned  tick;
	qrstage_t stage;
	char      url[128];
	char      user[96];
	char      err[16];
} qrstate_t;

/* Parse a status document. 0 on success; on failure *out is zeroed. */
int qrstate_parse(const char *json, size_t len, qrstate_t *out);

/* Read and parse a status file. 0 on success, -1 if missing/unreadable/bad. */
int qrstate_load(const char *path, qrstate_t *out);

/* Short heading and one-line explanation for a stage. Never NULL. Error text
 * names what went wrong in plain words. */
const char *qrstate_headline(const qrstate_t *s);
const char *qrstate_detail(const qrstate_t *s);

/* 1 while the stage is one where the page should keep waiting (not final). */
int qrstate_busy(const qrstate_t *s);

#ifdef __cplusplus
}
#endif

#endif
