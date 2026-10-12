/*
 * TestHostIavBrokerProtocol.cxx
 *
 * Malformed, truncated, oversized and unknown requests. Every one must get a refusal and none
 * may reach the driver.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <limits.h>
#include <poll.h>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

TEST_GROUP(IavBrokerProtocol)
{
    IavBrokerHarness *h;
    uint64_t handle;

    void setup()
    {
        h = new IavBrokerHarness();
        LONGS_EQUAL(0, h->initRc());
        LONGS_EQUAL(0, h->windowsRc());
        handle = h->mustOpen(7);
        CHECK(handle != 0);
    }

    void teardown()
    {
        CHECK(h->drv.violations().empty());
        delete h;
    }

    void expectRefused(const IavBrokerHarness::Result &r, int err)
    {
        LONGS_EQUAL(0, r.ret);
        LONGS_EQUAL(-err, r.r.status);
        LONGS_EQUAL(AMBA_VIRT_IAV_PROTO, r.r.proto);
        LONGS_EQUAL(0, r.r.reserved);
        LONGS_EQUAL(0, r.r.out_len);
    }
};

TEST(IavBrokerProtocol, EmptyAndShortRequests)
{
    uint8_t buf[64];
    size_t lens[] = {0, 1, 7, 8, sizeof(struct amba_virt_iav_req) - 1};

    memset(buf, 0xab, sizeof(buf));
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        expectRefused(h->send(7, buf, lens[i]), EPROTO);
    }
    expectRefused(h->send(7, NULL, 0), EPROTO);
}

TEST(IavBrokerProtocol, WrongProtocolVersion)
{
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0);
    struct amba_virt_iav_req q;

    memcpy(&q, &v[0], sizeof(q));
    q.proto = AMBA_VIRT_IAV_PROTO + 1;
    memcpy(&v[0], &q, sizeof(q));
    expectRefused(h->send(7, &v[0], v.size()), EPROTO);
    q.proto = 0;
    memcpy(&v[0], &q, sizeof(q));
    expectRefused(h->send(7, &v[0], v.size()), EPROTO);
}

TEST(IavBrokerProtocol, UnknownOperations)
{
    uint32_t ops[] = {0, 7, 8, 100, 0x7fffffffu, 0xffffffffu};

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        std::vector<uint8_t> v = IavBrokerHarness::build(ops[i], handle, 0, 0, 0, NULL, 0);

        expectRefused(h->send(7, &v[0], v.size()), ENOSYS);
    }
}

TEST(IavBrokerProtocol, ArgLengthMustMatchThePayloadExactly)
{
    uint8_t arg[16] = {0};
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_IOCTL, handle,
                                                     (uint32_t)IAV_IOC_GET_IAV_STATE, 0, 0, arg, 4);
    struct amba_virt_iav_req q;

    memcpy(&q, &v[0], sizeof(q));
    /* The header says 4 bytes follow, but 8 do, then 0 do. */
    q.arg_len = 4;
    v.resize(sizeof(q) + 8);
    memcpy(&v[0], &q, sizeof(q));
    expectRefused(h->send(7, &v[0], v.size()), EMSGSIZE);
    v.resize(sizeof(q));
    expectRefused(h->send(7, &v[0], v.size()), EMSGSIZE);
    /* The header claims a huge length with a short payload. */
    q.arg_len = 0xffffffffu;
    memcpy(&v[0], &q, sizeof(q));
    expectRefused(h->send(7, &v[0], v.size()), EMSGSIZE);
    q.arg_len = INT_MAX;
    memcpy(&v[0], &q, sizeof(q));
    expectRefused(h->send(7, &v[0], v.size()), EMSGSIZE);
}

TEST(IavBrokerProtocol, ArgLengthAboveTheProtocolMaximum)
{
    std::vector<uint8_t> big(sizeof(struct amba_virt_iav_req) + AMBA_VIRT_IAV_MAX_ARG + 1, 0);
    struct amba_virt_iav_req q;

    q.proto = AMBA_VIRT_IAV_PROTO;
    q.op = AMBA_VIRT_IAV_OP_IOCTL;
    q.handle = handle;
    q.cmd = (uint32_t)IAV_IOC_GET_IAV_STATE;
    q.arg_len = AMBA_VIRT_IAV_MAX_ARG + 1;
    q.arg_value = 0;
    q.arg2 = 0;
    memcpy(&big[0], &q, sizeof(q));
    expectRefused(h->send(7, &big[0], big.size()), EMSGSIZE);
}

