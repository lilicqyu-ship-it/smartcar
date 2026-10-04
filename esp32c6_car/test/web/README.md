# C6 control UI regression checks

Run from the repository root with Node.js and Playwright available:

```sh
node esp32c6_car/test/web/control-ui.cjs
node esp32c6_car/test/web/calibration-ui.cjs
```

The script serves the actual assets through Playwright routes and mocks the C6
WebSocket. It checks 1440px, 390px and 320px layouts, pointer hover/drag/release,
stop while holding the joystick, pointer cancellation, spectator control gating,
and missing firmware feedback. No vehicle or firmware upload is required.

Use `CHROME_PATH` to run an installed Chrome instead of Playwright's bundled
Chromium. Set `SCREENSHOT_DIR` to an existing directory to save layout previews.
Playwright can be supplied through `NODE_PATH` if installed outside this project.

These checks do not replace validation on the C6 device and a mobile browser.

Calibration checks cover SE2 (375 × 667), sticky stop visibility, airborne
confirmation, calibration/jog interlocks, device results, jog release, numeric
parameter validation and loss of control authority.

Save receipt checks distinguish live-record echoes from final flash outcomes,
cover unknown/invalid `saved`, timeout recovery, a real failure followed by a
successful final receipt, and consistent results/status in step 4. TC275's
production scheduler has a separate host test with mocked hardware:
`python3 tc275_car/test/host/test_calib_store.py`.

Motor checks inspect the transmitted 0x71 bytes for all A/B/C/D channels in both
directions, plus zero-duty release frames and non-default position layouts.

Navigation integration check (repack first):

```sh
python3 esp32c6_car/tools/build_assets.py esp32c6_car/assets_src esp32c6_car/build/assets.bin
node esp32c6_car/test/web/navigation-ui.cjs
```

This test reads the actual packed bundle and the firmware's registered asset
routes, then taps the driving link in a 375 × 667 touch browser. It checks
stopping an active jog and navigation with JavaScript disabled. Calibration
styles ship through `/style.css`, so existing firmware serves every resource.

Battery display regression check:

```sh
node esp32c6_car/test/web/battery-ui.cjs
```

It feeds the actual telemetry handler with virtual 50 Hz measurements. It checks
small jitter, isolated low spikes, sustained declines followed, short charge
blips rejected, sustained charge recovery followed (voltage 50 mV rise
deadband, percent +2 % for 10 s stepwise back to 100 %), a vehicle restart
(uptime regression) resetting both display latches, zero measurements,
fresh-page initialization, and unchanged fault presentation and raw
low-battery warning colors. Percent checks cover 1% boundary jitter, isolated
spikes, sustained decline confirmation, gaps, invalid measurements, valid 0%,
and page reload.

No Node/Playwright on the host? A macOS JavaScriptCore variant drives the same
scenarios against the same app.js (no DOM, `$`/`Date.now` stubbed only):

```sh
osascript -l JavaScript esp32c6_car/test/web/battery-ui-jxa.js
```
