/*
 * ota_boot.c - the doc 24 §5.1 boot ladder over OtaMeta
 */
#include "ota_boot.h"

static uint8_t boot_slotState(const OtaMeta *m, uint8_t slot)
{
    return (slot == OTA_SLOT_B) ? m->slot_b_state : m->slot_a_state;
}

static void boot_setSlotState(OtaMeta *m, uint8_t slot, uint8_t state)
{
    if (slot == OTA_SLOT_B)
    {
        m->slot_b_state = state;
    }
    else
    {
        m->slot_a_state = state;
    }
}

static OtaBootAction boot_jump(uint8_t slot)
{
    return (slot == OTA_SLOT_B) ? OTABOOT_JUMP_B : OTABOOT_JUMP_A;
}

uint8_t OTABOOT_decide(OtaMeta *m, uint8_t firstBootAOk, OtaBootAction *out)
{
    uint8_t a;
    uint8_t p;

    if (m->magic != OTA_META_MAGIC)
    {
        /* No (valid) metadata: factory bring-up path only. */
        *out = (firstBootAOk != 0u) ? OTABOOT_JUMP_A : OTABOOT_SAFE_MODE;
        return 0u;
    }

    /* 1. a fully received image is waiting for its first boot */
    p = m->pending_slot;
    if ((p <= OTA_SLOT_B) && (boot_slotState(m, p) == OTA_SSTATE_DOWNLOADED))
    {
        m->active_slot    = p;
        m->pending_slot   = OTA_SLOT_NONE;
        m->boot_attempts  = 0u;
        boot_setSlotState(m, p, OTA_SSTATE_PENDING_VERIFY);
        *out = boot_jump(p);
        return 1u;
    }

    a = m->active_slot;
    if (a > OTA_SLOT_B)
    {
        *out = OTABOOT_SAFE_MODE;
        return 0u;
    }

    /* 2. the pending-verify image never confirmed itself: roll back */
    if ((boot_slotState(m, a) == OTA_SSTATE_PENDING_VERIFY) &&
        (m->boot_attempts >= OTABOOT_MAX_ATTEMPTS))
    {
        uint8_t other = (uint8_t)(a ^ 1u);

        boot_setSlotState(m, a, OTA_SSTATE_INVALID);
        if (boot_slotState(m, other) == OTA_SSTATE_VALID)
        {
            m->active_slot   = other;
            m->boot_attempts = 0u;
            *out = boot_jump(other);
            return 1u;
        }
        /* nothing valid to fall back to - brick-safe stop in the SBL */
        *out = OTABOOT_SAFE_MODE;
        return 1u;
    }

    /* 3. pending-verify with tries left: count this boot */
    if (boot_slotState(m, a) == OTA_SSTATE_PENDING_VERIFY)
    {
        m->boot_attempts++;
        *out = boot_jump(a);
        return 1u;
    }

    /* 4. plain valid image: boot it without touching DFlash */
    if (boot_slotState(m, a) == OTA_SSTATE_VALID)
    {
        *out = boot_jump(a);
        return 0u;
    }

    /* 5. EMPTY / WRITING / INVALID active slot */
    *out = OTABOOT_SAFE_MODE;
    return 0u;
}

uint8_t OTABOOT_confirmSelftest(OtaMeta *m)
{
    uint8_t a;

    if ((m->magic != OTA_META_MAGIC) || (m->active_slot > OTA_SLOT_B))
    {
        return 0u;
    }
    a = m->active_slot;
    if (boot_slotState(m, a) != OTA_SSTATE_PENDING_VERIFY)
    {
        return 0u;
    }
    boot_setSlotState(m, a, OTA_SSTATE_VALID);
    m->boot_attempts = 0u;
    m->pending_slot  = OTA_SLOT_NONE;
    return 1u;
}

uint8_t OTABOOT_markDownloaded(OtaMeta *m, uint8_t slot)
{
    if ((m->magic != OTA_META_MAGIC) || (slot > OTA_SLOT_B))
    {
        return 0u;
    }
    boot_setSlotState(m, slot, OTA_SSTATE_DOWNLOADED);
    m->pending_slot = slot;
    return 1u;
}

uint8_t OTABOOT_markAborted(OtaMeta *m, uint8_t slot)
{
    if ((m->magic != OTA_META_MAGIC) || (slot > OTA_SLOT_B))
    {
        return 0u;
    }
    boot_setSlotState(m, slot, OTA_SSTATE_EMPTY);
    if (m->pending_slot == slot)
    {
        m->pending_slot = OTA_SLOT_NONE;
    }
    return 1u;
}

uint8_t OTABOOT_bootSlot(const OtaMeta *m)
{
    OtaBootAction action;
    OtaMeta copy = *m;

    (void)OTABOOT_decide(&copy, 0u, &action);
    if (action == OTABOOT_JUMP_B)
    {
        return OTA_SLOT_B;
    }
    if (action == OTABOOT_JUMP_A)
    {
        return OTA_SLOT_A;
    }
    return OTA_SLOT_NONE;
}
