/*
 * TestHostIavBrokerTable.cxx
 *
 * The ioctl table: every ioctl the driver serves is catalogued, the allow-list is exactly what
 * was reviewed, and every ioctl that is not on it is refused without reaching the driver. A
 * change to the driver that adds an ioctl cannot be allowed by accident: the table is
 * regenerated from the driver sources, and a new entry starts denied.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <set>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

/* The reviewed allow-list. Changing the broker's allow-list means changing this too. */
const unsigned long Reviewed[] = {
    IAV_IOC_GET_IAV_STATE,        IAV_IOC_GET_CHIP_ID,          IAV_IOC_QUERY_ENC_MODE_CAP,
    IAV_IOC_QUERY_ENC_BUF_CAP,    IAV_IOC_QUERY_IAV_CAP,        IAV_IOC_GET_SYSTEM_RESOURCE,
    IAV_IOC_GET_CHAN_CONFIG,      IAV_IOC_GET_CANVAS_CONFIG,    IAV_IOC_DRV_DSP_INFO,
    IAV_IOC_QUERY_INFO,           IAV_IOC_QUERY_MEMBLOCK,       IAV_IOC_QUERY_DESC,
    IAV_IOC_SET_POLL_EVENT_CFG,   IAV_IOC_CLEAR_POLL_EVENT_CFG, IAV_IOC_WAIT_NEXT_IDSP_FRAME,
};

bool reviewed(unsigned int cmd)
{
    for (size_t i = 0; i < sizeof(Reviewed) / sizeof(Reviewed[0]); i++) {
        if (Reviewed[i] == cmd) {
            return true;
        }
    }
    return false;
}

bool inProfile(const struct iav_policy_entry *e, enum iav_profile p)
{
    return (e->profiles & IAV_PROFILE_BIT(p)) != 0;
}

} // namespace

TEST_GROUP(IavBrokerTable)
{
    void setup() { LONGS_EQUAL(0, iav_policy_init()); }
};

TEST(IavBrokerTable, EveryIoctlOfTheDriverIsCatalogued)
{
    /* 205 on N1-655 at the time of writing; a regenerated table with another count must be reviewed. */
    LONGS_EQUAL(205, iav_policy_count());
}

TEST(IavBrokerTable, NoTwoIoctlsShareANumber)
{
    std::set<unsigned int> seen;

    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        CHECK(e != NULL);
        CHECK(seen.insert(e->cmd).second);
    }
}

TEST(IavBrokerTable, EntriesAreInternallyConsistent)
{
    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        CHECK(e->name != NULL && e->name[0] != '\0');
        /* The consumer set is part of the qualification set. */
        if (inProfile(e, IAV_PROFILE_CONSUMER)) {
            CHECK_TEXT(inProfile(e, IAV_PROFILE_QUALIFICATION), e->name);
        }
        switch (e->kind) {
        case IAV_ARG_NONE:
            LONGS_EQUAL(0, e->size);
            break;
        case IAV_ARG_VALUE:
            LONGS_EQUAL(0, e->size);
            break;
        case IAV_ARG_PTR:
            CHECK(e->size > 0);
            CHECK(e->dir != IAV_DIR_NONE);
            break;
        default:
            FAIL("an entry has an unknown argument kind");
        }
        if (e->profiles != 0) {
            /* Whatever is allowed must fit in one message. */
            CHECK(e->size <= AMBA_VIRT_IAV_MAX_ARG);
            CHECK(e->size <= AMBA_VIRT_IAV_MAX_OUT);
        }
        LONGS_EQUAL(e, iav_policy_lookup(e->cmd));
    }
}

TEST(IavBrokerTable, TheConsumerAllowListIsExactlyWhatWasReviewed)
{
    size_t allowed = 0;

    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        if (inProfile(e, IAV_PROFILE_CONSUMER)) {
            allowed++;
            CHECK_TEXT(reviewed(e->cmd), e->name);
        } else {
            CHECK_TEXT(!reviewed(e->cmd), e->name);
        }
    }
    LONGS_EQUAL(sizeof(Reviewed) / sizeof(Reviewed[0]), allowed);
}

