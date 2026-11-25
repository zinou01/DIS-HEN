/* loginview.h - the "Login with Discord" page.
 *
 * Draws the QR code the daemon asks for and walks the user through scanning it.
 * The Discord handshake itself is done by the payload (dishen/rlogin.c); this
 * page only writes a request file, reads qr_state.json and paints. Nothing on
 * this page ever sees the token. */
#ifndef INSTALLER_LOGINVIEW_H
#define INSTALLER_LOGINVIEW_H

#include <stdint.h>

#include <SDL2/SDL.h>
#include <proto-include.h> // FT_Library

#include "qr.h"
#include "qrstate.h"

class LoginView
{
public:
	LoginView();
	~LoginView();

	bool Init(SDL_Surface *dst, FT_Library ftLib);

	// Page opened: clears any stale status and asks the payload for a code.
	void Enter();

	// Page closed: tells the payload to stop a login that is in progress.
	void Leave();

	// X pressed: asks for a fresh code, unless one is already being made.
	void Restart();

	// Polls the status file (rate-limited internally). Call once per frame.
	void Update();

	void Draw();

	// True once the token has been saved.
	bool Done() const { return state_.stage == QS_DONE; }

	// True while a login is in progress and X would do nothing.
	bool Busy() const { return !offline_ && !stuck_ && qrstate_busy(&state_); }

private:
	void BuildBackground();
	void DrawQr(int x, int y, int size);
	void DrawSpinner(int cx, int cy, uint32_t ms);
	void DrawSteps(int x, int y);
	void DrawFooter();
	int  DrawWrapped(int x, int baseline, int maxW, const char *text,
	                 FT_Face face, int lineGap);
	void RequestFile(const char *path);

	SDL_Surface *dst_;
	SDL_Surface *bg_;
	bool         ready_;

	qrstate_t state_;
	bool      haveState_;
	bool      offline_;   // payload not answering
	bool      stuck_;     // payload is up but already logged in
	uint32_t  enterMs_;
	uint32_t  lastPollMs_;
	uint32_t  lastTickMs_;
	unsigned  lastTick_;

	uint8_t modules_[QR_MAX_MODULES];
	int     side_;
	char    qrUrl_[128];
};

#endif
