/* cloudinary.h - unsigned image upload to Cloudinary (fork addition).
 *
 * POST https://api.cloudinary.com/v1_1/<cloud>/image/upload as multipart
 * (file + upload_preset + public_id) and return the delivery secure_url.
 * Unsigned: no API secret is stored or sent. Pure helpers are host-tested;
 * cld_upload_png() is PS4-only (host builds return -1), mirroring art.c. */
#ifndef DISHEN_CLOUDINARY_H
#define DISHEN_CLOUDINARY_H
#include <stddef.h>

#define CLD_MAX_FILE (3u * 1024u * 1024u)   /* refuse to push anything bigger */

/* Only our own delivery host counts as "already uploaded". */
int cld_url_ok(const char *url);

/* "<folder>/<TITLEID>_<kind>" -> out. folder may be empty. Allowed folder
 * chars: [A-Za-z0-9_-/]. 0 ok, -1 invalid. */
int cld_public_id(const char *folder, const char *tid, const char *kind,
                  char *out, size_t cap);

/* Multipart body up to and including the file part's header line (the raw
 * PNG bytes follow), and the closing delimiter. Returns length or -1. */
int cld_multipart_head(char *out, size_t cap, const char *boundary,
                       const char *preset, const char *public_id);
int cld_multipart_tail(char *out, size_t cap, const char *boundary);

/* secure_url from an upload response (must be https://res.cloudinary.com/).
 * 0 + url, else -1. */
int cld_parse_url(const char *body, size_t len, char *out, size_t cap);

/* Upload the PNG at path. 0 + url on success. Blocking, bounded by deadlines. */
int cld_upload_png(const char *cloud, const char *preset, const char *public_id,
                   const char *path, char *out_url, size_t cap);
#endif
