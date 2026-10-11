/*
 * TestHostIavBrokerHandles.cxx
 *
 * Handle lifetime, identity and quotas: stale, foreign, forged, duplicate and zero handles, slot
 * reuse, a broker restart, the per-guest limits and guest teardown.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

u32 g_state;

}

TEST_GROUP(IavBrokerHandles)
{
    IavBrokerHarness *h;

    void setup() { h = new IavBrokerHarness(); }

    void teardown()
    {
        CHECK(h->drv.violations().empty());
        delete h;
    }

    IavBrokerHarness::Result getState(uint32_t cid, uint64_t handle)
    {
        return h->ioctl(cid, handle, IAV_IOC_GET_IAV_STATE, NULL, 0);
    }
};

TEST(IavBrokerHandles, OpenUseClose)
{
    IavBrokerHarness::Result r = h->open(7);

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    CHECK(r.r.handle != 0);
    LONGS_EQUAL(1, h->drv.openFds());
    LONGS_EQUAL(1, iav_broker_session_count(&h->b));

    r = getState(7, h->mustOpen(7));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    LONGS_EQUAL(sizeof(u32), r.out.size());
    memcpy(&g_state, &r.out[0], sizeof(g_state));
    LONGS_EQUAL(2, g_state);
}

TEST(IavBrokerHandles, ClosedHandleIsStaleAndClosingTwiceIsToo)
{
    uint64_t hd = h->mustOpen(7);

    LONGS_EQUAL(0, h->close(7, hd).r.status);
    LONGS_EQUAL(0, h->drv.openFds());
    LONGS_EQUAL(-ESTALE, getState(7, hd).r.status);
    LONGS_EQUAL(-ESTALE, h->close(7, hd).r.status);
    LONGS_EQUAL(0, h->drv.openFds());
    LONGS_EQUAL(1, h->drv.totalCloses());
}

TEST(IavBrokerHandles, ZeroAndMalformedHandlesAreBadDescriptors)
{
    uint64_t malformed[] = {0, 0xffffffffffffffffull, 0x0000000100000000ull, 0x0000000200010000ull,
                            0x000000010001ffffull, 0x0000000100010041ull /* index 65 */,
                            0x00000000ffff0000ull};

    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        int st = getState(7, malformed[i]).r.status;

        CHECK(st == -EBADF || st == -ESTALE);
    }
    LONGS_EQUAL(-EBADF, getState(7, 0).r.status);
    LONGS_EQUAL(0, h->drv.ioctlCalls());
}

TEST(IavBrokerHandles, ForgedHandlesNeverSucceed)
{
    uint64_t good = h->mustOpen(7);
    uint32_t x = 0x9e3779b9u;

    CHECK(good != 0);
    for (int i = 0; i < 20000; i++) {
        uint64_t forged;

        x = x * 1664525u + 1013904223u;
        forged = ((uint64_t)x << 32);
        x = x * 1664525u + 1013904223u;
        forged |= x;
        if (forged == good) {
            continue;
        }
        CHECK(getState(7, forged).r.status != 0);
    }
    LONGS_EQUAL(0, getState(7, good).r.status);
    LONGS_EQUAL(0, h->drv.ioctlCalls() - 1);
}

TEST(IavBrokerHandles, AnotherGuestsHandleIsForeign)
{
    uint64_t hd = h->mustOpen(7);
    struct iav_poll_event_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    LONGS_EQUAL(-EACCES, getState(8, hd).r.status);
    LONGS_EQUAL(-EACCES, h->close(8, hd).r.status);
    LONGS_EQUAL(-EACCES, h->poll(8, hd, POLLIN, 10).r.status);
    LONGS_EQUAL(-EACCES, h->map(8, hd, MockIavDriver::DspBase, MockIavDriver::DspLen, PROT_READ).r.status);
    LONGS_EQUAL(-EACCES, h->ioctl(8, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)).r.status);
    /* Nothing of the owner's was disturbed. */
    LONGS_EQUAL(1, h->drv.openFds());
    LONGS_EQUAL(0, getState(7, hd).r.status);
    LONGS_EQUAL(1, h->drv.reached(IAV_IOC_GET_IAV_STATE));
}

TEST(IavBrokerHandles, ReusedSlotDoesNotRevivesAnOldHandle)
{
    uint64_t first = h->mustOpen(7);
    uint64_t second;

    LONGS_EQUAL(0, h->close(7, first).r.status);
    second = h->mustOpen(7);
    CHECK(first != second);
    LONGS_EQUAL(-ESTALE, getState(7, first).r.status);
    LONGS_EQUAL(0, getState(7, second).r.status);
}

