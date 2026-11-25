#include "qrstate.h"

#include <stdio.h>
#include <string.h>

#include "jsonlite.h"

static void copy_str(char *dst, size_t cap, const jl_val_t *v)
{
	dst[0] = 0;
	if (v && v->type == JL_STRING && v->str)
		snprintf(dst, cap, "%s", v->str);
}

static qrstage_t stage_of(const char *s)
{
	static const struct { const char *name; qrstage_t st; } map[] = {
		{"idle", QS_IDLE},         {"keygen", QS_KEYGEN},
		{"connecting", QS_CONNECTING}, {"qr", QS_QR},
		{"scanned", QS_SCANNED},   {"finishing", QS_FINISHING},
		{"done", QS_DONE},         {"cancelled", QS_CANCELLED},
		{"expired", QS_EXPIRED},   {"error", QS_ERROR},
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
		if (strcmp(map[i].name, s) == 0)
			return map[i].st;
	return QS_UNKNOWN;
}

int qrstate_parse(const char *json, size_t len, qrstate_t *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof *out);
	if (!json || len == 0)
		return -1;

	jl_val_t *root = jl_parse(json, len);
	if (!root || root->type != JL_OBJECT)
	{
		jl_free(root);
		return -1;
	}

	const jl_val_t *st = jl_obj_get(root, "state");
	const jl_val_t *tk = jl_obj_get(root, "tick");
	if (!st || st->type != JL_STRING || !st->str ||
	    !tk || tk->type != JL_NUMBER || tk->num_is_int == 0 || tk->inum < 0)
	{
		jl_free(root);
		return -1;
	}

	out->tick  = (unsigned)tk->inum;
	out->stage = stage_of(st->str);
	copy_str(out->url, sizeof out->url, jl_obj_get(root, "url"));
	copy_str(out->user, sizeof out->user, jl_obj_get(root, "user"));
	copy_str(out->err, sizeof out->err, jl_obj_get(root, "err"));
	jl_free(root);

	/* A code with no address in it is no code at all. */
	if (out->stage == QS_QR && strncmp(out->url, "https://discord.com/ra/", 23) != 0)
	{
		memset(out, 0, sizeof *out);
		return -1;
	}
	return 0;
}

int qrstate_load(const char *path, qrstate_t *out)
{
	char buf[1024];
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		if (out)
			memset(out, 0, sizeof *out);
		return -1;
	}
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return qrstate_parse(buf, n, out);
}

const char *qrstate_headline(const qrstate_t *s)
{
	switch (s ? s->stage : QS_UNKNOWN)
	{
	case QS_IDLE:       return "Ready";
	case QS_KEYGEN:     return "Preparing secure login";
	case QS_CONNECTING: return "Connecting to Discord";
	case QS_QR:         return "Scan with your phone";
	case QS_SCANNED:    return "Almost there";
	case QS_FINISHING:  return "Logging you in";
	case QS_DONE:       return "You're logged in";
	case QS_CANCELLED:  return "Login cancelled";
	case QS_EXPIRED:    return "Code expired";
	case QS_ERROR:      return "Login didn't work";
	default:            return "Waiting for Dis-HEN";
	}
}

const char *qrstate_detail(const qrstate_t *s)
{
	switch (s ? s->stage : QS_UNKNOWN)
	{
	case QS_IDLE:       return "Starting the login...";
	case QS_KEYGEN:     return "Making a one-time key. This can take up to a minute.";
	case QS_CONNECTING: return "Just a moment.";
	case QS_QR:
		return "Open Discord on your phone, go to Settings > Scan QR Code, "
		       "and point it at the code.";
	case QS_SCANNED:    return "Tap \"Yes, log me in\" on your phone to confirm.";
	case QS_FINISHING:  return "Saving your login to the console.";
	case QS_DONE:
		return "Saved. Dis-HEN will start showing your game on Discord.";
	case QS_CANCELLED:  return "You can press X to try again.";
	case QS_EXPIRED:    return "Press X to get a fresh code.";
	case QS_ERROR:
		if (s && strcmp(s->err, "net") == 0)
			return "Couldn't reach Discord. Check the console's internet, then try again.";
		if (s && strcmp(s->err, "captcha") == 0)
			return "Discord asked for a captcha. Log in once on your phone's browser, then try again.";
		if (s && strcmp(s->err, "key") == 0)
			return "Couldn't create the secure key. Try again.";
		if (s && strcmp(s->err, "proto") == 0)
			return "Discord sent something unexpected. Try again.";
		return "Discord refused the login. Press X to try again.";
	default:
		return "Start the Dis-HEN payload on your PS4, then come back here.";
	}
}

int qrstate_busy(const qrstate_t *s)
{
	switch (s ? s->stage : QS_UNKNOWN)
	{
	case QS_KEYGEN: case QS_CONNECTING: case QS_QR:
	case QS_SCANNED: case QS_FINISHING:
		return 1;
	default:
		return 0;
	}
}
