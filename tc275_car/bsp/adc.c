#include "bsp/adc.h"

#include "IfxVadc_Adc.h"
#include "IfxVadc.h"
#include "mw/xcore/xcore.h"

/* Battery voltage acquisition (header doc: D24A J6-1 -> X2-23/AN4, VIN/11).
 *
 * Ownership: CPU1 (doc 21 SS3.4 - the analog measurement sits with the motor
 * core, which is also where the under-voltage guard of SS5.2 will consume
 * it). CPU0/CPU2 read the filtered mV through the xcore battery block.
 *
 * The conversion is the iLLD polled one-shot: fill the group queue with the
 * channel (no refill), wait for the result valid flag (wait-for-read mode
 * clears it on the read itself), done. At ~2-4 us per conversion this is
 * noise in a 1 kHz loop that only asks every ADC_TASK_PERIOD_MS. */

#define ADC_TASK_PERIOD_MS      10u     /* 100 Hz battery sample rate        */
#define ADC_EMA_SHIFT           4u      /* filter: filt += (new-filt) / 16   */
#define ADC_WAIT_LOOPS          2000u   /* bounded spin: conv is ~2-4 us     */

/* the pin voltage in mV as an EMA (raw domain), so one convert step at the end */
static IfxVadc_Adc        g_vadc;
static IfxVadc_Adc_Group  g_group;
static IfxVadc_Adc_Channel g_channel;

static uint32  g_taskDiv;                /* 1 kHz -> 100 Hz divider           */
static uint32  g_pinMvEma;               /* filtered divider-node voltage, mV */
static uint16  g_vinMv;                  /* filtered VIN, mV                  */

/* One queued conversion, result in counts (0..4095). Returns FALSE on a
 * timeout - a dead VADC must not hang the 1 kHz motor loop. */
static boolean adc_convertOnce(uint16 *counts)
{
    Ifx_VADC_RES res;
    uint32       wait;

    IfxVadc_Adc_addToQueue(&g_channel, 0u);          /* one-shot, no refill */

    wait = ADC_WAIT_LOOPS;
    do
    {
        res = IfxVadc_Adc_getResult(&g_channel);     /* read clears VF (wfr) */
        wait--;
    } while ((res.B.VF == 0u) && (wait > 0u));

    if (res.B.VF == 0u)
    {
        return FALSE;
    }

    /* right-aligned storage was set at init: RESULT carries 0..4095 */
    *counts = (uint16)(res.B.RESULT & 0x0FFFu);
    return TRUE;
}

void ADC_init(void)
{
    IfxVadc_Adc_Config         mcfg;
    IfxVadc_Adc_GroupConfig    gcfg;
    IfxVadc_Adc_ChannelConfig  ccfg;

    IfxVadc_Adc_initModuleConfig(&mcfg, &MODULE_VADC);
    /* This board's analog rail (VDDM/VAREF1) is the 3.3 V VEXT rail, not 5 V
     * - the iLLD default would lie to the converter about its supply. */
    mcfg.supplyVoltage = IfxVadc_LowSupplyVoltageSelect_3V;
    IfxVadc_Adc_initModule(&g_vadc, &mcfg);

    IfxVadc_Adc_initGroupConfig(&gcfg, &g_vadc);
    gcfg.groupId = IfxVadc_GroupId_0;                /* AN4 lives here      */
    gcfg.master  = IfxVadc_GroupId_0;
    gcfg.arbiter.requestSlotQueueEnabled = TRUE;     /* polled queue mode   */
    gcfg.queueRequest.triggerConfig.gatingMode = IfxVadc_GatingMode_always;
    IfxVadc_Adc_initGroup(&g_group, &gcfg);

    IfxVadc_Adc_initChannelConfig(&ccfg, &g_group);
    ccfg.channelId           = IfxVadc_ChannelId_4;  /* AN4 = X2-23         */
    ccfg.resultRegister      = IfxVadc_ChannelResult_4;
    ccfg.rightAlignedStorage = TRUE;                 /* RESULT = 0..4095    */
    IfxVadc_Adc_initChannel(&g_channel, &ccfg);

    /* valid flag clears on the result read: add/queue/read, no manual
     * clear step to forget (doc 21 SS18 C6 spirit: keep the loop dumb) */
    IfxVadc_Adc_configureWaitForReadMode(&g_channel, TRUE);

    g_taskDiv  = 0u;
    g_pinMvEma = 0u;
    g_vinMv    = 0u;

    /* Prime the filter with a real sample so the first telemetry frame
     * carries a measurement, not the tail of a ramp from 0. */
    {
        uint16 counts;

        if (adc_convertOnce(&counts))
        {
            uint32 pinMv = ADC_countsToPinMv(counts);

            g_pinMvEma = pinMv;
            g_vinMv    = ADC_pinMvToVinMv(pinMv);
            XCORE_battSetMv(g_vinMv);
        }
    }
}

void ADC_task(void)
{
    uint16 counts;

    g_taskDiv++;
    if ((g_taskDiv % ADC_TASK_PERIOD_MS) != 0u)
    {
        return;
    }

    if (!adc_convertOnce(&counts))
    {
        return;                                      /* keep the last value */
    }

    {
        /* pin voltage mV -> EMA -> VIN via the divider ratio; all unsigned,
         * both ends of the scale are physical clamp points (0 V / VAREF) */
        uint32 pinMv = ADC_countsToPinMv(counts);
        sint32 diff  = (sint32)pinMv - (sint32)g_pinMvEma;

        g_pinMvEma = (uint32)((sint32)g_pinMvEma + (diff >> ADC_EMA_SHIFT));
        g_vinMv    = ADC_pinMvToVinMv(g_pinMvEma);
    }

    XCORE_battSetMv(g_vinMv);
}

uint16 ADC_battVinMv(void)
{
    return g_vinMv;
}

uint8 ADC_battPct(void)
{
    return ADC_battPctFromMv(g_vinMv);
}

uint8 ADC_battPctFromMv(uint16 vinMv)
{
    /* per-cell estimate; percent is a display nicety, the mV figure above is
     * the measurement - cell count is an assumption (doc 23 SS6: 2S..3S) */
    uint32 cellMv = (uint32)vinMv / ADC_BATT_CELLS;

    if (cellMv <= ADC_CELL_EMPTY_MV)
    {
        return 0u;
    }
    if (cellMv >= ADC_CELL_FULL_MV)
    {
        return 100u;
    }
    return (uint8)(((cellMv - ADC_CELL_EMPTY_MV) * 100u)
                   / (ADC_CELL_FULL_MV - ADC_CELL_EMPTY_MV));
}
