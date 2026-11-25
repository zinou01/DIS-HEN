# Getting your Discord token (2 minutes)

The daemon needs your Discord **user session token**. It goes in one
place only: `/data/Dis-HEN/config.json`, in the `"token"` slot.

## Step 1 — copy it from your browser

1. On your PC/Mac, open Discord in a browser (Thorium, Chrome, anything
   logged in) and press **F12** (DevTools).
2. At the top of the console panel, make sure the context dropdown says
   **`top`** — not a worker. (Wrong context gives
   `localStorage is not defined`.)
3. Paste this and hit Enter:
   ```
   copy(localStorage.token)
   ```
   The token is now on your clipboard. It looks like
   `MTM4...Gs0Z...-Tcr8...` (three parts separated by dots).

Do NOT paste the token into any Discord chat or server — Discord
auto-invalidates leaked tokens on sight, and then it stops working.

## Step 2 — paste it into config.json

1. Open `/data/Dis-HEN/config.json` on the PS4 (FTP it to your PC,
   edit, send it back — or edit in place).
2. Replace `"SET_ME"` with your token, keeping the quotes:
   ```json
   {"token": "paste-it-here", ...}
   ```
3. Save, re-run the payload from `/data/payloads`.

## Step 3 — check it worked

- Launch a game → your Discord profile shows it within ~30 seconds.
- In `log.txt`: `discord: gateway ready`, no `4004` lines.
- If you see `gateway closed during auth: 4004`, the token is dead —
  redo Step 1 (password changes and "log out everywhere" kill tokens).

One account per token. Each login mints a fresh one; old copies die.

## Easiest way: log in with a QR code

1. Install Dis-HEN with the Setup app and start the payload on your PS4.
2. Open the Setup app and choose **LOGIN**.
3. On your phone open Discord, go to **Settings > Scan QR Code**, point it at the
   code on the TV and tap **Yes, log me in**.
4. Dis-HEN saves the login to `/data/Dis-HEN/config.json` and starts by itself.
   There is nothing to copy or paste.

Notes:
- The payload must be running. If the page says "Waiting for Dis-HEN", start it.
- If Dis-HEN already has a working token it does not ask for a new login. To
  switch accounts, clear the token in **Settings** and restart the payload.
- Making the one-time key can take up to a minute on the console.
- This uses Discord's unofficial QR login for a user account, which Discord's
  terms do not allow. Use an account you can afford to lose.
- If Discord asks for a captcha, log in once in your phone's browser and retry.
