/*
 * TestUsbWebcamControl.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../UsbWebcamControl.hxx"

#include <errno.h>
#include <string.h>

TEST_GROUP(UsbWebcamControlParse)
{
};

static int backendOf(const char *body, int *out)
{
    return UsbWebcamControl::parseBackend(body, strlen(body), out);
}

static int exposureOf(const char *body, UsbWebcamControl::Exposure *out)
{
    return UsbWebcamControl::parseExposure(body, strlen(body), out);
}

TEST(UsbWebcamControlParse, Backend)
{
    int b = -1;

    LONGS_EQUAL(0, backendOf("{\"backend\":\"hw\"}", &b));
    LONGS_EQUAL(UsbWebcamControl::BackendHardware, b);
    LONGS_EQUAL(0, backendOf("{ \"backend\" : \"hardware\" }", &b));
    LONGS_EQUAL(UsbWebcamControl::BackendHardware, b);
    LONGS_EQUAL(0, backendOf("{\"backend\":\"cpu\"}", &b));
    LONGS_EQUAL(UsbWebcamControl::BackendSoftware, b);
    LONGS_EQUAL(0, backendOf("{\"backend\":\"software\"}", &b));
    LONGS_EQUAL(UsbWebcamControl::BackendSoftware, b);
}

TEST(UsbWebcamControlParse, BackendRefusals)
{
    int b = -1;

    LONGS_EQUAL(-EINVAL, backendOf("", &b));
    LONGS_EQUAL(-EINVAL, backendOf("{}", &b));
    LONGS_EQUAL(-EINVAL, backendOf("{\"backend\":\"gpu\"}", &b));
    LONGS_EQUAL(-EINVAL, backendOf("{\"backend\":\"hw", &b));
    LONGS_EQUAL(-EINVAL, backendOf("hw", &b));
    LONGS_EQUAL(-EINVAL, UsbWebcamControl::parseBackend(nullptr, 0, &b));
    LONGS_EQUAL(-EINVAL, UsbWebcamControl::parseBackend("{\"backend\":\"hw\"}", 16, nullptr));
    LONGS_EQUAL(-1, b);
}

TEST(UsbWebcamControlParse, Exposure)
{
    UsbWebcamControl::Exposure e{};

    LONGS_EQUAL(0, exposureOf("{\"mode\":\"auto\"}", &e));
    LONGS_EQUAL(UsbWebcamControl::ExposureAuto, e.mode);
    LONGS_EQUAL(-1, e.gain);
    LONGS_EQUAL(0, exposureOf("{\"mode\":\"fixed\",\"gain\":200}", &e));
    LONGS_EQUAL(UsbWebcamControl::ExposureFixed, e.mode);
    LONGS_EQUAL(200, e.gain);
    LONGS_EQUAL(-1, e.exposureAbs);
    LONGS_EQUAL(0, exposureOf("{\"mode\": \"fixed\", \"exposure\": 300, \"gain\": 64}", &e));
    LONGS_EQUAL(300, e.exposureAbs);
    LONGS_EQUAL(64, e.gain);
}

TEST(UsbWebcamControlParse, ExposureRefusals)
{
    UsbWebcamControl::Exposure e{};

    LONGS_EQUAL(-EINVAL, exposureOf("", &e));
    LONGS_EQUAL(-EINVAL, exposureOf("{\"mode\":\"manual\"}", &e));
    LONGS_EQUAL(-EINVAL, exposureOf("{\"mode\":\"fixed\",\"gain\":-5}", &e));
    LONGS_EQUAL(-EINVAL, exposureOf("{\"mode\":\"fixed\",\"gain\":abc}", &e));
    LONGS_EQUAL(-EINVAL, exposureOf("{\"mode\":\"fixed\",\"gain\":99999999999999999999}", &e));
    LONGS_EQUAL(-EINVAL, exposureOf("{\"mode\":\"fixed\",\"exposure\":1e3}", &e));
    LONGS_EQUAL(-EINVAL, exposureOf("{\"gain\":10}", &e));
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
