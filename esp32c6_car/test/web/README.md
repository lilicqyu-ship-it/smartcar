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
