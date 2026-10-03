/* SmartDrive control page - speaks proto v2 over WS binary (SDD 6) */
"use strict";

/* ---- proto v2 constants (mirror components/c6_proto) ---- */
const P_SYNC1 = 0xAA, P_SYNC2 = 0x55, P_VER = 0x02;
const CMD = { DRIVE:0x50, TELEMETRY:0x41 };
const TELEMETRY_LEN = 38;

/* CRC16-CCITT-FALSE, check("123456789")==0x29B1 */
function crc16(buf) {
  let crc = 0xFFFF;
  for (const b of buf) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    crc &= 0xFFFF;
  }
  return crc;
}

let seq = 0;
function buildFrame(cmd, data) {
  const head = [P_SYNC1, P_SYNC2, P_VER, cmd, (++seq) & 0xFF, data.length];
  const body = head.concat(Array.from(data));
  const c = crc16(body);
  return new Uint8Array(body.concat([(c >> 8) & 0xFF, c & 0xFF]));
}

function u16le(v) { return [v & 0xFF, (v >> 8) & 0xFF]; }
function i16le(v) { v = Math.max(-32768, Math.min(32767, v|0)); return u16le(v & 0xFFFF); }

/* ---- state ---- */
const $ = (id) => document.getElementById(id);
/* state line text + tone (CSS: .state[data-tone=ok|warn|bad|dim]) */
function setState(msg, tone) { $("state").textContent = msg; $("state").dataset.tone = tone; }
const state = {
  /* localStorage: token survives new tabs / browser restarts - a per-tab
   * (sessionStorage) token silently demoted every freshly opened tab to
   * spectator, which read as "everything green but nothing drives" (09-28) */
  token: localStorage.getItem("sd_token") || sessionStorage.getItem("sd_token")
         || new URLSearchParams(location.search).get("token") || "",
  ws: null, ctrl: false, tc: false, uploading: false,
  driveTimer: null, joyV: 0, joyW: 0,
};

function setRole(txt, tone) {
  const p = $("role_pill");
  $("role_txt").textContent = txt;
  p.className = "pill " + tone;
  updateControlHint();
}

/* ---- WebSocket ---- */
function wsUrl() {
  const p = (location.protocol === "https:") ? "wss://" : "ws://";
  return p + location.host + "/ws" + (state.token ? ("?token=" + state.token) : "");
}
let wsBackoff = 1000;
let wsLastRx = 0, wsLastPing = 0;
let reconnectTimer = null;
function connect() {
  clearTimeout(reconnectTimer);
  if (state.ws && state.ws.readyState < 2) return;
  const socket = new WebSocket(wsUrl());
  state.ws = socket;
  state.ws.binaryType = "arraybuffer";
  state.ws.onopen = () => {
    if (state.ws !== socket) return;
    wsBackoff = 1000;
    wsLastRx = Date.now();
    $("dot_ws").className = "dot on";
    sendDrive(0, 0);
    updateControlHint();
  };
  state.ws.onclose = () => {
    if (state.ws !== socket) return;
    state.tc = false;
    $("dot_tc").className = "dot off";
    setState("链路断开，正在重连", "warn");
    $("dot_ws").className = "dot off"; state.ctrl = false;
    setRole("重连中", "bad");
    // fixed 1 s retries churn sockets on the device while it is struggling;
    // back off so recovery is not fought by the page itself
    reconnectTimer = setTimeout(connect, wsBackoff);
    wsBackoff = Math.min(wsBackoff * 2, 3000);
  };
  state.ws.onmessage = (ev) => {
    if (state.ws !== socket) return;
    wsLastRx = Date.now();
    if (typeof ev.data === "string") {
      try { onCtl(JSON.parse(ev.data)); } catch (e) { setState("收到无效状态数据", "warn"); }
      return;
    }
    const d = new Uint8Array(ev.data);
    if (d.length >= 8 && d[0] === P_SYNC1 && d[1] === P_SYNC2 && d[3] === CMD.TELEMETRY) {
      onTelemetry(d.subarray(6, 6 + d[5]));
    }
  };
}

/* liveness: a healthy chain pushes telemetry at 50 Hz, so silence means the
 * socket is half-dead (iOS backgrounding kills it without a close event and
 * the tab read "green but unresponsive").  probe at 3 s, re-connect at 10 s */
