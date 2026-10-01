/*
 * ota_keys.h - ed25519 public keys embedded in the TC275 firmware
 *
 * OTA_KEYS_DEV is the DEVELOPMENT key pair, byte-identical to
 * c6_car/components/c6_ota/keys/pub_ed25519_dev.bin so both sides of the
 * bench accept bundles from the same dev seed
 * (c6_car tools/keys/ed25519_dev.seed, used by tools/sign_bundle.py).
 *
 * Production MUST provision a distinct TC275 key pair: the private seed of
 * the dev pair sits in a git-tracked repo. Swapping the key here and in the
 * packer at the same time is a breaking change to every stored bundle.
 */
#ifndef OTA_KEYS_H
#define OTA_KEYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_KEYS_DEV_LEN 32u

static const uint8_t OTA_KEYS_DEV[OTA_KEYS_DEV_LEN] =
{
    0x74, 0x51, 0x09, 0x99, 0xf7, 0x42, 0x48, 0x14,
    0x44, 0x63, 0x95, 0x8a, 0xbf, 0xbd, 0xd6, 0xfc,
    0x55, 0x9f, 0x9a, 0xb2, 0x47, 0xc3, 0x11, 0xfb,
    0x58, 0xf7, 0x4c, 0x5d, 0x6d, 0x46, 0x8b, 0x5f
};

#ifdef __cplusplus
}
#endif

#endif /* OTA_KEYS_H */
