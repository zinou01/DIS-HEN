/* localmeta.h - title name + cover from the console's own appmeta folders.
 *
 *   name  : <sfo_dir>/<TITLEID>/param.sfo   (default /system_data/priv/appmeta)
 *   image : <img_dir>/<TITLEID>/icon0.png   (default /user/appmeta)
 *
 * External-storage apps live under <dir>/external/<TITLEID>/ (checked after
 * the two above, along with a few other known spots).
 * Each location falls back to the other directory, so a title staged in
 * either place is found. The cover is uploaded once to Cloudinary (see
 * cloudinary.h); the resulting https URL is what the presence uses, and it is
 * persisted in games_cache.json so later boots do not touch the network.
 *
 * Fork addition -- not part of upstream Dis-HEN. */
#ifndef DISHEN_LOCALMETA_H
#define DISHEN_LOCALMETA_H
#include <stddef.h>

#define LM_DEFAULT_SFO_DIR "/system_data/priv/appmeta"
#define LM_DEFAULT_IMG_DIR "/user/appmeta"

#define LM_NAME 1   /* name was filled */
#define LM_ART  2   /* art URL was filled */

/* --- pure helpers (host-testable) ----------------------------------- */

/* 9 chars, [A-Z0-9] only: blocks path traversal and junk ids. */
int localmeta_tid_valid(const char *tid);

/* "<base>/<tid>/<file>" -> out. 0 ok, -1 invalid tid / overflow. */
int localmeta_join(const char *base, const char *tid, const char *file,
                   char *out, size_t cap);

/* --- file lookups ---------------------------------------------------- */

/* TITLE from param.sfo. 0 + name, else -1. dirs may be NULL for defaults. */
int localmeta_title(const char *sfo_dir, const char *img_dir,
                    const char *tid, char *out, size_t cap);

/* First existing icon0.png (lower- then upper-case name). 0 + path, else -1. */
int localmeta_icon_path(const char *sfo_dir, const char *img_dir,
                        const char *tid, char *out, size_t cap);

/* One-stop resolver used by detect.c. Cheap to call every poll: the result
 * for the current title id is memoised, so files are read and the upload is
 * attempted at most once per title per boot. Returns a bitmask of
 * LM_NAME / LM_ART (0 = nothing local, caller falls through to the usual
 * sources). Fills name_out / art_out only for the bits it sets. */
int localmeta_resolve(const char *tid,
                      char *name_out, size_t name_cap,
                      char *art_out, size_t art_cap);
#endif