TEST(IavBrokerHandles, HandleFromBeforeABrokerRestartIsStale)
{
    IavBrokerHarness a(IAV_PROFILE_QUALIFICATION, 0, 0, 0, 0x11111111u);
    IavBrokerHarness b(IAV_PROFILE_QUALIFICATION, 0, 0, 0, 0x77777777u);
    uint64_t ha = a.mustOpen(7);
    uint64_t hb = b.mustOpen(7);

    CHECK(ha != 0 && hb != 0);
    /* Same slot, same generation, different broker instance: stale, not foreign and not valid. */
    LONGS_EQUAL(-ESTALE, b.ioctl(7, ha, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
    LONGS_EQUAL(0, b.ioctl(7, hb, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
}

TEST(IavBrokerHandles, PerGuestLimit)
{
    IavBrokerHarness lim(IAV_PROFILE_QUALIFICATION, 3);
    uint64_t hs[3];

    for (int i = 0; i < 3; i++) {
        hs[i] = lim.mustOpen(7);
        CHECK(hs[i] != 0);
    }
    LONGS_EQUAL(-EMFILE, lim.open(7).r.status);
    /* Another guest is not affected. */
    CHECK(lim.mustOpen(8) != 0);
    /* Closing one frees a place. */
    LONGS_EQUAL(0, lim.close(7, hs[1]).r.status);
    CHECK(lim.mustOpen(7) != 0);
    LONGS_EQUAL(4, lim.drv.openFds());
}

TEST(IavBrokerHandles, TotalLimit)
{
    IavBrokerHarness all(IAV_PROFILE_QUALIFICATION, IAV_BROKER_MAX_SESSIONS);
    int ok = 0;

    for (uint32_t cid = 1; cid <= 8; cid++) {
        for (int i = 0; i < 8; i++) {
            ok += all.mustOpen(cid) != 0;
        }
    }
    LONGS_EQUAL(IAV_BROKER_MAX_SESSIONS, ok);
    LONGS_EQUAL(-EMFILE, all.open(9).r.status);
    LONGS_EQUAL(IAV_BROKER_MAX_SESSIONS, all.drv.openFds());
}

TEST(IavBrokerHandles, DriverOpenFailureIsReportedAndCostsNoQuota)
{
    IavBrokerHarness lim(IAV_PROFILE_QUALIFICATION, 1);
    IavBrokerHarness::Result r;

    lim.drv.failOpensWith(ENOENT);
    r = lim.open(7);
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(ENOENT, r.r.err);
    LONGS_EQUAL(0, iav_broker_session_count(&lim.b));
    lim.drv.failOpensWith(0);
    CHECK(lim.mustOpen(7) != 0);
}

TEST(IavBrokerHandles, GuestGoneReleasesItsSessionsOnly)
{
    uint64_t mine[3], theirs[2];

    for (int i = 0; i < 3; i++) {
        mine[i] = h->mustOpen(7);
    }
    for (int i = 0; i < 2; i++) {
        theirs[i] = h->mustOpen(8);
    }
    LONGS_EQUAL(5, h->drv.openFds());
    iav_broker_client_gone(&h->b, 7);
    LONGS_EQUAL(2, h->drv.openFds());
    LONGS_EQUAL(0, iav_broker_cid_sessions(&h->b, 7));
    LONGS_EQUAL(2, iav_broker_cid_sessions(&h->b, 8));
    for (int i = 0; i < 3; i++) {
        LONGS_EQUAL(-ESTALE, getState(7, mine[i]).r.status);
    }
    LONGS_EQUAL(0, getState(8, theirs[0]).r.status);
    /* Idempotent. */
    iav_broker_client_gone(&h->b, 7);
    iav_broker_client_gone(&h->b, 999);
    LONGS_EQUAL(2, h->drv.openFds());
}

TEST(IavBrokerHandles, IdleSessionsAreReaped)
{
    uint64_t old1 = h->mustOpen(7);
    uint64_t busy;

    h->drv.advance(5000);
    busy = h->mustOpen(7);
    LONGS_EQUAL(1, iav_broker_reap_idle(&h->b, 3000));
    LONGS_EQUAL(-ESTALE, getState(7, old1).r.status);
    LONGS_EQUAL(0, getState(7, busy).r.status);
    LONGS_EQUAL(1, h->drv.openFds());
}

TEST(IavBrokerHandles, ManyGuestsComeAndGoWithoutExhaustingTheTable)
{
    /* Guests restart with new ids. More ids than the table holds must all be served. */
    for (uint32_t cid = 1000; cid < 1100; cid++) {
        uint64_t hd = h->mustOpen(cid);

        CHECK(hd != 0);
        LONGS_EQUAL(0, h->close(cid, hd).r.status);
    }
    LONGS_EQUAL(0, h->drv.openFds());
}

TEST(IavBrokerHandles, EverySessionIsClosedOnDestroy)
{
    IavBrokerHarness k;

    for (int i = 0; i < 5; i++) {
        CHECK(k.mustOpen(7) != 0);
    }
    LONGS_EQUAL(5, k.drv.openFds());
    k.destroyBroker();
    LONGS_EQUAL(0, k.drv.openFds());
    CHECK(k.drv.violations().empty());
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
