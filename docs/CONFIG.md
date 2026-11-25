# Config reference (`/data/Dis-HEN/config.json`)

Only `token` is required. Everything else ships with working defaults
(see `cfg_defaults()` in `dishen/cfg.c`). Junk values are clamped
(`poll_interval_s` is forced into 5–300); a `SET_ME` token logs a
reminder and the daemon waits instead of exiting.
When the daemon or setup app writes the config, it uses owner-only file
permissions (`0600`) because the token grants full account access.

## Auth

| Key | Default | Meaning |
|---|---|---|
| `token` | `"SET_ME"` | Discord user session token (**required**). Full account access — never share it, never commit it. A gateway `4004` means Discord rejected the login; check Discord's account recovery prompts, then enter a fresh token if the account is healthy. |
| `qos_token` | `""` | Server-issued Gateway session data. Managed automatically, stored with the same owner-only permissions as the config, and cleared when the account token changes. |
| `application_id` | `"1574591231785308330"` | Your Discord application (fork default). Used for the `mp:` external-assets calls and shown as the "Playing …" app. |
| `enabled` | `1` | Master switch. |

## Presence text

Discord shows three lines: name, details, state (+ timer). Details says
what you're doing; state carries the firmware.

| Key | Default | Meaning |
|---|---|---|
| `presence_state` | `"On PS4"` | State line for games and home. |
| `presence_details_game` | `"Playing on PlayStation 4"` | Details line while a game is open. |
| `presence_details_home` | `"Idling on Home Menu"` | Details line on home. |
| `presence_settings_text` | `"In Settings"` | Activity name while Settings is open (replaces the whole presence; the game timer underneath is kept). |

## Visibility flags (all default `true`)

A `false` flag still detects the title — it just stops telling Discord
about it, so a game underneath keeps showing. Exception: with
`show_idle` off, a closed game clears (nothing runs underneath home).

| Key | Hides |
|---|---|
| `show_firmware` | The `Firmware X.YY` line. |
| `show_idle` | Home screen, Settings, browser presence. |
| `show_media` | Media apps (Netflix, YouTube, Plex…) as `Watching`. |
| `show_homebrew` | Homebrew titles. Retro classics (PS1/PS2/PSP) are **not** affected. |

## Sources & art

| Key | Default | Meaning |
|---|---|---|
| `poll_interval_s` | `12` | Detection cadence (clamped 5–300). |
| `pkgzone_enabled` | `true` | Ask pkg-zone.com for **homebrew-only** names. Sends the title id to a third party — one boolean away from off. Retail titles never reach it. |
| `retro_enabled` | `true` | PS1/PS2/PSP names + covers from the retro indexes. |
| `art_base_url` | project icon pack | Per-title art source, resolved through Discord's `mp:` proxy at post time. |
| `large_art` / `small_art` / `browser_art` | project logos | Fallback artwork (large falls back to `home_art`, small to large). Raw `https` only renders via the `mp:` proxy — a typo here blanks the tile, nothing else. |
| `asset_idle` / `asset_playing` | empty | Uploaded Discord asset keys. A key needs no network; empty means the URLs above get used. |
| `home_art` | project logo | Legacy idle-tile fallback (predates `large_art`). |
| `auto_update` | `1` | Signed self-updates with boot rollback. |
| `debug` | `false` | Verbose per-poll logging. |

## Self-learned state (daemon-written, don't hand-edit)

- `titles` map — every resolved title is remembered, so later boots
  resolve instantly. Also the manual-override slot: one entry fixes a
  title forever.
- `session.json` / `playtime.log` — resume window (10 min, cross-restart)
  and the playtime ledger.

## Local appmeta + Cloudinary covers (fork)

The daemon first looks at the console's own metadata; only if that has
nothing does it fall through to cache / TMDB / pkg-zone / retro / custom.

| Key | Default | Meaning |
|---|---|---|
| `localmeta_enabled` | `true` | Master switch for the local lookup. |
| `local_sfo_dir` | `/system_data/priv/appmeta` | `<dir>/<TITLEID>/param.sfo` supplies the title name. |
| `local_img_dir` | `/user/appmeta` | `<dir>/<TITLEID>/icon0.png` supplies the cover. Each dir is the fallback for the other. |
| `cloud_name` | `u7sphtjn` | Cloudinary cloud name. Empty = never upload. |
| `cloud_preset` | `PS4PS4` | Cloudinary **unsigned** upload preset. |
| `cloud_folder` | `ps4` | Folder part of the public id (`ps4/CUSA12345_icon0`). |

The cover is uploaded once per title; the returned Cloudinary URL is stored
in `games_cache.json` and then goes through Discord's `mp:` proxy like any
other art URL. Log lines to look for: `name: ... via local appmeta`,
`art: local icon0 -> cloudinary ok`, `cloudinary: ...` on failure.
