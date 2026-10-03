/**
  ******************************************************************************
  * @file    platform.h
  *
  * @brief   Aurix TC275 porting layer for ST's VL53L5CX Ultra Lite Driver.
  *
  * @attention
  *
  * This file is NOT an upstream copy. It is the customer-side porting contract
  * that vl53l5cx_api.h includes; the ULD itself (vl53l5cx_api.c/.h,
  * vl53l5cx_buffers.h, changelog.txt, LICENSE.md in this directory) is shipped
  * verbatim from github.com/STMicroelectronics/stm32-vl53l5cx v1.0.7 and must
  * never be edited here (doc/30-tc275/36-tof-driver.md section 2).
  *
  * The six functions prototyped below are implemented in bsp/tof.c, which owns
  * the I2C0 master (CPU0 only) - see doc/20-design/23-wiring.md section 11 for
  * the pin/电气 truth source.
  ******************************************************************************
  */

#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <stdint.h>
#include <string.h>

/* Bus callbacks. Signature kept exactly as upstream defines it so that any
 * upstream example, plugin or later version of the ULD drops in unchanged.
 * The write/read callbacks receive the 8-bit slave address, the 16-bit
 * register index, and a byte count that reaches 32768 (the firmware download
 * writes the whole 32 KB page in one call), so the implementation must chunk. */
typedef int32_t (*VL53L5CX_get_tick_Func)(void);
typedef int32_t (*VL53L5CX_write_Func)(uint16_t, uint16_t, uint8_t *, uint16_t);
typedef int32_t (*VL53L5CX_read_Func)(uint16_t, uint16_t, uint8_t *, uint16_t);

typedef struct
{
    uint16_t address;               /* 8-bit write address, 0x52           */
    VL53L5CX_write_Func Write;
    VL53L5CX_read_Func Read;
    VL53L5CX_get_tick_Func GetTick;
} VL53L5CX_Platform;

/* One target per zone: the avoidance consumer needs the closest/strongest
 * target only, and every extra target doubles distance/status/... arrays in
 * both RAM and the per-frame I2C read. */
#ifndef VL53L5CX_NB_TARGET_PER_ZONE
#define VL53L5CX_NB_TARGET_PER_ZONE     (1U)
#endif

/* Output trimming. Every enabled output is an extra block the sensor puts in
 * the result page and the host must read per frame, so the set below is the
 * I2C-time and RAM budget of the whole driver (doc 36 section 5).
 *
 * Kept:  NB_TARGET_DETECTED (68 B) + DISTANCE_MM (132 B) + TARGET_STATUS (68 B)
 * Cut:   AMBIENT_PER_SPAD (260) NB_SPADS_ENABLED (260) SIGNAL_PER_SPAD (260)
 *        RANGE_SIGMA_MM (132) REFLECTANCE_PERCENT (68) MOTION_INDICATOR (144)
 *
 * That is VL53L5CX_MAX_RESULTS_SIZE = 40 + 68 + 132 + 68 + 20 = 328 B per
 * frame (full output set would be 1232 B), and it holds the temporary buffer
 * at its minimum, 1024 B.
 *
 * Re-enabling one of these is a one-line change here, but it must be rebuilt
 * together with the frame-time budget check of doc 23 section 11.3. */
#define VL53L5CX_DISABLE_AMBIENT_PER_SPAD
#define VL53L5CX_DISABLE_NB_SPADS_ENABLED
#define VL53L5CX_DISABLE_SIGNAL_PER_SPAD
#define VL53L5CX_DISABLE_RANGE_SIGMA_MM
#define VL53L5CX_DISABLE_REFLECTANCE_PERCENT
#define VL53L5CX_DISABLE_MOTION_INDICATOR

/* TriCore is little endian. The ULD's data path is host-order after the
 * platform's SwapBuffer has done the byte reverse, so this is the only
 * endianness statement needed (SWAP_UINT16/SWAP_UINT32 below are not
 * referenced by the API; they are kept for upstream compatibility). */
#define PROCESSOR_LITTLE_ENDIAN
#ifdef PROCESSOR_LITTLE_ENDIAN
  #define SWAP_UINT16(x) (x)
  #define SWAP_UINT32(x) (x)
#else
    #define SWAP_UINT16(x) (((x) >> 8) | ((x) << 8))
    #define SWAP_UINT32(x) (((x) >> 24) | (((x) & 0x00FF0000) >> 8) \
    | (((x) & 0x0000FF00) << 8) | ((x) << 24))
#endif

/**
 * @brief Mandatory functions, implemented in bsp/tof.c.
 *
 * RdByte/WrByte/RdMulti/WrMulti take the 16-bit register index and return 0 on
 * success; any non-zero value is OR-ed into the API's status by vl53l5cx_api.c.
 * SwapBuffer reverses every 4-byte group in place (size is always a multiple
 * of 4). WaitMs must yield, not spin: on CPU0 these calls run inside a
 * FreeRTOS task whose priority is below the robot task.
 */

uint8_t RdByte(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t *p_value);

uint8_t WrByte(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t value);

uint8_t RdMulti(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t *p_values,
        uint32_t size);

uint8_t WrMulti(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t *p_values,
        uint32_t size);

void SwapBuffer(
    uint8_t     *buffer,
    uint16_t     size);

uint8_t WaitMs(
        VL53L5CX_Platform *p_platform,
        uint32_t TimeMs);

#endif  /* _PLATFORM_H_ */
