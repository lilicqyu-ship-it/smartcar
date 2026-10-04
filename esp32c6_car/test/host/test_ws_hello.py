#!/usr/bin/env python3
"""Reproduce tc:on before hello on an already-up vehicle link using production functions."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
text = (root / "components/c6_http/http_server.c").read_text(encoding="utf-8")
def function(signature):
    begin = text.index(signature)
    return text[begin:text.index("\n}", begin) + 2]
source = r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGI(tag,...) ((void)(tag), (void)fprintf(stderr,__VA_ARGS__))
typedef struct { int fd; } httpd_req_t;
typedef struct { unsigned hello_sent; } ws_session_t;
typedef enum {PAIR_IDLE,PAIR_OPEN,PAIR_CLAIMED} pair_state_t;
static const char *TAG="test";
static bool up;
static bool (*s_link_up_provider)(void);
static struct { char fw_ver[24]; } s_http;
static ws_session_t session;
static pair_state_t pair_state(void) {return PAIR_IDLE;}
static int ws_sess_ctrl_fd(void) {return 7;}
static esp_err_t ws_send_ctl(int fd,const char *json) {(void)fd;puts(json);return ESP_OK;}
static int httpd_req_to_sockfd(httpd_req_t *req) {return req->fd;}
static void ws_tighten_send_timeout(int fd) {(void)fd;}
static void ws_sess_set_ws(int fd) {
    (void)fd;
    /* Actual session-change callback broadcasts online BEFORE hello. */
    puts(up ? "{\"t\":\"tc\",\"on\":true}" : "{\"t\":\"tc\",\"on\":false}");
}
static ws_session_t *ws_sess_get(int fd) {(void)fd;return &session;}
static const char *esp_err_to_name(int rc) {(void)rc;return "OK";}
static bool current_link(void) {return up;}
'''
source += function("void http_set_link_provider(") + "\n"
source += function("static esp_err_t ws_send_hello(") + "\n"
source += function("static esp_err_t ws_post_handshake(") + "\n"
source += r'''
int main(int argc,char **argv) {
    httpd_req_t req={7};
    strcpy(s_http.fw_ver,"1.1.1");
    up=argc>1 && strcmp(argv[1],"up")==0;
    http_set_link_provider(argc>1 && strcmp(argv[1],"unwired")==0 ? NULL : current_link);
    ws_post_handshake(&req);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ws-hello-") as directory:
    work=Path(directory); c=work/"hello.c"; exe=work/"hello"
    c.write_text(source,encoding="utf-8")
    subprocess.run([os.environ.get("CC","gcc"),"-std=c99","-Wall","-Wextra","-Werror",str(c),"-o",str(exe)],check=True)
    for case in ("up","down","unwired"):
        result=subprocess.run([str(exe),case],check=True,capture_output=True,text=True)
        messages=[json.loads(line) for line in result.stdout.splitlines()]
        assert len(messages)==2 and messages[0]["t"]=="tc" and messages[1]["t"]=="hello"
        hello=messages[1]
        assert hello["tc"] == ("up" if case=="up" else "down")
        assert hello["role"]=="ctrl" and hello["ctrl"] is True
        # iOS applyHello assigns tcUp; it must not overwrite online with a placeholder.
        ios_up=messages[0]["on"]
        ios_up=hello["tc"]=="up"
        assert ios_up==(case=="up")
print("PASS: online-before-hello order, online/offline/unwired provider, iOS final status")
