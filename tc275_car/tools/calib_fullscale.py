#!/usr/bin/env python3
"""Bench helper for calibrating the wheel-speed constants (doc 34 SS8.2).

Physical ground truth (tape + stopwatch) must be MEASURED by the operator; this
tool never invents a value. It only (1) decides whether the encoder's mm/s
geometry is trustworthy, (2) computes the recommended wheelDiaMm / fullScaleMmS,
(3) emits the exact 0x73 REC_SET 12-byte body, and (4) refuses out-of-range or
inconsistent inputs. See doc 34 SS8 for the runbook and the red lines.

Run `python calib_fullscale.py --selftest` to exercise the logic offline.
Run `python calib_fullscale.py --help` for the CLI.
"""
import argparse
import json
import sys

# Ranges mirror mw/calib/calib_record.h CALIB_FULLSCALE_* / CALIB_WHEELDIA_*
FULLSCALE_MIN, FULLSCALE_MAX = 100, 5000
WHEELDIA_MIN, WHEELDIA_MAX = 30, 200
GEOM_TOL = 0.10      # |true/reported - 1| beyond this => geometry scaling off
SIDE_SPREAD = 0.10   # left/right true-vs-reported ratio disagreement => defect


class CalibError(Exception):
    pass


def i16le(v: int) -> bytes:
    if not (-32768 <= v <= 32767):
        raise CalibError(f"value {v} does not fit i16")
    return int(v).to_bytes(2, "little", signed=True)


def read_i16le(b: bytes, off: int) -> int:
    return int.from_bytes(b[off:off + 2], "little", signed=True)


def true_speed_mm_s(distance_mm: float, time_ms: float) -> float:
    if time_ms <= 0:
        raise CalibError("elapsed time must be > 0")
    return distance_mm * 1000.0 / time_ms


def build_rec_set_body(pos, invert, full_scale, wheel_dia) -> bytes:
    """0x73 body (after the op byte), 12 B:
    p[0..3]=pos u8, p[4..7]=invert i8, p[8..9]=fullScaleMmS i16 LE,
    p[10..11]=wheelDiaMm i16 LE. recSetDecode overwrites every field from this
    body, so pos/invert MUST be carried through unchanged from the 0x72 read."""
    if len(pos) != 4 or len(invert) != 4:
        raise CalibError("pos and invert must each be 4 entries")
    body = bytes(pos) + bytes((i & 0xFF) for i in invert) + i16le(full_scale) + i16le(wheel_dia)
    assert len(body) == 12
    return body


def parse_evt_rec(body: bytes):
    """Decode a 0x72->EVT 0x23 echo body (ver,src,pos*4,invert*4,fullScale i16,
    wheelDia i16,crcOk). fullScale sits at offset 10 here, NOT 8 like the SET."""
    return {
        "ver": body[0],
        "src": body[1],
        "pos": list(body[2:6]),
        "invert": [int.from_bytes(bytes([b]), "big", signed=True) for b in body[6:10]],
        "fullScaleMmS": read_i16le(body, 10),
        "wheelDiaMm": read_i16le(body, 12),
        "crcOk": body[14],
    }


