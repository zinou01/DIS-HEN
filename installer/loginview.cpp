#include "loginview.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "console.h"
#include "dbglog.h"
#include "paint.h"
#include "qrstate.h"

namespace {

// Discord-style palette.
const Color kBg        = {  30,  31,  34 };
const Color kBgDeep    = {  17,  18,  20 };
const Color kPanel     = {  43,  45,  49 };
const Color kPanelHi   = {  53,  55,  60 };
const Color kBlurple   = {  88, 101, 242 };
const Color kGreen     = {  35, 165,  90 };
const Color kRed       = { 237,  66,  69 };
const Color kWhite     = { 242, 243, 245 };
const Color kMuted     = { 181, 186, 193 };
const Color kDim       = { 128, 132, 142 };

const char *const kFont = "/app0/assets/fonts/SegUIVar.ttf";

const int kW = 1920;
const int kH = 1080;
const int kFooterH = 76;

const int kQrPanelX = 150, kQrPanelY = 270, kQrPanelS = 560;
const int kCardX = 780, kCardY = 270, kCardW = 990, kCardH = 560;

const unsigned kPollMs        = 250;
const unsigned kOfflineIdleMs = 8000;    // idle heartbeat is every 2 s
const unsigned kOfflineBusyMs = 200000;  // session cap is 180 s
const unsigned kStuckMs       = 12000;   // request not picked up

bool FileExists(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (!f)
		return false;
	fclose(f);
	return true;
}

// Same paths as dishen/rlogin.h; kept literal because that header pulls in the
// payload's config types.
const char *const kStatePath   = "/data/Dis-HEN/qr_state.json";
const char *const kRequestPath = "/data/Dis-HEN/qr_request";
const char *const kCancelPath  = "/data/Dis-HEN/qr_cancel";

} // namespace

LoginView::LoginView()
	: dst_(NULL), bg_(NULL), ready_(false), haveState_(false), offline_(false),
	  stuck_(false), enterMs_(0), lastPollMs_(0), lastTickMs_(0), lastTick_(0),
	  side_(0)
{
	memset(&state_, 0, sizeof state_);
	memset(modules_, 0, sizeof modules_);
	qrUrl_[0] = 0;
}

LoginView::~LoginView()
{
	if (bg_)
		SDL_FreeSurface(bg_);
}

bool LoginView::Init(SDL_Surface *dst, FT_Library ftLib)
{
	(void)ftLib; // faces come from paint::Face, whose library is set at startup
	dst_ = dst;
	if (!dst_)
		return false;
	BuildBackground();
	ready_ = bg_ != NULL;
	return ready_;
}

void LoginView::BuildBackground()
{
	bg_ = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_RGBX8888);
	if (!bg_)
		return;

	paint::Clear(bg_, kBgDeep);
	paint::RadialGlow(bg_, 420, 240, 820, kBlurple, 0.28f);
	paint::RadialGlow(bg_, 1650, 900, 700, kBlurple, 0.16f);

	FT_Face title = paint::Face(kFont, 60, 700.0f);
	FT_Face sub   = paint::Face(kFont, 26, 400.0f);

	paint::FillRounded(bg_, 150, 96, 10, 66, 5, kBlurple, 255);
	if (title)
		paint::Text(bg_, 178, 150, "LOGIN WITH DISCORD", title, kWhite, 255);
	if (sub)
		paint::Text(bg_, 180, 198,
		            "Log in once. Dis-HEN saves it and shows your game on Discord by itself.",
		            sub, kMuted, 255);

	// Panels that never change.
	paint::FillRounded(bg_, kCardX, kCardY, kCardW, kCardH, 28, kPanel, 255);
	paint::StrokeRounded(bg_, kCardX, kCardY, kCardW, kCardH, 28, 2, kPanelHi, 255);
}

void LoginView::RequestFile(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (f)
	{
		fputs("1\n", f);
		fclose(f);
	}
	else
		dbglog::Write("LoginView: cannot create %s", path);
}

