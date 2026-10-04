/*
 * drivers/amba_virt/tools/test/TestHostCavalryEveSync.cxx
 *
 * CppUTest test suite for Dynamic EVE Xen CID auto-rebinding across HVM guest reboots.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "CppUTest/TestHarness.h"
#include "CppUTest/CommandLineTestRunner.h"

#include "cavalry_proxy.h"

TEST_GROUP(CavalryEveSync)
{
    char tmp_dir[256];

    void setup() override
    {
        snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/mock_xen_test_%d", getpid());
        mkdir(tmp_dir, 0755);

        cavalry_proxy_init(-1, nullptr, 0x80000000ULL, 0x100000000ULL);
        cavalry_proxy_set_eve_xen_dir(tmp_dir);
    }

    void teardown() override
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_dir);
        (void)system(cmd);

        cavalry_proxy_cleanup();
        cavalry_proxy_set_eve_xen_dir(nullptr); // restore default
    }

    void write_xen_cfg(const char *filename, const char *uuid, uint32_t cid)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", tmp_dir, filename);
        FILE *f = fopen(path, "w");
        CHECK_TRUE(f != nullptr);

        fprintf(f, "# EVE Xen configuration file for HVM domain\n");
        fprintf(f, "name = \"%s.1.0\"\n", uuid);
        fprintf(f, "uuid = \"%s\"\n", uuid);
        fprintf(f, "device_model_args = [ \"-device\", \"vhost-vsock-pci,guest-cid=\\\"%u\\\"\" ]\n", cid);
        fclose(f);
    }
};

/*
 * Test 1: Xen config parsing and binding of Ubuntu and Alpine UUIDs to correct slots
 */
TEST(CavalryEveSync, test_eve_xen_config_parsing_and_slot_binding)
{
    // xen7.cfg -> Ubuntu 24.04 (slot 0)
    write_xen_cfg("xen7.cfg", "6b43e816-fbba-4abf-a2df-81964577931c", 3266);

    // xen1.cfg -> Alpine 3.20 (slot 1)
    write_xen_cfg("xen1.cfg", "2721e105-6011-4b5e-81c6-3b18f4cec9e3", 3265);

    int ret = cavalry_proxy_sync_eve_cids();
    LONGS_EQUAL(0, ret);

    struct cavalry_tenant_ctx *t0 = cavalry_proxy_get_tenant(3266);
    CHECK_TRUE(t0 != nullptr);
    LONGS_EQUAL(3266, t0->cid);
    LONGS_EQUAL(0, t0->tenant_idx);
    LONGS_EQUAL(0, t0->slice_offset);

    struct cavalry_tenant_ctx *t1 = cavalry_proxy_get_tenant(3265);
    CHECK_TRUE(t1 != nullptr);
    LONGS_EQUAL(3265, t1->cid);
    LONGS_EQUAL(1, t1->tenant_idx);
    LONGS_EQUAL(0x40000000U, t1->slice_offset);
}

/*
 * Test 2: Dynamic CID migration across guest reboot
 * Simulates Ubuntu reboot (CID 3266 -> 3270) and Alpine reboot (CID 3265 -> 3275).
 * Asserts tenant slots update CIDs while preserving physical memory slices.
 */
TEST(CavalryEveSync, test_dynamic_cid_migration_across_reboots)
{
    write_xen_cfg("xen7.cfg", "6b43e816-fbba-4abf-a2df-81964577931c", 3266);
    write_xen_cfg("xen1.cfg", "2721e105-6011-4b5e-81c6-3b18f4cec9e3", 3265);
    cavalry_proxy_sync_eve_cids();

    // Guest reboots: EVE assigns new guest-cids
    write_xen_cfg("xen7.cfg", "6b43e816-fbba-4abf-a2df-81964577931c", 3270);
    write_xen_cfg("xen1.cfg", "2721e105-6011-4b5e-81c6-3b18f4cec9e3", 3275);

    // Dynamic auto-sync via cavalry_proxy_get_tenant
    struct cavalry_tenant_ctx *t0 = cavalry_proxy_get_tenant(3270);
    CHECK_TRUE(t0 != nullptr);
    LONGS_EQUAL(3270, t0->cid);
    LONGS_EQUAL(0, t0->tenant_idx);
    LONGS_EQUAL(0, t0->slice_offset);

    struct cavalry_tenant_ctx *t1 = cavalry_proxy_get_tenant(3275);
    CHECK_TRUE(t1 != nullptr);
    LONGS_EQUAL(3275, t1->cid);
    LONGS_EQUAL(1, t1->tenant_idx);
    LONGS_EQUAL(0x40000000U, t1->slice_offset);
}

/*
 * Test 3: Foreign domain rejection
 * Non-Ambarella VM domains (e.g. Windows 11) must be ignored and not bound to slots.
 */
TEST(CavalryEveSync, test_foreign_domain_rejection)
{
    write_xen_cfg("xen7.cfg", "6b43e816-fbba-4abf-a2df-81964577931c", 3266);
    write_xen_cfg("xen1.cfg", "2721e105-6011-4b5e-81c6-3b18f4cec9e3", 3265);

    // Foreign Windows domain with unrelated UUID
    write_xen_cfg("xen3.cfg", "1ff27012-4e1d-437f-83c4-8db12f6bc6b2", 3260);

    cavalry_proxy_sync_eve_cids();

    // Foreign CID must not have been bound to tenant slot 0 or 1
    struct cavalry_tenant_ctx *t0 = cavalry_proxy_get_tenant(3266);
    CHECK_TRUE(t0 != nullptr);
    LONGS_EQUAL(0, t0->tenant_idx);

    struct cavalry_tenant_ctx *t1 = cavalry_proxy_get_tenant(3265);
    CHECK_TRUE(t1 != nullptr);
    LONGS_EQUAL(1, t1->tenant_idx);
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