def calibrate(current, left, right):
    """current: {wheelDiaMm, fullScaleMmS, pos[4], invert[4]}
    left/right: {distance_mm, time_ms, vMeas_mm_s} measured at full command.
    Returns a dict with the decision, recommended values and the wire body."""
    cur_wd = current["wheelDiaMm"]
    for rec in (left, right):
        if rec["vMeas_mm_s"] <= 0:
            raise CalibError("vMeas at full command must be > 0")

    lt = true_speed_mm_s(left["distance_mm"], left["time_ms"])
    rt = true_speed_mm_s(right["distance_mm"], right["time_ms"])
    ratio_l = lt / left["vMeas_mm_s"]
    ratio_r = rt / right["vMeas_mm_s"]

    # Left/right must agree on the geometry factor or one side's counting/traction
    # is off; a single global wheelDia cannot represent two different factors.
    spread = abs(ratio_l - ratio_r) / ((ratio_l + ratio_r) / 2.0)
    side_consistent = spread <= SIDE_SPREAD

    geometry_ok = all(abs(r - 1.0) <= GEOM_TOL for r in (ratio_l, ratio_r))
    ratio = (ratio_l + ratio_r) / 2.0

    # mm/s is linear in wheelDia (encoder.c: circ * meanCounts / countsPerRev),
    # so the reported->true correction rides wheelDiaMm within its range.
    if geometry_ok:
        wd_new = cur_wd
        wd_action = "keep"
    else:
        wd_new = int(round(cur_wd * ratio))
        if not (WHEELDIA_MIN <= wd_new <= WHEELDIA_MAX):
            wd_action = "DEFECT"
        elif not side_consistent:
            wd_action = "DEFECT"
        else:
            wd_action = "set"

    # fullScaleMmS := the true mm/s at 100% command (calib_record.h: "pct==1000
    # at this mm/s"). Take the faster side, fresh-battery run, so the pct domain
    # no longer clamps and the fusion mm/s cap equals real speed. Erring high only
    # over-brakes; erring low under-protects.
    true_at_full = max(lt, rt) if wd_action != "DEFECT" else max(
        left["vMeas_mm_s"] * ratio_l, right["vMeas_mm_s"] * ratio_r)
    # After a wheelDia correction the reported mm/s already tracks true; fullScale
    # should equal the true top speed regardless (it is a pct-domain reference).
    fs_new = int(round(true_at_full))

    notes = []
    if not side_consistent:
        notes.append(
            f"left/right geometry factors disagree ({ratio_l:.3f} vs "
            f"{ratio_r:.3f}, spread {spread:.1%}): per-side traction or a single-"
            "encoder decode fault. The firmware has ONE global wheelDia/fullScale, "
            "so this cannot be absorbed here - investigate the faster side.")
    if wd_action == "DEFECT":
        corrected = int(round(cur_wd * ratio))
        notes.append(
            f"wheelDia correction would need {corrected} mm (allowed "
            f"{WHEELDIA_MIN}-{WHEELDIA_MAX}). A ratio of {ratio:.3f} that "
            "wheelDia cannot express means the PPR/gear/both-wheel x2 counting "
            "assumption (encoder.h) is wrong - that is a DECODE/GEOMETRY DEFECT "
            "(red line: do not edit formulas/PPR/gear), not a calibration. "
            "Escalate; do not send 0x73 for speed.")

    result = {
        "v_true_left_mm_s": round(lt, 1),
        "v_true_right_mm_s": round(rt, 1),
        "geom_ratio_left": round(ratio_l, 3),
        "geom_ratio_right": round(ratio_r, 3),
        "side_consistent": side_consistent,
        "geometry_ok": geometry_ok,
        "wheelDiaMm": {"current": cur_wd, "recommended": wd_new, "action": wd_action},
        "fullScaleMmS": {
            "current": current["fullScaleMmS"],
            "recommended": fs_new,
            "reason": "true mm/s at 100% command (faster side)",
        },
        "notes": notes,
    }

    if wd_action == "DEFECT":
        result["block"] = "geometry defect branch: no speed parameters should be written"
        return result

    # Range-check the writes we are about to emit.
    if not (FULLSCALE_MIN <= fs_new <= FULLSCALE_MAX):
        raise CalibError(f"recommended fullScale {fs_new} outside [{FULLSCALE_MIN},{FULLSCALE_MAX}]")
    if not (WHEELDIA_MIN <= wd_new <= WHEELDIA_MAX):
        raise CalibError(f"recommended wheelDia {wd_new} outside [{WHEELDIA_MIN},{WHEELDIA_MAX}]")

    body = build_rec_set_body(current["pos"], current["invert"], fs_new, wd_new)
    result["rec_set_body_hex"] = body.hex(" ")
    result["rec_set_body_cmd"] = "73 " + body.hex(" ")
    return result