void LoginView::Enter()
{
	// A "done" or "qr" file left from an earlier visit must not be mistaken for
	// this visit's answer.
	remove(kStatePath);
	remove(kCancelPath);
	memset(&state_, 0, sizeof state_);
	haveState_ = false;
	offline_ = false;
	stuck_ = false;
	side_ = 0;
	qrUrl_[0] = 0;
	enterMs_ = SDL_GetTicks();
	lastPollMs_ = 0;
	lastTickMs_ = enterMs_;
	lastTick_ = 0;
	RequestFile(kRequestPath);
}

void LoginView::Leave()
{
	// Withdraw an unanswered request and stop a session in progress, so the
	// payload is not left holding a QR code nobody is looking at.
	remove(kRequestPath);
	if (haveState_ && qrstate_busy(&state_))
		RequestFile(kCancelPath);
}

void LoginView::Restart()
{
	if (Busy())
		return;
	remove(kCancelPath);
	remove(kStatePath);
	memset(&state_, 0, sizeof state_);
	haveState_ = false;
	offline_ = false;
	stuck_ = false;
	side_ = 0;
	qrUrl_[0] = 0;
	enterMs_ = SDL_GetTicks();
	lastTickMs_ = enterMs_;
	lastTick_ = 0;
	RequestFile(kRequestPath);
}

void LoginView::Update()
{
	const uint32_t now = SDL_GetTicks();
	if (now - lastPollMs_ < kPollMs)
		return;
	lastPollMs_ = now;

	qrstate_t s;
	if (qrstate_load(kStatePath, &s) == 0)
	{
		if (!haveState_ || s.tick != lastTick_)
		{
			lastTick_ = s.tick;
			lastTickMs_ = now;
		}
		state_ = s;
		haveState_ = true;
	}

	const uint32_t quiet = now - lastTickMs_;
	const bool busyStage = haveState_ && qrstate_busy(&state_);

	// A busy stage can legitimately go a long time without a write (key
	// generation), so only the idle heartbeat is held to the short deadline.
	offline_ = !haveState_ ? (now - enterMs_ > kOfflineIdleMs)
	         : busyStage   ? (quiet > kOfflineBusyMs)
	         : (state_.stage == QS_IDLE && quiet > kOfflineIdleMs);

	// The payload is alive and idling but never took the request: it already
	// has a working login and is not listening for a new one.
	stuck_ = !offline_ && haveState_ && state_.stage == QS_IDLE &&
	         FileExists(kRequestPath) && (now - enterMs_ > kStuckMs);

	if (haveState_ && state_.stage == QS_QR &&
	    strcmp(qrUrl_, state_.url) != 0)
	{
		snprintf(qrUrl_, sizeof qrUrl_, "%s", state_.url);
		side_ = qr_encode(qrUrl_, modules_, sizeof modules_);
		if (side_ == 0)
			dbglog::Write("LoginView: QR encode failed");
	}
}

int LoginView::DrawWrapped(int x, int baseline, int maxW, const char *text,
                           FT_Face face, int lineGap)
{
	std::string line, word;
	int y = baseline;
	const std::string src = text;
	for (size_t i = 0; i <= src.size(); ++i)
	{
		const char c = i < src.size() ? src[i] : ' ';
		if (c != ' ')
		{
			word += c;
			continue;
		}
		if (word.empty())
			continue;
		const std::string trial = line.empty() ? word : line + " " + word;
		if (!line.empty() && paint::TextWidth(trial, face) > maxW)
		{
			paint::Text(dst_, x, y, line, face, kMuted, 255);
			y += lineGap;
			line = word;
		}
		else
			line = trial;
		word.clear();
	}
	if (!line.empty())
	{
		paint::Text(dst_, x, y, line, face, kMuted, 255);
		y += lineGap;
	}
	return y;
}

