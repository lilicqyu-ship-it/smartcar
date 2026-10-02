/*
 * test_vision.c - host self-test for contracts/vision/vision.h builders.
 * Pins the exact wire strings both ends must agree on (remote sends them,
 * the gateway parses them byte-for-byte).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vision.h"

static int s_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    char buf[VISION_CMD_BUF_MAX];

    CHECK(vision_fmt_mode_cmd(buf, sizeof(buf), VISION_MODE_LINE, true) &&
          strcmp(buf, "{\"t\":\"vision_cmd\",\"mode\":\"line\",\"enable\":true}") == 0);
    CHECK(vision_fmt_mode_cmd(buf, sizeof(buf), VISION_MODE_OFF, false) &&
          strcmp(buf, "{\"t\":\"vision_cmd\",\"mode\":\"off\",\"enable\":false}") == 0);

    CHECK(vision_fmt_drive_mode(buf, sizeof(buf), VISION_DRIVE_ASSIST) &&
          strcmp(buf, "{\"t\":\"drive_mode\",\"mode\":\"assist\"}") == 0);

    CHECK(vision_fmt_cam_profile(buf, sizeof(buf), CAM_PROFILE_REMOTE) &&
          strcmp(buf, "{\"t\":\"cam_cmd\",\"op\":\"profile\",\"name\":\"REMOTE_PREVIEW\"}") == 0);

    /* mode string <-> index roundtrip over the whole enum */
    for (int i = 0; i < VISION_MODE_IDX_COUNT; i++) {
        CHECK(vision_mode_from_str(vision_mode_str(i)) == i);
    }
    CHECK(vision_mode_from_str("bogus") == -1);
    CHECK(vision_mode_from_str(NULL) == -1);
    CHECK(vision_drive_mode_from_str("auto") == 2);
    CHECK(vision_drive_mode_from_str("nope") == -1);

    /* buffer too small -> builder reports failure, never a truncated send */
    char tiny[8];
    CHECK(vision_fmt_drive_mode(tiny, sizeof(tiny), VISION_DRIVE_ASSIST) == 0);

    if (s_fail != 0) {
        printf("vision: %d FAILED\n", s_fail);
        return EXIT_FAILURE;
    }
    printf("vision: all checks passed\n");
    return EXIT_SUCCESS;
}
