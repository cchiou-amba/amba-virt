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

#include <vector>

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
        /* The primary profile is never the server-wide one, so it is always a subset of qualification. */
        if (inProfile(e, IAV_PROFILE_PRIMARY)) {
            CHECK_TEXT(inProfile(e, IAV_PROFILE_QUALIFICATION), e->name);
        }
        if (inProfile(e, IAV_PROFILE_CONSUMER)) {
            CHECK_TEXT(inProfile(e, IAV_PROFILE_PRIMARY), e->name);
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
    /* Larger than any argument struct: some (the statistics and debug calls) are far bigger than a message. */
    size_t biggest = AMBA_VIRT_MAX_MSG;
    size_t denied = 0;

    for (size_t i = 0; i < iav_policy_count(); i++) {
        if (iav_policy_at(i)->size > biggest) {
            biggest = iav_policy_at(i)->size;
        }
    }
    std::vector<uint8_t> argv(biggest, 0);
    uint8_t *arg = &argv[0];

    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);
        IavBrokerHarness::Result r;

        if (inProfile(e, IAV_PROFILE_CONSUMER)) {
            continue;
        }
        CHECK_TEXT(e->size <= argv.size(), e->name);
        denied++;
        /* With the shape the driver would take, and with the wrong shape: both are refused. */
        r = h.ioctl(7, hd, e->cmd, e->kind == IAV_ARG_PTR ? arg : NULL,
                    e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ? (uint32_t)e->size : 0);
        /* An argument larger than a message cannot be sent at all: refused at the framing, still never run. */
        LONGS_EQUAL_TEXT(e->size > AMBA_VIRT_IAV_MAX_ARG && e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ?
                         -EMSGSIZE : -EPERM, r.r.status, e->name);
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

/* ---- the primary client ---- */

namespace {

/* Ioctls no profile may ever allow in this release, from the review, independent of the generated list. */
const unsigned long NeverAllowed[] = {
    IAV_IOC_ENABLE_DRAM_TEST,     IAV_IOC_CUSTOM_CMDS,          IAV_IOC_RESET,
    IAV_IOC_GET_DEBUG_CONFIG,     IAV_IOC_SET_DEBUG_CONFIG,     IAV_IOC_SET_DSP_LOG,
    IAV_IOC_ALLOC_MEM_PART,       IAV_IOC_ALLOC_ANON_MEM_PART,  IAV_IOC_REGISTER_ANON_MEM_PART,
    IAV_IOC_FEED_CANVAS_BUF,      IAV_IOC_FEED_PYRAMID_BUF,     IAV_IOC_FEED_RAW_BUF,
    IAV_IOC_SET_MEM_LAYOUT_RESOURCE, IAV_IOC_SET_RAW_ENCODE,    IAV_IOC_GDMA_COPY,
    IAV_IOC_SET_IMG_SCALE,        IAV_IOC_VIN_SET_DEVINFO,      IAV_IOC_EFM_SET_EXT_POOL,
};

/* Not PLAIN by the table, but their "offset" fields are not memory offsets (the field comments were read). */
const unsigned long NotMemoryOffsets[] = {
    IAV_IOC_SET_CANVAS_CONFIG,    IAV_IOC_SET_CHAN_CONFIG,      IAV_IOC_GET_H264_CONFIG,
    IAV_IOC_SET_H264_CONFIG,      IAV_IOC_GET_H265_CONFIG,      IAV_IOC_SET_H265_CONFIG,
    IAV_IOC_VIN_AAA_COMPENSATION, IAV_IOC_VIN_GET_AAAINFO,      IAV_IOC_VIN_GET_MSYNC,
    IAV_IOC_VIN_SET_MSYNC,        IAV_IOC_VIN_GET_VIRT_MODE,    IAV_IOC_VIN_SET_VIRT_MODE,
    IAV_IOC_VIN_GET_WDR_AGAIN_INDEX_GP, IAV_IOC_VIN_GET_WDR_DGAIN_INDEX_GP,
    IAV_IOC_VIN_GET_WDR_SHUTTER_ROW_GP, IAV_IOC_VIN_SET_WDR_GP, IAV_IOC_VIN_WDR_SHUTTER2OFFSET,
    IAV_IOC_VIN_WDR_SHUTTER2ROW,
};

/* The calls with an argument checked against a window: decode (version 3 of the allow-list) and overlay (version 4). */
const unsigned long DecodeChecked[] = {
    IAV_IOC_CREATE_DECODER,  IAV_IOC_DECODE_JPEG,         IAV_IOC_DECODE_VIDEO,   IAV_IOC_QUERY_DECODE_BSB,
    IAV_IOC_WAIT_DECODE_BSB, IAV_IOC_QUERY_DECODE_STATUS, IAV_IOC_WAIT_PLAYBACK_EOS,
    IAV_IOC_SET_OVERLAY_INSERT, IAV_IOC_GET_OVERLAY_INSERT,
};

bool decodeChecked(unsigned int cmd)
{
    for (size_t i = 0; i < sizeof(DecodeChecked) / sizeof(DecodeChecked[0]); i++) {
        if (DecodeChecked[i] == cmd) {
            return true;
        }
    }
    return false;
}

bool notMemoryOffset(unsigned int cmd)
{
    for (size_t i = 0; i < sizeof(NotMemoryOffsets) / sizeof(NotMemoryOffsets[0]); i++) {
        if (NotMemoryOffsets[i] == cmd) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_GROUP(IavBrokerPrimary)
{
    void setup() { LONGS_EQUAL(0, iav_policy_init()); }
};

TEST(IavBrokerPrimary, ThePrimarySetIsMuchLargerThanTheConsumerSet)
{
    size_t consumer = 0, primary = 0;

    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        consumer += inProfile(e, IAV_PROFILE_CONSUMER);
        primary += inProfile(e, IAV_PROFILE_PRIMARY);
    }
    LONGS_EQUAL(15, consumer);
    /* An exact count: a change to the set is a decision, and this line is where it is recorded. */
    LONGS_EQUAL(128, primary);
}

TEST(IavBrokerPrimary, NoProfileAllowsTheHazardousIoctls)
{
    for (size_t k = 0; k < sizeof(NeverAllowed) / sizeof(NeverAllowed[0]); k++) {
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)NeverAllowed[k]);

        CHECK(e != NULL);
        LONGS_EQUAL_TEXT(0, e->profiles, e->name);
    }
}