void LoginView::DrawSpinner(int cx, int cy, uint32_t ms)
{
	static const int kCos[8] = { 100, 71, 0, -71, -100, -71, 0, 71 };
	static const int kSin[8] = { 0, 71, 100, 71, 0, -71, -100, -71 };
	const int head = (int)((ms / 110) % 8);
	for (int i = 0; i < 8; ++i)
	{
		const int age = (head - i + 8) % 8;
		const uint8_t a = (uint8_t)(255 - age * 30);
		const int dx = kCos[i] * 62 / 100, dy = kSin[i] * 62 / 100;
		paint::FillRounded(dst_, cx + dx - 9, cy + dy - 9, 18, 18, 9, kBlurple, a);
	}
}

void LoginView::DrawQr(int x, int y, int size)
{
	// White panel: a QR code needs a light margin to scan, and this is it.
	paint::FillRounded(dst_, x, y, size, size, 28, Color{255, 255, 255}, 255);
	if (side_ <= 0)
		return;

	const int avail = size - 2 * 40;
	const int scale = avail / side_;
	const int total = scale * side_;
	const int ox = x + (size - total) / 2;
	const int oy = y + (size - total) / 2;

	for (int r = 0; r < side_; ++r)
		for (int c = 0; c < side_; ++c)
			if (modules_[r * side_ + c])
				paint::FillRect(dst_, ox + c * scale, oy + r * scale, scale,
				                scale, Color{0, 0, 0}, 255);
}

void LoginView::DrawSteps(int x, int y)
{
	static const char *const kSteps[3] = {
		"Open Discord on your phone",
		"Go to Settings and tap Scan QR Code",
		"Point it at the code and tap Yes, log me in",
	};
	int active = -1;
	bool all = false;
	if (haveState_ && !offline_ && !stuck_)
	{
		if (state_.stage == QS_QR)            active = 0;
		else if (state_.stage == QS_SCANNED)  active = 2;
		else if (state_.stage == QS_FINISHING) active = 2;
		else if (state_.stage == QS_DONE)     all = true;
	}

	FT_Face num = paint::Face(kFont, 26, 700.0f);
	FT_Face txt = paint::Face(kFont, 28, 400.0f);
	for (int i = 0; i < 3; ++i)
	{
		const int cy = y + i * 64;
		const bool done = all || (active > i);
		const bool on = (i == active);
		const Color fill = done ? kGreen : on ? kBlurple : kPanelHi;
		paint::FillRounded(dst_, x, cy - 30, 44, 44, 22, fill, 255);
		char n[4];
		snprintf(n, sizeof n, "%d", i + 1);
		if (num)
			paint::Text(dst_, x + 22 - paint::TextWidth(n, num) / 2, cy + 3, n,
			            num, kWhite, 255);
		if (txt)
			paint::Text(dst_, x + 66, cy + 4, kSteps[i], txt,
			            (done || on) ? kWhite : kDim, 255);
	}
}

void LoginView::DrawFooter()
{
	paint::FillRect(dst_, 0, kH - kFooterH, kW, kFooterH, kBgDeep, 255);
	paint::FillRect(dst_, 0, kH - kFooterH, kW, 2, kPanelHi, 255);

	FT_Face f = paint::Face(kFont, 26, 600.0f);
	if (!f)
		return;
	const int base = kH - kFooterH + 48;

	// Button hints as little badges.
	int x = 64;
	const bool canRetry = !Busy();
	if (canRetry)
	{
		paint::FillRounded(dst_, x, base - 30, 40, 40, 20, kBlurple, 255);
		paint::Text(dst_, x + 20 - paint::TextWidth("X", f) / 2, base - 1, "X", f, kWhite, 255);
		paint::Text(dst_, x + 54, base, "New code", f, kMuted, 255);
		x += 54 + paint::TextWidth("New code", f) + 46;
	}
	paint::FillRounded(dst_, x, base - 30, 40, 40, 20, kPanelHi, 255);
	paint::Text(dst_, x + 20 - paint::TextWidth("O", f) / 2, base - 1, "O", f, kWhite, 255);
	paint::Text(dst_, x + 54, base, "Back", f, kMuted, 255);

	const std::string a = "MADE BY ", b = "DZDEXTER404";
	const int total = paint::TextWidth(a, f) + paint::TextWidth(b, f);
	const int cx = (kW - total) / 2;
	paint::Text(dst_, cx, base, a, f, kMuted, 255);
	paint::Text(dst_, cx + paint::TextWidth(a, f), base, b, f, kBlurple, 255);
}