TEST(IavBrokerProtocol, ZeroLengthWhereAStructIsRequired)
{
    LONGS_EQUAL(0, h->drv.reached(IAV_IOC_QUERY_MEMBLOCK));
    expectRefused(h->ioctl(7, handle, IAV_IOC_QUERY_MEMBLOCK, NULL, 0), EMSGSIZE);
}

TEST(IavBrokerProtocol, StructLengthOffByOne)
{
    struct iav_querymem q;
    uint8_t buf[sizeof(q) + 8];

    memset(buf, 0, sizeof(buf));
    expectRefused(h->ioctl(7, handle, IAV_IOC_QUERY_MEMBLOCK, buf, sizeof(q) - 1), EMSGSIZE);
    expectRefused(h->ioctl(7, handle, IAV_IOC_QUERY_MEMBLOCK, buf, sizeof(q) + 1), EMSGSIZE);
    expectRefused(h->ioctl(7, handle, IAV_IOC_QUERY_MEMBLOCK, buf, sizeof(q) + 8), EMSGSIZE);
}

TEST(IavBrokerProtocol, OutputOnlyIoctlTakesNoInput)
{
    u32 state = 0;

    expectRefused(h->ioctl(7, handle, IAV_IOC_GET_IAV_STATE, &state, sizeof(state)), EMSGSIZE);
}

TEST(IavBrokerProtocol, ValueIoctlTakesNoStruct)
{
    uint8_t arg[8] = {1};

    expectRefused(h->ioctl(7, handle, IAV_IOC_WAIT_NEXT_IDSP_FRAME, arg, sizeof(arg), 1), EMSGSIZE);
}

TEST(IavBrokerProtocol, NoArgIoctlTakesNoStruct)
{
    uint8_t arg[4] = {0};

    expectRefused(h->ioctl(7, handle, IAV_IOC_CLEAR_POLL_EVENT_CFG, arg, sizeof(arg)), EMSGSIZE);
}

TEST(IavBrokerProtocol, UnknownIoctlNumbersAreNotTTY)
{
    unsigned long cmds[] = {0, 1, 0xdeadbeefUL, _IO('V', 0xfe), _IO('Z', 1), 0xffffffffUL,
                            _IOWR('V', 0x7f, long), IAV_IOC_GET_IAV_STATE ^ (1ul << 16)};

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        expectRefused(h->ioctl(7, handle, cmds[i], NULL, 0), ENOTTY);
    }
    LONGS_EQUAL(0, h->drv.ioctlCalls());
}

TEST(IavBrokerProtocol, KnownButDeniedIoctlsAreRefusedAndNeverForwarded)
{
    /* State-changing, address-bearing and pointer-bearing ioctls that are not supported. */
    unsigned long cmds[] = {IAV_IOC_ENTER_IDLE, IAV_IOC_ENABLE_PREVIEW, IAV_IOC_START_ENCODE,
                            IAV_IOC_STOP_ENCODE, IAV_IOC_ABORT_ENCODE, IAV_IOC_RESET,
                            IAV_IOC_ENABLE_DRAM_TEST, IAV_IOC_GDMA_COPY, IAV_IOC_SET_IMG_SCALE,
                            IAV_IOC_SET_RAW_ENC_CFG, IAV_IOC_SET_ISP_NN_CFG, IAV_IOC_SET_DSP_LOG,
                            IAV_IOC_SET_SYSTEM_RESOURCE, IAV_IOC_ALLOC_MEM_PART};

    /* For a consumer. The qualification and primary profiles allow the state-changing ones; the
     * hazardous ones are refused for every profile (TestHostIavBrokerTable.cxx). */
    IavBrokerHarness consumer(IAV_PROFILE_CONSUMER);
    uint64_t ch = consumer.mustOpen(7);

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        expectRefused(consumer.ioctl(7, ch, cmds[i], NULL, 0), EPERM);
    }
    LONGS_EQUAL(0, consumer.drv.ioctlCalls());
}

TEST(IavBrokerProtocol, RequestsFromManyIdsDoNotInterfere)
{
    expectRefused(h->send(0, "", 0), EPROTO);
    expectRefused(h->send(0xffffffffu, "", 0), EPROTO);
    CHECK(h->info(0).r.status == 0);
    CHECK(h->info(0xffffffffu).r.status == 0);
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
