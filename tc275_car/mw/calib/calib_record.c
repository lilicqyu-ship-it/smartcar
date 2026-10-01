#include "mw/calib/calib_record.h"
#include "mw/sf/sf_frame.h"    /* SF_crc16: the one CRC16-CCITT-FALSE in this repo */

#include <string.h>

/* Blob offsets (doc 34 SS8.1) */
#define BLOB_MAGIC0   0u
#define BLOB_VER      4u
#define BLOB_SRC      5u
#define BLOB_POS      6u
#define BLOB_INVERT  10u
#define BLOB_FULL    14u
#define BLOB_DIA     16u
#define BLOB_CRC     18u

#define BLOB_CRC_LEN  18u        /* crc16 covers bytes 0..17 */

static const uint8_t g_magic[4] = {'S', 'D', 'C', '1'};

void CALIBREC_fillDefaults(CalibRecord *rec)
{
    static const uint8_t defaultPos[CALIB_REC_WHEELS] = {
        CALIB_POS_FRONT_LEFT,   /* motor A */
        CALIB_POS_REAR_LEFT,    /* motor B */
        CALIB_POS_REAR_RIGHT,   /* motor C */
        CALIB_POS_FRONT_RIGHT   /* motor D */
    };
    uint8_t i;

    rec->ver = CALIB_REC_VER;
    rec->src = CALIB_SRC_DEFAULT;
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        rec->pos[i]    = defaultPos[i];
        rec->invert[i] = 1;
    }
    rec->fullScaleMmS = CALIB_FULLSCALE_DEF;
    rec->wheelDiaMm   = CALIB_WHEELDIA_DEF;
}

uint8_t CALIBREC_paramsOk(const CalibRecord *rec)
{
    uint8_t i;

    if (rec == NULL)
    {
        return 0u;
    }
    if ((rec->fullScaleMmS < CALIB_FULLSCALE_MIN) ||
        (rec->fullScaleMmS > CALIB_FULLSCALE_MAX))
    {
        return 0u;
    }
    if ((rec->wheelDiaMm < CALIB_WHEELDIA_MIN) ||
        (rec->wheelDiaMm > CALIB_WHEELDIA_MAX))
    {
        return 0u;
    }
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        if ((rec->invert[i] != 1) && (rec->invert[i] != -1))
        {
            return 0u;
        }
        if (rec->pos[i] > CALIB_POS_REAR_RIGHT)
        {
            return 0u;
        }
    }
    return 1u;
}

uint8_t CALIBREC_jogDecode(const uint8_t *p, uint8_t *motor, int16_t *duty)
{
    int16_t d;

    if ((p == NULL) || (motor == NULL) || (duty == NULL))
    {
        return 0u;
    }
    if (p[0] >= CALIB_REC_WHEELS)
    {
        return 0u;
    }
    d = CALIBREC_getI16(&p[1]);
    if (d > CALIB_JOG_DUTY_MAX)
    {
        d = CALIB_JOG_DUTY_MAX;
    }
    if (d < -CALIB_JOG_DUTY_MAX)
    {
        d = -CALIB_JOG_DUTY_MAX;
    }
    *motor = p[0];
    *duty  = d;
    return 1u;
}

uint8_t CALIBREC_recSetDecode(const uint8_t *p, uint8_t len, CalibRecord *rec)
{
    uint8_t i;

    if ((p == NULL) || (rec == NULL) || (len != CALIB_REC_SET_LEN))
    {
        return 0u;
    }
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        rec->pos[i]    = p[i];
        rec->invert[i] = (int8_t)p[CALIB_REC_WHEELS + i];
    }
    rec->fullScaleMmS = CALIBREC_getI16(&p[8]);
    rec->wheelDiaMm   = CALIBREC_getI16(&p[10]);
    rec->ver          = CALIB_REC_VER;
    return CALIBREC_paramsOk(rec);
}

void CALIBREC_buildEvtResult(uint8_t *buf, uint8_t status,
                             const int8_t invert[CALIB_REC_WHEELS],
                             const int32_t delta[CALIB_REC_WHEELS],
                             uint8_t saved)
{
    uint8_t i;

    buf[0] = 0x70u;                       /* the op this result answers  */
    buf[1] = status;
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        buf[2u + i] = (uint8_t)invert[i];
    }
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        CALIBREC_putI32(&buf[6u + i * 4u], delta[i]);
    }
    buf[CALIB_EVT_RESULT_SAVED] = saved;
}

void CALIBREC_buildEvtRec(uint8_t *buf, const CalibRecord *rec, uint8_t crcOk)
{
    uint8_t i;

    buf[0] = rec->ver;
    buf[1] = rec->src;
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        buf[2u + i] = rec->pos[i];
    }
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        buf[6u + i] = (uint8_t)rec->invert[i];
    }
    CALIBREC_putI16(&buf[10u], rec->fullScaleMmS);
    CALIBREC_putI16(&buf[12u], rec->wheelDiaMm);
    buf[14u] = crcOk;
}

void CALIBREC_encode(const CalibRecord *rec, uint8_t *blob)
{
    uint8_t i;

    memcpy(&blob[BLOB_MAGIC0], g_magic, 4u);
    blob[BLOB_VER] = rec->ver;
    blob[BLOB_SRC] = (uint8_t)rec->src;   /* persisted as DFLASH once loaded  */
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        blob[BLOB_POS + i] = rec->pos[i];
    }
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        blob[BLOB_INVERT + i] = (uint8_t)rec->invert[i];
    }
    CALIBREC_putI16(&blob[BLOB_FULL], rec->fullScaleMmS);
    CALIBREC_putI16(&blob[BLOB_DIA], rec->wheelDiaMm);
    SF_putU16(&blob[BLOB_CRC], SF_crc16(blob, BLOB_CRC_LEN));
}

uint8_t CALIBREC_decode(const uint8_t *blob, CalibRecord *rec)
{
    uint8_t i;

    CALIBREC_fillDefaults(rec);

    if (blob == NULL)
    {
        return 0u;
    }
    if (memcmp(&blob[BLOB_MAGIC0], g_magic, 4u) != 0)
    {
        return 0u;
    }
    if (blob[BLOB_VER] != CALIB_REC_VER)
    {
        return 0u;
    }
    if (SF_getU16(&blob[BLOB_CRC]) != SF_crc16(blob, BLOB_CRC_LEN))
    {
        return 0u;
    }

    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        rec->pos[i]    = blob[BLOB_POS + i];
        rec->invert[i] = (int8_t)blob[BLOB_INVERT + i];
    }
    rec->fullScaleMmS = CALIBREC_getI16(&blob[BLOB_FULL]);
    rec->wheelDiaMm   = CALIBREC_getI16(&blob[BLOB_DIA]);
    if (CALIBREC_paramsOk(rec) == 0u)
    {
        CALIBREC_fillDefaults(rec);
        return 0u;
    }
    rec->src = CALIB_SRC_DFLASH;
    return 1u;
}
