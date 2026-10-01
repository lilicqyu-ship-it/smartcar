/*
 * test_ota_meta.c - OtaMeta wire codec + double-page store host tests
 * (power-cut safety at the logic level, doc 24 gate G-OTA-6's host half)
 */
#include "test.h"

#include <string.h>

#include "mock_ota.h"
#include "mw/ota/ota_meta.h"

/* tear the newest page exactly like a power cut mid-write: zero some bytes
 * so the CRC fails, without erasing */
static void corruptPage(uint8_t page)
{
    memset(&mock_metaPages[page][0], 0x00, 12u);
}

int main(void)
{
    OtaMeta m;
    OtaMeta got;
    uint8_t wire[OTA_META_WIRE_LEN];
    uint8_t okPages[OTA_META_PAGES];

    /* ---- wire codec ---- */

    T_CASE("wire roundtrip + magic bytes");
    OTAMETA_setDefault(&m, OTA_SLOT_A);
    m.seq = 0xAABBCC01u;
    m.boot_attempts = 2u;
    m.slot_b_state = OTA_SSTATE_DOWNLOADED;
    m.pending_slot = OTA_SLOT_B;
    OTAMETA_encode(&m, wire);
    T_CHECK_EQ_I(wire[0], 'T');
    T_CHECK_EQ_I(wire[1], 'C');
    T_CHECK_EQ_I(wire[2], 'O');
    T_CHECK_EQ_I(wire[3], 'M');
    T_CHECK(OTAMETA_decode(wire, &got) == 1u);
    T_CHECK_EQ_I(got.seq, 0xAABBCC01u);
    T_CHECK_EQ_I(got.active_slot, OTA_SLOT_A);
    T_CHECK_EQ_I(got.pending_slot, OTA_SLOT_B);
    T_CHECK_EQ_I(got.slot_b_state, OTA_SSTATE_DOWNLOADED);
    T_CHECK_EQ_I(got.boot_attempts, 2u);

    T_CASE("crc gate");
    wire[8] ^= 0x01u;                       /* active_slot field           */
    T_CHECK(OTAMETA_decode(wire, &got) == 0u);

    T_CASE("magic gate");
    OTAMETA_encode(&m, wire);
    wire[3] = 'X';
    T_CHECK(OTAMETA_decode(wire, &got) == 0u);

    /* ---- double page store ---- */

    T_CASE("blank flash -> invalid");
    MOCK_reset();
    OTAMETA_attach(mock_metaBackend());
    T_CHECK(OTAMETA_load() == 0u);

    T_CASE("commit + reload");
    OTAMETA_setDefault(&m, OTA_SLOT_A);
    T_CHECK(OTAMETA_commit(&m) == 1u);
    T_CHECK(OTAMETA_load() == 1u);
    T_CHECK_EQ_I(OTAMETA_get()->magic, OTA_META_MAGIC);
    T_CHECK_EQ_I(OTAMETA_get()->seq, 1u);
    T_CHECK_EQ_I(OTAMETA_get()->active_slot, OTA_SLOT_A);
    T_CHECK_EQ_I(OTAMETA_get()->slot_a_state, OTA_SSTATE_VALID);

    T_CASE("pages alternate and seq climbs");
    {
        uint8_t first = OTAMETA_currentPage();
        uint8_t i;

        for (i = 0u; i < 5u; i++)
        {
            m = *OTAMETA_get();
            m.boot_attempts = i;
            T_CHECK(OTAMETA_commit(&m) == 1u);
            T_CHECK_EQ_I(OTAMETA_currentPage(), (uint8_t)(first ^ 1u ^ (i & 1u)));
            T_CHECK_EQ_I(OTAMETA_get()->seq, 2u + i);
        }
        T_CHECK(OTAMETA_load() == 1u);              /* reload agrees         */
        T_CHECK_EQ_I(OTAMETA_get()->seq, 6u);
        T_CHECK_EQ_I(OTAMETA_get()->boot_attempts, 4u);
    }

    T_CASE("power cut corrupts newest -> older wins");
    {
        uint32_t seqBefore = OTAMETA_get()->seq;
        uint8_t  goodPage = OTAMETA_currentPage();

        corruptPage(goodPage);                      /* the newest revision  */
        T_CHECK(OTAMETA_load() == 1u);
        T_CHECK_EQ_I(OTAMETA_get()->seq, seqBefore - 1u);
        T_CHECK_EQ_I(OTAMETA_currentPage(), (uint8_t)(goodPage ^ 1u));
    }

    T_CASE("higher seq wins regardless of page order");
    MOCK_reset();
    OTAMETA_attach(mock_metaBackend());
    {
        OtaMeta p0;
        OtaMeta p1;

        OTAMETA_setDefault(&p0, OTA_SLOT_A);
        p0.seq = 10u;
        OTAMETA_encode(&p0, mock_metaPages[0]);
        OTAMETA_setDefault(&p1, OTA_SLOT_B);
        p1.seq = 20u;
        OTAMETA_encode(&p1, mock_metaPages[1]);
        T_CHECK(OTAMETA_load() == 1u);
        T_CHECK_EQ_I(OTAMETA_get()->seq, 20u);
        T_CHECK_EQ_I(OTAMETA_get()->active_slot, OTA_SLOT_B);
        T_CHECK_EQ_I(OTAMETA_currentPage(), 1u);
    }

    T_CASE("seq tie -> page 0, deterministic");
    MOCK_reset();
    OTAMETA_attach(mock_metaBackend());
    {
        OtaMeta p0;
        OtaMeta p1;

        OTAMETA_setDefault(&p0, OTA_SLOT_A);
        p0.seq = 7u;
        OTAMETA_encode(&p0, mock_metaPages[0]);
        OTAMETA_setDefault(&p1, OTA_SLOT_B);
        p1.seq = 7u;
        OTAMETA_encode(&p1, mock_metaPages[1]);
        T_CHECK(OTAMETA_load() == 1u);
        T_CHECK_EQ_I(OTAMETA_currentPage(), 0u);
        T_CHECK_EQ_I(OTAMETA_get()->active_slot, OTA_SLOT_A);
    }

    T_CASE("commit lands on the damaged page and recovers it");
    MOCK_reset();
    OTAMETA_attach(mock_metaBackend());
    OTAMETA_setDefault(&m, OTA_SLOT_A);
    T_CHECK(OTAMETA_commit(&m) == 1u);
    {
        uint8_t nextPage = (uint8_t)(OTAMETA_currentPage() ^ 1u);

        corruptPage(OTAMETA_currentPage());         /* kill the good page    */
        T_CHECK(OTAMETA_load() == 0u);
        (void)mock_metaValidPages(okPages);
        T_CHECK_EQ_I(okPages[0], 0u);
        T_CHECK_EQ_I(okPages[1], 0u);

        /* both pages dead: a fresh commit re-seeds page 0 (seq restarts)  */
        OTAMETA_reset();
        OTAMETA_setDefault(&m, OTA_SLOT_B);
        T_CHECK(OTAMETA_commit(&m) == 1u);
        T_CHECK_EQ_I(OTAMETA_currentPage(), 0u);
        T_CHECK_EQ_I(OTAMETA_get()->seq, 1u);
        T_CHECK(OTAMETA_load() == 1u);
        T_CHECK_EQ_I(OTAMETA_get()->active_slot, OTA_SLOT_B);
        (void)nextPage;
    }

    T_CASE("commit failure: erase refuses");
    MOCK_reset();
    OTAMETA_attach(mock_metaBackend());
    OTAMETA_setDefault(&m, OTA_SLOT_A);
    OTAMETA_commit(&m);
    mock_metaEraseFail = 1u;
    m = *OTAMETA_get();
    m.active_slot = OTA_SLOT_B;
    T_CHECK(OTAMETA_commit(&m) == 0u);
    T_CHECK(OTAMETA_load() == 1u);
    T_CHECK_EQ_I(OTAMETA_get()->active_slot, OTA_SLOT_A);   /* unchanged */

    T_CASE("commit failure: write refuses");
    mock_metaEraseFail = 0u;
    mock_metaWriteFail = 1u;
    T_CHECK(OTAMETA_commit(&m) == 0u);
    mock_metaWriteFail = 0u;
    T_CHECK(OTAMETA_load() == 1u);
    T_CHECK_EQ_I(OTAMETA_get()->active_slot, OTA_SLOT_A);

    T_RESULT("test_ota_meta");
}