void LoginView::Draw()
{
	if (!ready_ || !dst_)
		return;

	paint::BlitOpaque(dst_, bg_, 0, 0);
	const uint32_t now = SDL_GetTicks();

	// Left: the code, or a spinner / result glyph in its place.
	const bool showQr = haveState_ && !offline_ && !stuck_ &&
	                    state_.stage == QS_QR && side_ > 0;
	if (showQr)
		DrawQr(kQrPanelX, kQrPanelY, kQrPanelS);
	else
	{
		paint::FillRounded(dst_, kQrPanelX, kQrPanelY, kQrPanelS, kQrPanelS, 28,
		                   kPanel, 255);
		paint::StrokeRounded(dst_, kQrPanelX, kQrPanelY, kQrPanelS, kQrPanelS, 28,
		                     2, kPanelHi, 255);
		const int cx = kQrPanelX + kQrPanelS / 2, cy = kQrPanelY + kQrPanelS / 2;
		const qrstage_t st = (offline_ || stuck_ || !haveState_) ? QS_UNKNOWN
		                                                         : state_.stage;
		if (st == QS_DONE)
		{
			paint::FillRounded(dst_, cx - 90, cy - 90, 180, 180, 90, kGreen, 255);
			FT_Face ok = paint::Face(kFont, 84, 700.0f);
			if (ok)
				paint::Text(dst_, cx - paint::TextWidth("OK", ok) / 2, cy + 30,
				            "OK", ok, kWhite, 255);
		}
		else if (st == QS_ERROR || st == QS_EXPIRED || st == QS_CANCELLED ||
		         stuck_ || offline_)
		{
			const Color c = (st == QS_ERROR || offline_ || stuck_) ? kRed : kDim;
			paint::FillRounded(dst_, cx - 90, cy - 90, 180, 180, 90, c, 255);
			FT_Face ex = paint::Face(kFont, 110, 700.0f);
			if (ex)
				paint::Text(dst_, cx - paint::TextWidth("!", ex) / 2, cy + 38,
				            "!", ex, kWhite, 255);
		}
		else
			DrawSpinner(cx, cy, now);
	}

	// Right: headline, explanation, steps.
	const int tx = kCardX + 56;
	FT_Face head = paint::Face(kFont, 50, 700.0f);
	FT_Face body = paint::Face(kFont, 30, 400.0f);

	const char *headline, *detail;
	if (stuck_)
	{
		headline = "Dis-HEN is already logged in";
		detail = "It already has a working login, so it is not asking for a new "
		         "one. To switch accounts, clear the token in Settings and "
		         "restart the payload.";
	}
	else if (offline_)
	{
		headline = "Waiting for Dis-HEN";
		detail = "Start the Dis-HEN payload on your PS4, then come back to "
		         "this page.";
	}
	else
	{
		headline = qrstate_headline(&state_);
		detail = qrstate_detail(&state_);
		if (!haveState_)
		{
			headline = "Waiting for Dis-HEN";
			detail = "Contacting the payload...";
		}
	}

	if (head)
		paint::Text(dst_, tx, kCardY + 100, headline, head, kWhite, 255);
	if (body)
		DrawWrapped(tx, kCardY + 160, kCardW - 112, detail, body, 42);

	if (haveState_ && !offline_ && !stuck_ && state_.user[0] &&
	    (state_.stage == QS_SCANNED || state_.stage == QS_FINISHING ||
	     state_.stage == QS_DONE))
	{
		FT_Face small = paint::Face(kFont, 28, 600.0f);
		if (small)
		{
			const std::string who = std::string("Account: ") + state_.user;
			paint::FillRounded(dst_, tx, kCardY + 290,
			                   paint::TextWidth(who, small) + 44, 52, 26,
			                   kPanelHi, 255);
			paint::Text(dst_, tx + 22, kCardY + 326, who, small, kWhite, 255);
		}
	}

	DrawSteps(tx, kCardY + 420);
	DrawFooter();
}