setInterval(() => {
  if (!state.ws || state.ws.readyState !== 1) return;
  const now = Date.now();
  if (now - wsLastRx > 10000) {
    state.ws.close();                       /* onclose reconnects */
  } else if (now - wsLastRx > 3000 && now - wsLastPing > 3000) {
    wsLastPing = now;
    try { state.ws.send('{"t":"ping"}'); } catch (e) { /* racing a close */ }
  }
}, 1000);

/* iOS suspends background tabs and their sockets die silently: on return,
 * skip the backoff entirely and resync now, and zero the joystick so a
 * stale pointer deflection cannot linger as a drive command */
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState !== "visible") return;
  joyEnd();
  if (!state.ws || state.ws.readyState >= 2) {
    wsBackoff = 0;
    connect();
  } else if (Date.now() - wsLastRx > 4000) {
    wsLastRx = 0;                           /* force the watchdog to reconnect */
  }
});

let errT = 0;                     /* pending "错误:" auto-clear timer */
function onCtl(m) {
  if (m.t === "hello") {    state.ctrl = (m.role === "ctrl");
    $("ver").textContent = "fw " + m.ver;
    if (state.ctrl) setRole("控制器", "ctrl");
    else setRole("旁观者", "spec");
  } else if (m.t === "tc") {
    state.tc = !!m.on;
    $("dot_tc").className = "dot " + (m.on ? "on" : "off");
    setState(m.on ? "待命" : "车端未连接", m.on ? "ok" : "dim");
    if (!state.tc) joyEnd();
    updateControlHint();
  } else if (m.t === "otastatus") {
    $("ota_progress").textContent = "OTA " + m.pct + "%";
    $("ota_bar").style.width = Math.max(0, Math.min(100, m.pct)) + "%";
  } else if (m.t === "otaswap") {
    $("ota_progress").textContent = "TC275 切槽重启...";
    $("ota_bar").style.width = "100%";
  } else if (m.t === "otaerror") {
    $("ota_progress").textContent = "OTA 失败 " + m.e;
    $("ota_bar").style.width = "0%";
  } else if (m.t === "err") {
    if (m.e === "auth") {
      /* role gate rejected a command: the C6 no longer honours this
       * session (reboot / re-pair elsewhere) - latched, not auto-cleared */
      state.ctrl = false;
      setRole("无控制权", "bad");
      setState("控制权失效 - 请重新配对", "bad");
      return;
    }
    // transient hiccup (e.g. command queue momentarily full): show it, then
    // let the state line recover instead of latching a stale error forever
    setState("错误: " + m.e, "warn");
    clearTimeout(errT);
    errT = setTimeout(() => {
      setState(state.tc ? "待命" : "车端未连接", state.tc ? "ok" : "dim");
    }, 3000);
  }
}

/* ---- telemetry 0x41 (LE fields, SDD 6.3) ---- */
function onTelemetry(p) {
  if (p.length < TELEMETRY_LEN) return;
  const dv = (o) => p[o] | (p[o+1] << 8);
  const dl = (o) => (dv(o) << 16) >> 16;                       // sign
  const d32 = (o) => (p[o] | (p[o+1] << 8) | (p[o+2] << 16) | (p[o+3] << 24)) >>> 0;
  const tl = dl(11), tr = dl(13), ml = dl(15), mr = dl(17);
  const pct = p[21], fault = dv(9);
  renderSpeed(ml, mr);
  setBar("bar_lt", tl, 800); setBar("bar_lm", ml, 800);
  setBar("bar_rt", tr, 800); setBar("bar_rm", mr, 800);
  $("v_lt").textContent = ml; $("v_rt").textContent = mr;
  renderBatteryPercent(pct, dv(19));
  renderBatteryVoltage(dv(19));
  $("batt_pill").style.color = pct <= 10 ? "var(--bad)" : pct <= 20 ? "var(--warn)" : "var(--ok)";
  $("odo").textContent = (d32(26) / 1000).toFixed(1);
  $("rtt").textContent = "rtt " + dv(30) + "ms";
  if (fault) setState("故障 0x" + fault.toString(16), "bad");
  else setState(state.tc ? "待命" : "车端未连接", state.tc ? "ok" : "dim");
}
function setBar(id, v, full) {
  const el = $(id); const w = Math.min(50, Math.abs(v) / full * 50);
  el.classList.toggle("neg", v < 0);
  el.style.width = w + "%";
  if (v >= 0) el.style.left = "50%"; else el.style.left = "auto";
}

