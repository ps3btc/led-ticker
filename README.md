# LED Matrix Ticker

Type up to 16 characters into a web page and they scroll right-to-left across a
Modulino LED Matrix, repeating forever until you tap **Stop**.

Built for [Arduino App Lab](https://www.arduino.cc/app-lab) on the UNO Q, so the
message is captured in a browser on one machine, relayed through Linux on the
board, and rendered by an MCU sketch over I2C.

## Hardware

| Part | Notes |
|---|---|
| Arduino UNO Q | Runs the Linux side and the Zephyr MCU side |
| Modulino LED Matrix (ABX00152) | 12x8 monochrome, on `Wire1` via Qwiic |

The matrix is 12x8, so roughly two characters are visible at a time — that is why
the message scrolls rather than fitting.

## Layout

```
app.yaml               App Lab manifest: port 7000, arduino:web_ui brick
assets/index.html      Browser UI — single file, no build step, no framework
python/main.py         Linux side: HTTP API, input sanitizing, Bridge calls
sketch/sketch.ino      MCU side: owns the matrix, builds and advances frames
sketch/sketch.yaml     Build profiles and library pins
```

## Use it

Open `http://<board-ip>:7000`, type a message, tap **Submit**. Tap **Stop** to
clear the matrix. The page re-reads state from `GET /api/state` on load, so the
buttons reflect what is actually on the matrix rather than what was last clicked.

The UI is deliberately sparse: a 16-character input, a character counter, a board
status line, **Submit** / **Stop**, and a speed slider.

## Architecture

| Layer | File | Role |
|---|---|---|
| Browser | `assets/index.html` | Input, slider, Submit/Stop, plain `fetch()` |
| MPU (Linux) | `python/main.py` | `web_ui` brick on :7000, sanitizes input, `Bridge.call` |
| MCU (Zephyr) | `sketch/sketch.ino` | Owns the matrix on `Wire1`, renders frames |

Data flows one way at a time — nothing polls the board, and the browser learns the
truth only from the response body it just got.

### HTTP API

Registered with `ui.expose_api` in `python/main.py`.

| Method | Path | Body | Purpose |
|---|---|---|---|
| `POST` | `/api/text` | `{"text": "..."}` | Sanitize, push speed, scroll the text |
| `POST` | `/api/speed` | `{"speed": 400}` | Set ms per pixel of travel |
| `POST` | `/api/stop` | `{}` | Clear the matrix |
| `GET` | `/api/state` | — | Read current text, playing flag, speed, board presence |

Every handler returns the same shape, which is what lets the browser re-render from
any of them:

```json
{"ok": true, "matrix": true, "text": "HELLO", "playing": true, "speed": 240}
```

On rejection the body carries `error` plus the unchanged state, so the UI can roll
back the control that caused it:

```json
{"error": "Speed must be a number between 40 and 1200", "text": "HELLO", "playing": true, "speed": 240}
```

`matrix` is a tri-state, and reading it correctly matters when debugging:

| Value | Meaning |
|---|---|
| `true` | The sketch answered and the module acknowledged on I2C |
| `false` | Linux is fine, but no matrix answered — check the Qwiic cable |
| `null` | The sketch itself did not answer — see [Troubleshooting](#troubleshooting) |

### Bridge methods

Provided by the sketch (`Bridge.provide`), called from Python (`Bridge.call`):

| Method | Signature | Notes |
|---|---|---|
| `set_text` | `setText(String)` | Filters to printable ASCII, truncates at `MAX_CHARS`, latches a flag |
| `set_speed` | `setSpeed(int)` | Clamped to `MIN_MS`..`MAX_MS` |
| `stop_text` | `stopText()` | Latches `stopPending` |
| `matrix_ready` | `matrixReady() → bool` | Cached result of the detection attempts in `setup()` |

`submit_text` deliberately re-sends the current speed *before* the text. That
re-syncs the sketch after an MCU reset, which otherwise would still hold its
default speed while the Linux side believed otherwise.

### Speed

The slider sets **milliseconds per pixel of travel**: 40–1200, default 240, higher
is slower. `loop()` times each `nextFrame()` against that live value instead of the
duration baked into the captured frames, so a speed change applies to the scroll
that is already running and never needs a reflash.

The slider is inverted in the browser, not on the board — its left end is *slow*:

```js
const SPAN = 1240;                 // MIN_MS + MAX_MS
const toMs = (slider) => SPAN - Number(slider);
```

So `speed=1000` in the markup means 240 ms, and every state response re-derives the
slider position through `toSlider()`.

## Design notes

Two things worth knowing before editing `sketch.ino`:

**`ModulinoLEDMatrix::play()` blocks.** It is a `do {} while` with `delay()` inside,
so calling it from a Bridge handler would wedge the sketch and **Stop** would never
be served. Frames are advanced one at a time from `loop()` on a `millis()` cadence
instead, and `nextFrame()` wraps its own index back to 0 — that wrap *is* the repeat.

**Bridge handlers run on their own Zephyr thread.** So `setText` / `stopText` only
latch `volatile` flags, and every I2C write happens on the main thread in `loop()`.

Two further constraints that are easy to break:

- `MAX_CHARS` (16) is declared in both `sketch.ino` and `main.py` and must stay
  equal; the cap is also what keeps a `Font_4x6` animation (~92 frames) inside the
  128-frame buffer. Longer text is silently truncated by `endTextAnimation()` and
  noted on `Serial`, which is only reliably visible over a USB connection.
- Text is filtered to printable ASCII (`' '`..`~`) on both sides. Nothing below
  `0x20` or above `0x7E` can reach the matrix, which is what keeps a stray control
  byte from desynchronizing the I2C writes.

## Develop

```bash
arduino-app-cli app start   ~/ArduinoApps/led-ticker   # first run also flashes the sketch
arduino-app-cli app restart ~/ArduinoApps/led-ticker   # after edits (start refuses if running)
arduino-app-cli app logs    ~/ArduinoApps/led-ticker --follow
```

`app start` stops whichever App Lab app is currently running — only one runs at a
time.

Prefer the API over your eyes on the hardware when checking detection:

```bash
curl -s localhost:7000/api/state
# {"matrix":true,"text":"","playing":false,"speed":240}
```

If a sketch build acts stale after edits:
`arduino-app-cli app clean-cache user:led-ticker --force`, then restart.

### Start on boot

App Lab runs one default app and starts it when the board boots:

```bash
arduino-app-cli properties set default ~/ArduinoApps/led-ticker
cat /var/lib/arduino-app-cli/default.app     # confirm it persisted
```

This is worth doing explicitly. Without a default, an app whose container has
`restart=no` stays down after a power cycle while other containers on the box come
back, which reads as mysterious downtime.

### Reaching it remotely

`tailscale serve` keeps it inside your tailnet at
`https://<node>.<tailnet>.ts.net:8443`; `tailscale funnel` publishes the same route
to the public internet. Use a separate HTTPS port rather than a path prefix, because
`index.html` fetches absolute `/api/...` paths and a mount at `/ticker` would send
those requests to whatever owns the root.

## Security

**The app has no authentication.** Anyone who can reach the port can scroll text on
your hardware and change its speed. It is a display, not a data store, but treat the
port like an open microphone:

- Do not forward it, funnel it, or expose it on an untrusted network.
- If you do publish it, know that the message content you submit is readable by
  anyone who loads the page, via `GET /api/state`.
- Input is constrained to printable ASCII and 16 characters, and the browser side is
  cosmetic — the server-side filter in `_sanitize()` is the real limit.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `{"matrix":null,...}` and logs show `method set_text not available (2)` | The App Lab daemon lost its attachment to the board — commonly after a power cycle. Firmware is fine. | `sudo systemctl restart arduino-app-cli.service` |
| `{"matrix":false,...}` / `The board is running but no LED matrix answered on I2C` | Module not answering on `Wire1` | Reseat the Qwiic cable; a second Modulino on the bus can claim the default address `0x39` |
| Page does not load at all | Container exited and never came back | `arduino-app-cli app start ~/ArduinoApps/led-ticker`, then set it as the boot default (above) |
| Text scrolls, but at the wrong speed after an MCU reset | Sketch resumed at `DEFAULT_MS` | Tap **Submit** again — it re-sends speed before text |
| Long text cut off mid-message | Frame buffer full at 128 frames | Shorten the message; `MAX_CHARS` is the guard |
| `arduino-app-cli monitor` shows nothing | Expected on this setup — it produced no output even with a healthy sketch | Do not treat a silent monitor as a dead board |

## Known limitations

- One message at a time; no queue, no scheduling, no persistence across restarts.
- Repeats the same scroll forever — there is no "play once" mode.
- No authentication, TLS, or rate limiting of its own.
- Latin-1 printable range only: no accents, emoji, or right-to-left scripts.

## License

MIT — see [LICENSE](LICENSE).
