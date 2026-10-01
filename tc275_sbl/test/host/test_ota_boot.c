/*
 * test_ota_boot.c - the doc 24 §5.1 decision ladder (rollback semantics)
 */
#include "test.h"

#include "mw/ota/ota_boot.h"
#include "mw/ota/ota_meta.h"

static OtaMeta freshMeta(uint8_t active)
{
    OtaMeta m;

    OTAMETA_setDefault(&m, active);
    return m;
}

int main(void)
{
    OtaMeta m;
    OtaBootAction action;
    uint8_t commit;

    /* 1. pending DOWNLOADED -> becomes PENDING_VERIFY and boots */
    T_CASE("activate downloaded");
    m = freshMeta(OTA_SLOT_A);
    T_CHECK(OTABOOT_markDownloaded(&m, OTA_SLOT_B) == 1u);
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(commit, 1u);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_B);
    T_CHECK_EQ_I(m.active_slot, OTA_SLOT_B);
    T_CHECK_EQ_I(m.slot_b_state, OTA_SSTATE_PENDING_VERIFY);
    T_CHECK_EQ_I(m.pending_slot, OTA_SLOT_NONE);
    T_CHECK_EQ_I(m.boot_attempts, 0u);

    /* 2. PENDING_VERIFY boots increment attempts until the budget is gone */
    T_CASE("attempts climb");
    m = freshMeta(OTA_SLOT_A);
    m.slot_a_state = OTA_SSTATE_PENDING_VERIFY;
    m.boot_attempts = 0u;
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_A);
    T_CHECK_EQ_I(m.boot_attempts, 1u);
    T_CHECK_EQ_I(commit, 1u);
    m.boot_attempts = OTABOOT_MAX_ATTEMPTS - 1u;
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_A);
    T_CHECK_EQ_I(m.boot_attempts, OTABOOT_MAX_ATTEMPTS);

    /* 3. budget exhausted with a valid other slot -> roll back */
    T_CASE("rollback to valid other");
    m = freshMeta(OTA_SLOT_A);
    m.slot_a_state = OTA_SSTATE_PENDING_VERIFY;
    m.slot_b_state = OTA_SSTATE_VALID;
    m.boot_attempts = OTABOOT_MAX_ATTEMPTS;
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(commit, 1u);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_B);
    T_CHECK_EQ_I(m.active_slot, OTA_SLOT_B);
    T_CHECK_EQ_I(m.slot_a_state, OTA_SSTATE_INVALID);
    T_CHECK_EQ_I(m.slot_b_state, OTA_SSTATE_VALID);
    T_CHECK_EQ_I(m.boot_attempts, 0u);

    /* 4. budget exhausted, no valid other -> safe mode, failed slot INVALID */
    T_CASE("rollback impossible -> safe");
    m = freshMeta(OTA_SLOT_A);
    m.slot_a_state = OTA_SSTATE_PENDING_VERIFY;
    m.slot_b_state = OTA_SSTATE_INVALID;
    m.boot_attempts = OTABOOT_MAX_ATTEMPTS;
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(commit, 1u);
    T_CHECK_EQ_I(action, OTABOOT_SAFE_MODE);
    T_CHECK_EQ_I(m.slot_a_state, OTA_SSTATE_INVALID);

    /* 5. VALID boots without any metadata write (DFlash wear refinement) */
    T_CASE("valid boots clean");
    m = freshMeta(OTA_SLOT_B);
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_B);
    T_CHECK_EQ_I(commit, 0u);
    T_CHECK_EQ_I(m.boot_attempts, 0u);

    /* 6. pending image preempts an ailing active image */
    T_CASE("downloaded preempts failing active");
    m = freshMeta(OTA_SLOT_A);
    m.slot_a_state = OTA_SSTATE_PENDING_VERIFY;
    m.boot_attempts = OTABOOT_MAX_ATTEMPTS;      /* would roll back ...    */
    m.slot_b_state = OTA_SSTATE_VALID;
    T_CHECK(OTABOOT_markDownloaded(&m, OTA_SLOT_B) == 1u);
    /* pending B is DOWNLOADED but slot B was VALID before: the mark overwrote
     * the state, which is the intended flow (a new download replaces the old
     * image in that slot) */
    commit = OTABOOT_decide(&m, 0u, &action);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_B);
    T_CHECK_EQ_I(m.slot_b_state, OTA_SSTATE_PENDING_VERIFY);

    /* 7. unusable active slot -> safe */
    T_CASE("empty active -> safe");
    m = freshMeta(OTA_SLOT_A);
    m.slot_a_state = OTA_SSTATE_EMPTY;
    T_CHECK_EQ_I(OTABOOT_decide(&m, 0u, &action), 0u);
    T_CHECK_EQ_I(action, OTABOOT_SAFE_MODE);

    /* 8. no metadata + first-boot flag -> slot A; without it -> safe */
    T_CASE("first boot");
    memset(&m, 0, sizeof(m));                    /* no magic               */
    T_CHECK_EQ_I(OTABOOT_decide(&m, 1u, &action), 0u);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_A);
    T_CHECK_EQ_I(OTABOOT_decide(&m, 0u, &action), 0u);
    T_CHECK_EQ_I(action, OTABOOT_SAFE_MODE);

    /* 9. App self-confirmation flips PENDING_VERIFY to VALID */
    T_CASE("selftest confirm");
    m = freshMeta(OTA_SLOT_B);
    m.slot_b_state = OTA_SSTATE_PENDING_VERIFY;
    m.pending_slot = OTA_SLOT_B;
    m.boot_attempts = 1u;
    T_CHECK(OTABOOT_confirmSelftest(&m) == 1u);
    T_CHECK_EQ_I(m.slot_b_state, OTA_SSTATE_VALID);
    T_CHECK_EQ_I(m.boot_attempts, 0u);
    T_CHECK_EQ_I(m.pending_slot, OTA_SLOT_NONE);
    T_CHECK(OTABOOT_confirmSelftest(&m) == 0u);  /* idempotent: no rewrite  */

    /* 10. abort clears the pending download */
    T_CASE("abort");
    m = freshMeta(OTA_SLOT_A);
    OTABOOT_markDownloaded(&m, OTA_SLOT_B);
    T_CHECK(OTABOOT_markAborted(&m, OTA_SLOT_B) == 1u);
    T_CHECK_EQ_I(m.slot_b_state, OTA_SSTATE_EMPTY);
    T_CHECK_EQ_I(m.pending_slot, OTA_SLOT_NONE);
    T_CHECK_EQ_I(OTABOOT_decide(&m, 0u, &action), 0u);
    T_CHECK_EQ_I(action, OTABOOT_JUMP_A);

    /* 11. bootSlot helper agrees with decide */
    T_CASE("bootSlot helper");
    m = freshMeta(OTA_SLOT_B);
    T_CHECK_EQ_I(OTABOOT_bootSlot(&m), OTA_SLOT_B);
    m.slot_b_state = OTA_SSTATE_EMPTY;
    T_CHECK_EQ_I(OTABOOT_bootSlot(&m), OTA_SLOT_NONE);

    T_RESULT("test_ota_boot");
}