/* Battery display only: smooth noise, limit DOM updates to 2 Hz and latch
 * downward changes. The latch lasts for this document, including WS reconnects;
 * loading a new page starts from its first valid measurement. Safety, including
 * the battery warning color, uses the original telemetry, never display values. */
const BATT_DISPLAY_INTERVAL_MS = 500;
const BATT_DISPLAY_TAU_MS = 500;
const BATT_DISPLAY_DEADBAND_MV = 20;
const batteryDisplay = { samples: [], filteredMv: null, shownMv: null, sampleTs: 0, displayTs: 0 };
function renderBatteryVoltage(mv) {
  if (mv <= 0) return; // Uninitialized/absent measurement must not latch 0 V.
  const now = Date.now();
  batteryDisplay.samples.push(mv);
  if (batteryDisplay.samples.length > 5) batteryDisplay.samples.shift();
  const sorted = batteryDisplay.samples.slice().sort((a, b) => a - b);
  const medianMv = sorted[Math.floor(sorted.length / 2)];
  if (batteryDisplay.filteredMv === null) {
    batteryDisplay.filteredMv = medianMv;
  } else {
    const dt = Math.max(0, Math.min(1000, now - batteryDisplay.sampleTs));
    batteryDisplay.filteredMv += (medianMv - batteryDisplay.filteredMv) *
      (1 - Math.exp(-dt / BATT_DISPLAY_TAU_MS));
  }
  batteryDisplay.sampleTs = now;
  if (batteryDisplay.shownMv !== null && now - batteryDisplay.displayTs < BATT_DISPLAY_INTERVAL_MS) return;
  batteryDisplay.displayTs = now;
  const roundedMv = Math.round(batteryDisplay.filteredMv / 10) * 10;
  if (batteryDisplay.shownMv === null || roundedMv <= batteryDisplay.shownMv - BATT_DISPLAY_DEADBAND_MV) {
    batteryDisplay.shownMv = roundedMv;
    $("batt_v").textContent = (roundedMv / 1000).toFixed(2) + "V";
  }
}

/* Percent steps are coarse: require a sustained lower median for 1.5 s,
 * rather than latching a single noisy 1% dip for the rest of the session. */
const BATT_PERCENT_CONFIRM_MS = 1500;
const batteryPercentDisplay = { samples: [], shown: null, candidate: null, since: 0, sampleTs: 0 };
function renderBatteryPercent(pct, mv) {
  if (mv <= 0 || pct < 0 || pct > 100) return; // 0% is valid when voltage exists.
  const now = Date.now();
  if (batteryPercentDisplay.sampleTs && now - batteryPercentDisplay.sampleTs > 1000) {
    batteryPercentDisplay.samples = [];
    batteryPercentDisplay.candidate = null;
  }
  batteryPercentDisplay.sampleTs = now;
  batteryPercentDisplay.samples.push(pct);
  if (batteryPercentDisplay.samples.length > 5) batteryPercentDisplay.samples.shift();
  const sorted = batteryPercentDisplay.samples.slice().sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  if (batteryPercentDisplay.shown === null) {
    batteryPercentDisplay.shown = median;
    $("batt_pct").textContent = median + "%";
    return;
  }
  if (median >= batteryPercentDisplay.shown) {
    batteryPercentDisplay.candidate = null;
    return;
  }
  if (batteryPercentDisplay.candidate === null) {
    batteryPercentDisplay.candidate = median;
    batteryPercentDisplay.since = now;
  } else {
    // Use the highest lower estimate in the window, so a short deep dip cannot
    // set a falsely low permanent display while the readings fluctuate below it.
    batteryPercentDisplay.candidate = Math.max(batteryPercentDisplay.candidate, median);
  }
  if (now - batteryPercentDisplay.since >= BATT_PERCENT_CONFIRM_MS) {
    batteryPercentDisplay.shown = batteryPercentDisplay.candidate;
    batteryPercentDisplay.candidate = null;
    $("batt_pct").textContent = batteryPercentDisplay.shown + "%";
  }
}