def _selftest():
    # Byte layout: fullScale=1400 -> 0x0578 LE [0x78,0x05]; wheelDia=48 -> [0x30,0x00]
    body = build_rec_set_body([0, 1, 2, 3], [1, 1, -1, -1], 1400, 48)
    assert body[8:10] == b"\x78\x05", body[8:10].hex()
    assert body[10:12] == b"\x30\x00"
    assert list(body[0:4]) == [0, 1, 2, 3]
    assert body[4] == 0x01 and body[6] == 0xFF  # -1 as i8
    assert read_i16le(body, 8) == 1400 and read_i16le(body, 10) == 48

    # EVT 0x23 parse (offsets differ: fullScale at 10):
    evt = bytes([1, 2, 0, 1, 2, 3, 1, 1, 0xFF, 0xFF]) + i16le(1400) + i16le(48) + bytes([1])
    p = parse_evt_rec(evt)
    assert p["fullScaleMmS"] == 1400 and p["wheelDiaMm"] == 48
    assert p["invert"] == [1, 1, -1, -1]

    cur = {"wheelDiaMm": 48, "fullScaleMmS": 1000, "pos": [0, 1, 2, 3], "invert": [1, 1, -1, -1]}

    # 1) Geometry trusted, true top ~1367/1200: fullScale -> 1367, wheelDia kept.
    r = calibrate(cur, {"distance_mm": 4000, "time_ms": 4000 * 1000 / 1367, "vMeas_mm_s": 1367},
                       {"distance_mm": 4000, "time_ms": 4000 * 1000 / 1200, "vMeas_mm_s": 1200})
    assert r["geometry_ok"] and r["wheelDiaMm"]["action"] == "keep"
    assert r["fullScaleMmS"]["recommended"] == 1367, r["fullScaleMmS"]

    # 2) Reported over-reads 1.5x but wheelDia can absorb it (48->32): geometry fix.
    r = calibrate(cur, {"distance_mm": 3000, "time_ms": 3000 * 1000 / 800, "vMeas_mm_s": 1200},
                       {"distance_mm": 3000, "time_ms": 3000 * 1000 / 790, "vMeas_mm_s": 1185})
    assert not r["geometry_ok"] and r["wheelDiaMm"]["action"] == "set", r["wheelDiaMm"]
    assert WHEELDIA_MIN <= r["wheelDiaMm"]["recommended"] <= WHEELDIA_MAX

    # 3) 2x over-read -> wheelDia would need 24 (<30): DEFECT branch, no write.
    r = calibrate(cur, {"distance_mm": 2000, "time_ms": 2000 * 1000 / 700, "vMeas_mm_s": 1400},
                       {"distance_mm": 2000, "time_ms": 2000 * 1000 / 690, "vMeas_mm_s": 1380})
    assert r["wheelDiaMm"]["action"] == "DEFECT" and "block" in r
    assert "rec_set_body_hex" not in r

    # 4) Sides disagree on factor -> DEFECT even if a single value could fit.
    r = calibrate(cur, {"distance_mm": 4000, "time_ms": 4000 * 1000 / 1000, "vMeas_mm_s": 1367},
                       {"distance_mm": 4000, "time_ms": 4000 * 1000 / 900, "vMeas_mm_s": 1000})
    assert not r["side_consistent"]

    # 5) Range guard: geometry fine but true top speed > 5000 raises on fullScale.
    try:
        calibrate(cur, {"distance_mm": 9000, "time_ms": 1000, "vMeas_mm_s": 9000},
                       {"distance_mm": 9000, "time_ms": 1000, "vMeas_mm_s": 9000})
        raise AssertionError("expected CalibError for out-of-range fullScale")
    except CalibError:
        pass

    # 6) time_ms<=0 raises.
    try:
        calibrate(cur, {"distance_mm": 100, "time_ms": 0, "vMeas_mm_s": 500},
                       {"distance_mm": 100, "time_ms": 1000, "vMeas_mm_s": 500})
        raise AssertionError("expected CalibError for zero time")
    except CalibError:
        pass

    print("calib_fullscale self-test: PASS")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compute wheel-speed calibration + 0x73 body")
    ap.add_argument("--selftest", action="store_true", help="run offline logic tests")
    ap.add_argument("--current", required=False, help="JSON: wheelDiaMm,fullScaleMmS,pos[4],invert[4]")
    ap.add_argument("--left", required=False, help="JSON run: distance_mm,time_ms,vMeas_mm_s")
    ap.add_argument("--right", required=False, help="JSON run: distance_mm,time_ms,vMeas_mm_s")
    a = ap.parse_args(argv)
    if a.selftest:
        _selftest()
        return 0
    if not (a.current and a.left and a.right):
        ap.error("--current, --left and --right are required (or use --selftest)")
    cur = json.loads(a.current)
    for k in ("wheelDiaMm", "fullScaleMmS", "pos", "invert"):
        if k not in cur:
            raise CalibError(f"current record missing '{k}' (read it with 0x72 REC_GET)")
    res = calibrate(cur, json.loads(a.left), json.loads(a.right))
    print(json.dumps(res, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
