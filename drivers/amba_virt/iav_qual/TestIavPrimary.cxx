/*
 * TestIavPrimary.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The read-only ioctls of the primary set (configuration and VIN reads). Each is called with an
 * argument that names the first object (stream 0, vsrc 0) and the result is recorded: its hash when
 * the call succeeds, the negative errno when it fails. A run through the virtual /dev/iav and a
 * native run record the same values for the same pipeline state, which is how the Envelope 5
 * comparison checks that the broker passes the real driver's answer through unchanged.
 *
 * What is judged depends on the role the run is told it has (--role): a primary client must not be
 * refused (no EPERM, ENOTTY or EMSGSIZE) and at least one call must succeed, so the test cannot
 * pass by seeing nothing; a consumer must be refused (EPERM) on every call. Without a role, the
 * calls are recorded and nothing is judged. Only reads are made: nothing here changes the pipeline.
 */

#include "IavTestSupport.hxx"

#include <ambas_imgproc_ioctl_arch.h>

#include <CppUTest/TestHarness.h>

#include <stdint.h>

using namespace iavtest;

namespace {

struct Probe
{
    const char *name;
    unsigned long cmd;
    size_t size;
};

#define PROBE(cmd, type) { #cmd, cmd, sizeof(type) }

const Probe Probes[] = {
    PROBE(IAV_IOC_GET_STREAM_RESOURCE, struct iav_stream_resource),
    PROBE(IAV_IOC_GET_VCAP_CFG, struct iav_vcap_cfg),
    PROBE(IAV_IOC_GET_PYRAMID_CFG, struct iav_pyramid_cfg),
    PROBE(IAV_IOC_GET_MJPEG_CONFIG, struct iav_mjpeg_cfg),
    PROBE(IAV_IOC_GET_POSTP_SCALE_CTRL, struct iav_postp_scale),
    PROBE(IAV_IOC_GET_AUDIT_CONFIG, struct iav_audit_cfg),
    PROBE(IAV_IOC_GET_DSP_HEALTH_CONFIG, struct iav_dsp_health_cfg),
    PROBE(IAV_IOC_GET_VIN_COMPAND_CFG, struct iav_vin_compand_config),
    PROBE(IAV_IOC_GET_MEM_LAYOUT_RESOURCE, struct iav_mem_layout_resource),
    PROBE(IAV_IOC_GET_RAW_ENCODE, struct iav_raw_enc_setup),
    PROBE(IAV_IOC_VIN_GET_GLOBAL_INFO, struct vin_global_info),
    PROBE(IAV_IOC_VIN_GET_MODE, struct vindev_mode),
    PROBE(IAV_IOC_VIN_GET_FPS, struct vindev_fps),
    PROBE(IAV_IOC_VIN_GET_VIDEOINFO, struct vindev_video_info),
    PROBE(IAV_IOC_VIN_GET_DEVINFO, struct vindev_devinfo),
    PROBE(IAV_IOC_VIN_GET_MIRROR, struct vindev_mirror),
    PROBE(IAV_IOC_IMG_GET_STATISTICS_REPORT_RATE, struct img_statistics_report_rate),
};

uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;

    while (n--) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

TEST_GROUP(IavPrimary)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavPrimary, ReadOnlyConfigurationCallsFollowTheRole)
{
    const std::string &role = ctx().config()._role;
    size_t served = 0, refused = 0;

    for (size_t i = 0; i < sizeof(Probes) / sizeof(Probes[0]); i++) {
        const Probe &p = Probes[i];
        uint8_t buf[4096];
        int rc, err;

        CHECK_TEXT(p.size <= sizeof(buf), p.name);
        memset(buf, 0, sizeof(buf));
        errno = 0;
        rc = ioctlPtr(_f.fd(), p.cmd, buf);
        err = rc < 0 ? errno : 0;
        /* The hash when served, the negative errno when not: one note per call. */
        note(p.name, rc < 0 ? -(long long)err : (long long)(fnv1a(buf, p.size) >> 1));
        if (rc >= 0) {
            served++;
        }
        if (rc < 0 && err == EPERM) {
            refused++;
        }
        if (role == "primary") {
            CHECK_TEXT(rc >= 0 || (err != EPERM && err != ENOTTY && err != EMSGSIZE), p.name);
        } else if (role == "consumer") {
            CHECK_EQUAL_TEXT(-1, rc, p.name);
            CHECK_EQUAL_TEXT(EPERM, err, p.name);
        }
    }
    note("probes", (long long)(sizeof(Probes) / sizeof(Probes[0])));
    note("served", (long long)served);
    note("refused", (long long)refused);
    if (role == "primary") {
        CHECK_TEXT(served >= 1, "a primary client saw no call served");
    }
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
