#include "app/sensor_stream.h"

#include "bsp/tof.h"
#include "mw/sf/sf_frame.h"
#include "mw/xcore/xcore.h"

#include <string.h>

static void u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void u32le(uint8_t *p, uint32_t v)
{
    u16le(p, (uint16_t)v);
    u16le(p + 2, (uint16_t)(v >> 16));
}

/* Diagnostics: drop-on-full is the intended behaviour (the data ring exists
 * so backpressure here can never touch the driving paths). */
static void push(uint8 cid, const uint8 *payload, uint8 len)
{
    XcoreEvtFrame frame;

    frame.type = SF_TYPE_EVT;
    frame.cid = cid;
    frame.len = len;
    memcpy(frame.payload, payload, len);
    (void)XCORE_dataEvtPush(&frame);
}

static void sendImu(void)
{
    XcoreImu imu;
    uint8 p[SENSORSTREAM_IMU_WIRE_LEN];
    unsigned i;

    XCORE_imuRead(&imu);
    memset(p, 0, sizeof(p));
    u32le(p + 0, imu.seq);
    u32le(p + 4, imu.stampMs);
    u16le(p + 8, (uint16_t)imu.tempCentiC);
    for (i = 0u; i < 3u; i++)
    {
        u16le(p + 10 + (i * 2u), (uint16_t)imu.accMilliG[i]);
    }
    for (i = 0u; i < 3u; i++)
    {
        u32le(p + 16 + (i * 4u), (uint32_t)imu.gyroMilliDps[i]);
    }
    push(SENSORSTREAM_EVT_CID_IMU, p, (uint8)sizeof(p));
}

/* Emit one 64-zone frame as 3 fragments. valid/nearest are recomputed from
 * the driver status here rather than taken from FusionOutput: the map on the
 * phone must agree with the pixels it renders, while the fusion envelope
 * applies its own trust logic on top. The header (valid/nearest/mode) is
 * identical in all three fragments, so the phone can consume fragments
 * independently of arrival order. */
static void sendTofZones(const FusionTof *tof, uint8_t mode)
{
    uint32_t zone;
    uint16_t nearest = 0u;
    uint8_t valid = 0u;
    uint8_t haveNearest = 0u;
    uint8 frag;

    for (zone = 0u; zone < FUSION_MAX_ZONES; zone++)
    {
        int32_t mm;

        if (!tof->alive || !TOF_targetStatusOk(tof->status[zone]))
        {
            continue;
        }
        mm = tof->distanceMm[zone];
        if (mm < 0)
        {
            mm = 0;
        }
        valid++;
        /* haveNearest instead of a 0 sentinel: a genuine 0 mm nearest must
         * not let later zones overwrite it. */
        if (!haveNearest || ((uint16_t)mm < nearest))
        {
            nearest = (uint16_t)mm;
            haveNearest = 1u;
        }
    }

    for (frag = 0u; frag < SENSORSTREAM_TOF_FRAG_COUNT; frag++)
    {
        uint8 p[SENSORSTREAM_TOF_WIRE_LEN];
        unsigned i;
        unsigned first = (unsigned)frag * SENSORSTREAM_TOF_FRAG_ZONES;
        unsigned count = SENSORSTREAM_TOF_FRAG_ZONES;

        if (first >= FUSION_MAX_ZONES)
        {
            break;
        }
        if (first + count > FUSION_MAX_ZONES)
        {
            count = FUSION_MAX_ZONES - first;
        }
        memset(p, 0, sizeof(p));
        u16le(p + 0, (uint16_t)tof->seq);
        p[2] = frag;
        p[3] = mode;
        p[4] = valid;
        u16le(p + 5, nearest);
        for (i = 0u; i < count; i++)
        {
            int32_t mm = tof->distanceMm[first + i];

            if (!tof->alive || !TOF_targetStatusOk(tof->status[first + i]))
            {
                p[SENSORSTREAM_TOF_HDR_LEN + i] = SENSORSTREAM_ZONE_INVALID;
            }
            else
            {
                if (mm < 0)
                {
                    mm = 0;
                }
                mm /= 16; /* 0..4080 mm in the u8 cell, see sensor_stream.h */
                if (mm > 255)
                {
                    mm = 255;
                }
                p[SENSORSTREAM_TOF_HDR_LEN + i] = (uint8_t)mm;
            }
        }
        push(SENSORSTREAM_EVT_CID_TOF_ZONES, p, (uint8)sizeof(p));
    }
}

void SENSORSTREAM_tick(uint32_t nowMs, uint8_t tofMode)
{
    static uint32_t s_lastImuMs;
    static uint8_t s_imuSeen;
    static uint32_t s_lastTofSeq;
    static uint8_t s_tofSeen;

    if (!s_imuSeen || ((uint32_t)(nowMs - s_lastImuMs) >= SENSORSTREAM_IMU_PERIOD_MS))
    {
        s_imuSeen = 1u;
        s_lastImuMs = nowMs;
        sendImu();
    }

    {
        FusionTof tof;

        XCORE_tofRead(&tof);
        if (!s_tofSeen || (tof.seq != s_lastTofSeq))
        {
            s_tofSeen = 1u;
            s_lastTofSeq = tof.seq;
            sendTofZones(&tof, tofMode);
        }
    }
}
