/* Transaction schema 1. Canonical pure C99 wire helpers, no native structs
 * on wire. See transaction-v1.md. Adoption does not imply C6/iOS support. */
#ifndef SMARTCAR_DIAG_WIRE_H
#define SMARTCAR_DIAG_WIRE_H
#include <stdint.h>
#include <string.h>
#define DT_SUB 0x60u
#define DT_SCHEMA 1u
#define DT_REQUEST_LEN 16u
#define DT_HEADER_LEN 18u
#define DT_FRAME_MAX 32u
#define DT_BODY_CHUNK 14u
#define DT_CONTROL_CID 0x28u
#define DT_DATA_CID 0x29u
enum { DT_HELLO=1, DT_CAPS=2, DT_STATUS=3, DT_CANCEL=4, DT_KEEPALIVE=5,
       DT_CAPTURE=0x10, DT_IMU_STATIC=0x11, DT_TOF_REFERENCE=0x12 };
enum { DT_ACK=1, DT_PROGRESS=2, DT_RESULT=3, DT_CAPS_RECORD=4,
       DT_STATUS_RECORD=5, DT_IMU_RECORD=0x10, DT_TOF_RECORD=0x11 };
enum { DT_OK=0, DT_BAD_LENGTH, DT_BAD_SCHEMA, DT_BAD_OPCODE, DT_BAD_ARGUMENT,
       DT_BUSY, DT_NOT_READY, DT_STALE, DT_ID_CONFLICT, DT_SESSION_LOST,
       DT_CANCELED, DT_LINK_LOST, DT_OVERFLOW, DT_INTERNAL, DT_EXPIRED,
       DT_MOTION_ACTIVE, DT_UNAUTHORIZED };
enum { DT_IDLE=0, DT_RUNNING, DT_COMPLETED, DT_TASK_CANCELED,
       DT_FAILED, DT_UNKNOWN };
enum { DT_UNASSESSED=0, DT_PASS, DT_FAIL, DT_INCONCLUSIVE };
static inline uint16_t DT_u16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }
static inline uint32_t DT_u32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline void DT_put16(uint8_t *p, uint16_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void DT_put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
/* Also usable by CPU2 for pre-execution queue/length rejection. */
static inline uint8_t DT_rejection(uint8_t *out, const uint8_t *request,
                                 uint8_t len, uint8_t code, uint32_t now)
{
    if (!out || !request || len<10u || request[0]!=DT_SUB) return 0u;
    memset(out,0,DT_HEADER_LEN);
    out[0]=DT_SCHEMA; out[1]=DT_RESULT; out[2]=code;
    memcpy(out+4,request+4,6u); DT_put32(out+12,now); out[17]=1u;
    out[18]=9u; out[19]=1u; out[20]=4u; /* task_state=failed */
    out[21]=15u; out[22]=1u; out[23]=3u; /* verdict=inconclusive */
    return 24u;
}
#endif
