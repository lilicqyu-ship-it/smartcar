/*
 * sf_telemetry.c - the 38 byte SF telemetry payload codec
 *
 * See sf_telemetry.h for the byte table this file is a transcription of, and for
 * why a short payload means the slave drops the frame entirely.
 */
#include "mw/sf/sf_telemetry.h"

#include "mw/sf/sf_frame.h"          /* SF_putU16 / SF_putU32 and their getters      */

int16_t SF_telemetryEncode(const SF_Telemetry *tel, uint8_t *out, uint16_t cap)
{
    if ((tel == NULL) || (out == NULL) || (cap < SF_TELEMETRY_LEN))
    {
        return -1;
    }

    SF_putU32(&out[0],  tel->seq);
    SF_putU32(&out[4],  tel->uptimeMs);
    out[8]  = tel->state;
    SF_putU16(&out[9],  tel->faultCode);
    SF_putU16(&out[11], (uint16_t)tel->vTargetLeft);
    SF_putU16(&out[13], (uint16_t)tel->vTargetRight);
    SF_putU16(&out[15], (uint16_t)tel->vMeasLeft);
    SF_putU16(&out[17], (uint16_t)tel->vMeasRight);
    SF_putU16(&out[19], tel->batteryMv);
    out[21] = tel->batteryPct;
    SF_putU32(&out[22], tel->odoSessionMm);
    SF_putU32(&out[26], tel->odoTotalMm);
    SF_putU16(&out[30], tel->linkRttMs);
    out[32] = tel->linkErrRate;
    SF_putU32(&out[33], tel->fwVer);
    out[37] = tel->hwRev;
    return (int16_t)SF_TELEMETRY_LEN;
}

uint8_t SF_telemetryDecode(const uint8_t *data, uint16_t len, SF_Telemetry *tel)
{
    if ((data == NULL) || (tel == NULL) || (len < SF_TELEMETRY_LEN))
    {
        return 0u;
    }

    tel->seq           = SF_getU32(&data[0]);
    tel->uptimeMs      = SF_getU32(&data[4]);
    tel->state         = data[8];
    tel->faultCode     = SF_getU16(&data[9]);
    tel->vTargetLeft   = (int16_t)SF_getU16(&data[11]);
    tel->vTargetRight  = (int16_t)SF_getU16(&data[13]);
    tel->vMeasLeft     = (int16_t)SF_getU16(&data[15]);
    tel->vMeasRight    = (int16_t)SF_getU16(&data[17]);
    tel->batteryMv     = SF_getU16(&data[19]);
    tel->batteryPct    = data[21];
    tel->odoSessionMm  = SF_getU32(&data[22]);
    tel->odoTotalMm    = SF_getU32(&data[26]);
    tel->linkRttMs     = SF_getU16(&data[30]);
    tel->linkErrRate   = data[32];
    tel->fwVer         = SF_getU32(&data[33]);
    tel->hwRev         = data[37];
    return 1u;
}