/* ---- speedometer: body speed = mean of measured wheel speeds (mm/s) ----
 * signed average -> in-place rotation reads 0; km/h with 1 decimal because
 * full joystick deflection is only 600 mm/s = 2.2 km/h.  The display value is
 * a dt-aware EMA (tau 150 ms): one 0.1 km/h digit is ~28 mm/s, so raw 50 Hz
 * readings flicker the digit constantly and the direction line flaps near
 * zero.  Snap instead of ramp on the first frame, after a transmission gap,
 * and whenever crossing in/out of rest. */
const STOP_MM_S = 30;                          /* < 0.1 km/h counts as stopped */
const SPEED_TAU_MS = 150;                      /* EMA time constant */
let teleTs = 0, dispV = 0, dispSeeded = false;
const speedCache = { v: "", d: "" };
function renderSpeed(ml, mr) {
  const now = Date.now();
  const dt = teleTs ? Math.min(1000, now - teleTs) : 0;
  teleTs = now;
  const v = (ml + mr) / 2;
  if (!dispSeeded || dt > 400 || Math.abs(v) < STOP_MM_S) {
    dispV = v; dispSeeded = true;
  } else {
    dispV += (v - dispV) * (1 - Math.exp(-dt / SPEED_TAU_MS));
  }
  const stopped = Math.abs(dispV) < STOP_MM_S;
  const sv = stopped ? "0.0" : (Math.abs(dispV) * 0.0036).toFixed(1);
  const dir = stopped ? "" : (dispV > 0 ? "▲ 前进" : "▼ 倒车");
  if (sv !== speedCache.v) { $("speed_val").textContent = sv; speedCache.v = sv; }
  if (dir !== speedCache.d) {
    $("speed_dir").textContent = dir || "\u00a0";
    $("speed_dir").className = stopped ? "" : (dispV > 0 ? "fwd" : "rev");
    speedCache.d = dir;
  }
  $("speed_val").classList.remove("stale");
}
/* staleness guard: 50 Hz nominal, so 1 s without telemetry means the chain
 * (TC275 -> SPI -> C6 -> WS) is broken somewhere - freeze the number as "--"
 * instead of letting a stale speed keep looking live */
setInterval(() => {
  if (Date.now() - teleTs <= 1000 || teleTs === 0) return;
  if (speedCache.v !== "--") {
    speedCache.v = "--"; speedCache.d = "";
    dispSeeded = false;                        /* resync snaps on next frame */
    $("speed_val").textContent = "--";
    $("speed_val").classList.add("stale");
    $("speed_dir").textContent = "\u00a0";
    $("speed_dir").className = "";
  }
}, 500);

/* ---- drive: joystick -> DRIVE 0x50 {v:i16, w:i16} at 30 Hz ---- */
function sendDrive(v, w) {
  if (state.ws && state.ws.readyState === 1 && state.ctrl) {
    state.ws.send(buildFrame(CMD.DRIVE, new Uint8Array([...i16le(v), ...i16le(w)])));
  }
}
const joy = $("joy"), knob = $("knob");
function joyMove(ev) {
  const r = joy.getBoundingClientRect();
  const t = ev.touches ? ev.touches[0] : ev;
  let dx = t.clientX - (r.left + r.width / 2), dy = t.clientY - (r.top + r.height / 2);
  const max = r.width / 2 - knob.offsetWidth / 2 - 2, len = Math.hypot(dx, dy);
  if (len > max) { dx *= max / len; dy *= max / len; }
  knob.style.transform = `translate(${dx}px,${dy}px)`;
  state.joyV = Math.round(-dy / max * 600);       // mm/s, up = forward
  state.joyW = Math.round(-dx / max * 300);       // deg/s
}
function joyEnd() {
  knob.style.transform = ""; state.joyV = 0; state.joyW = 0;
  joy.classList.remove("active");
  activePointer = null;
}
let activePointer = null;
function updateControlHint() {
  const ready = !!(state.ws && state.ws.readyState === 1 && state.ctrl && state.tc && !state.uploading);
  $("joy").setAttribute("aria-disabled", String(!ready));
  $("control_hint").textContent = !state.ws || state.ws.readyState !== 1
    ? "链路未连接，正在自动重连…" : !state.ctrl
    ? "当前为观察模式。按车侧键 3 秒，再点击配对。" : !state.tc
    ? "已取得控制权，等待车端上线。" : state.uploading
    ? "固件上传中，请等待完成。" : "已就绪，按住摇杆即可驾驶。";
  if (!ready) joyEnd();
}
joy.addEventListener("pointerdown", (ev) => {
  if (activePointer !== null || state.uploading || !state.ctrl || !state.tc || !state.ws || state.ws.readyState !== 1 || ev.button !== 0) return;
  activePointer = ev.pointerId;
  joy.setPointerCapture(ev.pointerId);
  joy.classList.add("active");
  joyMove(ev);
});
joy.addEventListener("pointermove", (ev) => {
  if (ev.pointerId === activePointer) joyMove(ev);
});
function releaseJoystick(ev) {
  if (ev && ev.pointerId !== activePointer) return;
  activePointer = null;
  joyEnd();
  sendDrive(0, 0);
}
["pointerup", "pointercancel", "lostpointercapture"].forEach((event) => joy.addEventListener(event, releaseJoystick));
window.addEventListener("blur", () => releaseJoystick());
document.addEventListener("visibilitychange", () => {
  if (document.hidden) releaseJoystick();
});
setInterval(() => sendDrive(state.joyV, state.joyW), 33);   // 30 Hz, doubles as heartbeat