TEST(IavBrokerPrimary, ThePrimarySetHasNoAddressPointerFdOrDebugIoctl)
{
    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        if (!inProfile(e, IAV_PROFILE_PRIMARY) || inProfile(e, IAV_PROFILE_CONSUMER)) {
            continue;
        }
        /* The decode calls name offsets in the decode window, or (the status) report addresses that are cleared:
         * they are allowed with a check function, and TestHostIavBrokerDecode.cxx tests each. */
        if (decodeChecked(e->cmd)) {
            CHECK_TEXT(e->check != NULL, e->name);
            continue;
        }
        CHECK_TEXT(e->cls == IAV_CLS_PLAIN || notMemoryOffset(e->cmd), e->name);
        CHECK_TEXT((e->cls & (IAV_CLS_PHYS | IAV_CLS_POINTER)) == 0, e->name);
        CHECK_TEXT((e->flags & IAV_F_DEBUG) == 0, e->name);
        CHECK_TEXT(e->size <= AMBA_VIRT_IAV_MAX_ARG, e->name);
    }
}

TEST(IavBrokerPrimary, OnlyThePrimaryGuestMayConfigureThePipeline)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t primary = h.mustOpen(7);
    uint64_t other = h.mustOpen(8);
    uint8_t arg[AMBA_VIRT_MAX_MSG];

    memset(arg, 0, sizeof(arg));
    LONGS_EQUAL(0, h.ioctl(7, primary, IAV_IOC_ENTER_IDLE, arg, sizeof(u8)).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(8, other, IAV_IOC_ENTER_IDLE, arg, sizeof(u8)).r.status);
    /* Both may still query. */
    LONGS_EQUAL(0, h.ioctl(8, other, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
    LONGS_EQUAL(0, h.ioctl(7, primary, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerPrimary, NoPrimaryMeansNoOneMayConfigureThePipeline)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER);
    uint64_t hd = h.mustOpen(7);
    uint8_t arg[16] = {0};

    LONGS_EQUAL(-EPERM, h.ioctl(7, hd, IAV_IOC_ENTER_IDLE, arg, sizeof(u8)).r.status);
    LONGS_EQUAL(0, h.drv.ioctlCalls());
}

TEST(IavBrokerPrimary, ThePrimaryGuestStillCannotCallTheHazardousIoctls)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd = h.mustOpen(7);
    uint8_t arg[AMBA_VIRT_MAX_MSG];

    memset(arg, 0, sizeof(arg));
    for (size_t k = 0; k < sizeof(NeverAllowed) / sizeof(NeverAllowed[0]); k++) {
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)NeverAllowed[k]);

        LONGS_EQUAL_TEXT(-EPERM, h.ioctl(7, hd, e->cmd, e->kind == IAV_ARG_PTR ? arg : NULL,
                                         e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ? (uint32_t)e->size : 0).r.status,
                         e->name);
    }
    LONGS_EQUAL(0, h.drv.ioctlCalls());
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerPrimary, EveryPrimaryIoctlWorksForThePrimaryAndTheWrongLengthIsRefused)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd = h.mustOpen(7);
    uint8_t arg[AMBA_VIRT_MAX_MSG];
    size_t n = 0;

    memset(arg, 0, sizeof(arg));
    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);
        IavBrokerHarness::Result r;

        if (!inProfile(e, IAV_PROFILE_PRIMARY)) {
            continue;
        }
        n++;
        if (decodeChecked(e->cmd)) {
            /* A zero argument is refused by the call's own check; the valid shapes are tested in
             * TestHostIavBrokerDecode.cxx. The wrong length is still refused at the framing. */
            if (e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W)) {
                LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, (uint32_t)e->size - 1).r.status, e->name);
                LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, (uint32_t)e->size + 1).r.status, e->name);
            }
            continue;
        }
        r = h.ioctl(7, hd, e->cmd, e->kind == IAV_ARG_PTR ? arg : NULL,
                    e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ? (uint32_t)e->size : 0, 1);
        LONGS_EQUAL_TEXT(0, r.r.status, e->name);
        if (e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W)) {
            LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, (uint32_t)e->size - 1).r.status, e->name);
            LONGS_EQUAL_TEXT(-EMSGSIZE, h.ioctl(7, hd, e->cmd, arg, (uint32_t)e->size + 1).r.status, e->name);
        }
    }
    LONGS_EQUAL(128, n);
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerPrimary, AnotherGuestAndThePrimaryAreIndependent)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t other = h.mustOpen(9);
    uint8_t arg[16] = {0};

    /* A guest cannot claim to be the primary: the id is the kernel's, not the request's. */
    LONGS_EQUAL(-EPERM, h.ioctl(9, other, IAV_IOC_START_ENCODE, NULL, 0, 1).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(9, other, IAV_IOC_SET_SYSTEM_RESOURCE, arg, 1).r.status);
    LONGS_EQUAL(0, h.drv.ioctlCalls());
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
