#!/usr/bin/env python3
"""Validate production diagnostic JSON and embedded JS after local sensor removal."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
header=(root/"main/include/app_state.h").read_text(encoding="utf-8")
struct=re.search(r"typedef struct\s*\{.*?\} app_diag_t;",header,re.S).group(0)
source=(root/"main/app_state.c").read_text(encoding="utf-8")
begin=source.index("void app_diag_render(")
function=source[begin:source.index("\n}",begin)+2]
harness=r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
'''+struct+r'''
#define PAIR_CLAIMED 2
#define PAIR_OPEN 1
static int pair_state(void) {return PAIR_CLAIMED;}
static const char *app_state_name(void) {return "online";}
static struct {const char *version;} desc={"1.1.2"};
static struct {const char *label;} partition={"ota_0"};
#define esp_app_get_description() (&desc)
#define esp_ota_get_running_partition() (&partition)
static void app_diag_snapshot(app_diag_t *d) {
    memset(d,0,sizeof(*d));d->link_up=true;d->frames_rx=123;
    d->frames_tx=456;d->clock_hz=1000000;d->uptime_s=42;d->heap_min=100000;
}
'''+function+r'''
int main(void) {char json[768];app_diag_render(json,sizeof(json));puts(json);return 0;}
'''
http=(root/"components/c6_http/http_server.c").read_text(encoding="utf-8")
page=http[http.index("static const char DIAG_PAGE[] ="):http.index("static esp_err_t diag_page_handler(")]
html="".join(json.loads(line.strip().rstrip(";")) for line in page.splitlines() if line.strip().startswith(chr(34)))
assert "ADXL345" not in html and "j.imu" not in html
js=re.search(r"<script>(.*?)</script>",html,re.S).group(1)
with tempfile.TemporaryDirectory(prefix="c6-diag-") as directory:
    work=Path(directory);c=work/"diag.c";exe=work/"diag"
    c.write_text(harness,encoding="utf-8")
    subprocess.run([os.environ.get("CC","gcc"),"-std=c99","-Wall","-Wextra","-Werror",str(c),"-o",str(exe)],check=True)
    result=json.loads(subprocess.check_output([str(exe)],text=True))
    assert "imu" not in result and result["link"]["up"] is True and result["link"]["rx"]==123
    assert result["uptime_s"]==42 and result["pair"]=="claimed"
    node=shutil.which("node")
    if node:
        script=work/"diag.js";script.write_text(js,encoding="utf-8")
        subprocess.run([node,"--check",str(script)],check=True)
print("PASS: production /api/diag JSON, embedded diagnostic JS, no local ADXL345 UI")