/* ---- pairing ---- */
$("btn_pair").onclick = async () => {
  releaseJoystick();
  $("btn_pair").disabled = true;
  try {
    const r = await fetch("/api/pair", { method: "POST" });
    const j = await r.json();
    if (j.ok && j.token) {
      state.token = j.token;
      localStorage.setItem("sd_token", j.token);
      sessionStorage.setItem("sd_token", j.token);
      setState("已配对 (控制端)", "ok");
      if (state.ws) state.ws.close();
    } else {
      setState("配对失败: " + (j.hint || j.e || "先按车侧键3秒"), "warn");
    }
  } catch (e) { setState("配对未完成，请检查设备连接后重试", "warn"); }
  finally { $("btn_pair").disabled = false; }
};

/* ---- OTA upload (control token required) ---- */
async function upload(file, uri, btn) {
  if (!file) { $("ota_progress").textContent = "请先选择固件文件"; return; }
  if (state.uploading) return;
  if (!state.ctrl) { $("ota_progress").textContent = "请先配对取得控制权"; return; }
  releaseJoystick();
  state.uploading = true;
  ["btn_ota_c6", "btn_ota_tc", "file_c6", "file_tc", "btn_pair"].forEach((id) => { $(id).disabled = true; });
  const originalLabel = btn.textContent;
  btn.textContent = "上传中…";
  $("ota_progress").textContent = "正在上传固件，请保持设备连接…";
  $("ota_bar").style.width = "0%";
  updateControlHint();
  try {
    const r = await fetch(uri + (state.token ? ("?token=" + state.token) : ""),
      { method: "POST", body: file });
    const j = await r.json();
    $("ota_progress").textContent = j.ok ? "完成，设备将重启" : "失败 " + j.e;
    $("ota_bar").style.width = j.ok ? "100%" : "0%";
  } catch (e) { $("ota_progress").textContent = "上传中断"; $("ota_bar").style.width = "0%"; }
  finally {
    state.uploading = false;
    ["btn_ota_c6", "btn_ota_tc", "file_c6", "file_tc", "btn_pair"].forEach((id) => { $(id).disabled = false; });
    btn.textContent = originalLabel;
    updateControlHint();
  }
}
$("btn_ota_c6").onclick = () => upload($("file_c6").files[0], "/ota/c6", $("btn_ota_c6"));
$("btn_ota_tc").onclick = () => upload($("file_tc").files[0], "/ota/tc275", $("btn_ota_tc"));

/* picked firmware name inside the styled drop label */
[["file_c6", "lbl_c6", "选择 C6 固件 (.bin)"],
 ["file_tc", "lbl_tc", "选择 TC275 固件 (.bin)"]].forEach(([f, l, d]) => {
  $(f).addEventListener("change", () => { $(l).textContent = $(f).files[0] ? $(f).files[0].name : d; });
});

/* ---- stop ---- */
$("btn_stop").onclick = () => { releaseJoystick(); setState("摇杆已归零", "warn"); };

fetch("/api/health").then((r) => r.json()).then((j) => {
  $("ssid").textContent = "SmartDrive";
  $("ver").textContent = "fw " + j.ver;
}).catch(() => {});
connect();