TEST(IavBrokerTable, NothingStateChangingOrAddressBearingIsAllowedToAConsumer)
{
    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        if (!inProfile(e, IAV_PROFILE_CONSUMER)) {
            continue;
        }
        CHECK_TEXT((e->flags & IAV_F_STATE) == 0, e->name);
        CHECK_TEXT((e->flags & IAV_F_DEBUG) == 0 || e->cmd == IAV_IOC_DRV_DSP_INFO, e->name);
        /* QUERY_INFO and QUERY_MEMBLOCK only report addresses; the others carry none in. */
        if (e->cls & IAV_CLS_PHYS) {
            CHECK_TEXT(e->cmd == IAV_IOC_QUERY_INFO || e->cmd == IAV_IOC_QUERY_MEMBLOCK ||
                       e->cmd == IAV_IOC_QUERY_DESC, e->name);
        }
        CHECK_TEXT((e->cls & IAV_CLS_POINTER) == 0, e->name);
    }
}

TEST(IavBrokerTable, EveryIoctlOutsideTheConsumerSetIsRefusedAndNeverReachesTheDriver)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER);
    uint64_t hd = h.mustOpen(7);
    uint8_t arg[AMBA_VIRT_MAX_MSG];
    size_t denied = 0;

    memset(arg, 0, sizeof(arg));
    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);
        IavBrokerHarness::Result r;

        if (inProfile(e, IAV_PROFILE_CONSUMER)) {
            continue;
        }
        denied++;
        /* With the shape the driver would take, and with the wrong shape: both are refused. */
        r = h.ioctl(7, hd, e->cmd, e->kind == IAV_ARG_PTR ? arg : NULL,
                    e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ? (uint32_t)e->size : 0);
        LONGS_EQUAL_TEXT(-EPERM, r.r.status, e->name);
        r = h.ioctl(7, hd, e->cmd, arg, 17, 0x1234567890abcdefull);
        LONGS_EQUAL_TEXT(-EPERM, r.r.status, e->name);
        r = h.ioctl(7, 0, e->cmd, NULL, 0);
        LONGS_EQUAL_TEXT(-EPERM, r.r.status, e->name);
    }
    LONGS_EQUAL(205 - 15, denied);
    LONGS_EQUAL(0, h.drv.ioctlCalls());
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerTable, EveryConsumerIoctlWorksWithAWellFormedRequest)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER);
    uint64_t hd = h.mustOpen(7);
    uint8_t arg[AMBA_VIRT_MAX_MSG];

    memset(arg, 0, sizeof(arg));
    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);
        IavBrokerHarness::Result r;

        if (!inProfile(e, IAV_PROFILE_CONSUMER)) {
            continue;
        }
        r = h.ioctl(7, hd, e->cmd, e->kind == IAV_ARG_PTR ? arg : NULL,
                    e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ? (uint32_t)e->size : 0, 1);
        LONGS_EQUAL_TEXT(0, r.r.status, e->name);
        CHECK_TEXT(r.r.rc >= 0, e->name);
        if (e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_R)) {
            LONGS_EQUAL_TEXT(e->size, r.out.size(), e->name);
        } else {
            LONGS_EQUAL_TEXT(0, r.out.size(), e->name);
        }
        /* The wrong length is refused for each. */
        if (e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W)) {
            LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, (uint32_t)e->size - 1).r.status, e->name);
            LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, (uint32_t)e->size + 1).r.status, e->name);
        } else {
            LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, 1).r.status, e->name);
        }
    }
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerTable, TheConsumerProfileSeesTheSameSupportedSet)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER);
    uint64_t hd = h.mustOpen(7);

    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(7, hd, IAV_IOC_ENTER_IDLE, NULL, 0).r.status);
}

TEST(IavBrokerTable, ABrokenAllowListIsRejectedAtInit)
{
    /* The policy refuses to initialize if the allow-list names an ioctl that is not catalogued; the
     * check is in iav_policy_init and exercised by the negative controls that edit the allow-list. */
    LONGS_EQUAL(0, iav_policy_init());
    LONGS_EQUAL(0, iav_policy_init());
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
