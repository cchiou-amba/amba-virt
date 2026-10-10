/*
 * test_virt_camera.c
 *
 * Unit tests for the camera config parser, the module list grammar, the GPIO
 * line request, and the camera pipeline text checks.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "virt_camera_conf.h"
#include "virt_camera_pipeline.h"
#include "virt_gpio.h"
#include "virt_module_loader.h"

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void rm_tree(const char *root)
{
    nftw(root, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void write_file(const char *path, const void *data, size_t len, mode_t mode)
{
    FILE *f = fopen(path, "wb");

    assert(f != NULL);
    assert(fwrite(data, 1, len, f) == len);
    fclose(f);
    chmod(path, mode);
}

static void write_conf(const char *path, const char *text)
{
    write_file(path, text, strlen(text), 0644);
}

static void test_camera_conf(void)
{
    struct virt_camera_conf c;
    char tmp[64] = "/tmp/amba_conf_test_XXXXXX";
    char cf[128];
    unsigned int seen = 0;
    char name[64], params[256];
    FILE *f;
    char line[512];

    printf("[TEST] Camera config and module list examples...\n");
    assert(mkdtemp(tmp) != NULL);
    snprintf(cf, sizeof(cf), "%s/camera.conf", tmp);

    assert(virt_camera_conf_load("./camera.conf.example", &c) == 0);
    assert(strcmp(c.firmware_dir, "/usr/lib/amba-virt/firmware") == 0);
    assert(strcmp(c.resource_script, "/usr/share/ambarella/lua_scripts/n1_655_vin0_1080p_linear.lua") == 0);
    assert(strcmp(c.vout_script, "/usr/share/ambarella/lua_scripts/vout0_dsi.lua") == 0);
    assert(c.app_img_profile == 10);
    assert(strcmp(c.poc_chip, "/dev/gpiochip0") == 0);
    assert(c.poc_line_count == 4 && c.poc_lines[0] == 92 && c.poc_lines[1] == 93 &&
           c.poc_lines[2] == 98 && c.poc_lines[3] == 99);
    assert(c.poc_settle_ms == 4000);
    assert(strcmp(c.early_modules, "/etc/amba-virt/early-modules.conf") == 0);
    assert(strcmp(c.late_modules, "/etc/amba-virt/late-modules.conf") == 0);
    assert(strcmp(c.aaa_bin, "/usr/bin/amba-virt-aaa") == 0);
    assert(strcmp(c.monitor_bin, "/usr/bin/dsp_monitor_service") == 0);

    /* Every line that must be rejected, one at a time. */
    {
        struct virt_camera_conf t;
        static const char *const bad[] = {
            "no_such_key=1", "app_img_profile=11", "app_img_profile=1x", "app_img_profile=-1",
            "app_img_profile=", "firmware_dir=relative/path", "firmware_dir=/a/../b",
            "poc_lines=92 92", "poc_lines=", "poc_lines=1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17",
            "poc_lines=70000", "poc_settle_ms=60001", "poc_chip=gpiochip0", "just a line",
            "=value", "aaa_bin=/usr/bin/x\x01y",
        };

        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            memset(&t, 0, sizeof(t));
            seen = 0;
            assert(virt_camera_conf_parse_line(&t, &seen, bad[i]) < 0);
        }
        memset(&t, 0, sizeof(t));
        seen = 0;
        assert(virt_camera_conf_parse_line(&t, &seen, "app_img_profile=3\r\n") == 1 && t.app_img_profile == 3);
        assert(virt_camera_conf_parse_line(&t, &seen, "app_img_profile=4") < 0); /* repeated key */
        assert(virt_camera_conf_parse_line(&t, &seen, "  # comment") == 0);
        assert(virt_camera_conf_parse_line(&t, &seen, "") == 0);
        assert(virt_camera_conf_parse_line(&t, &seen, "poc_settle_ms=5 # inline comment") == 1 &&
               t.poc_settle_ms == 5);
    }

    /* A file missing a key, or with an over-long line, is rejected whole. */
    write_conf(cf, "firmware_dir=/a\nresource_script=/b\n");
    assert(virt_camera_conf_load(cf, &c) == -EINVAL);
    {
        char *big = malloc(2000);

        assert(big != NULL);
        memset(big, 'a', 1999);
        big[1999] = '\0';
        f = fopen(cf, "w");
        assert(f != NULL);
        fprintf(f, "firmware_dir=/%s\n", big);
        fclose(f);
        free(big);
        assert(virt_camera_conf_load(cf, &c) == -EINVAL);
    }
    assert(virt_camera_conf_load("/nonexistent/camera.conf", &c) == -ENOENT);
    rm_tree(tmp);
    printf("[PASS] conf_example_parses: camera.conf.example loads; %d bad lines, a missing key, and an over-long line rejected\n", 16);

    /* The two module list examples use the modules.conf grammar. */
    {
        static const char *const files[] = { "./early-modules.conf.example", "./late-modules.conf.example" };
        int entries = 0;

        for (size_t i = 0; i < 2; i++) {
            f = fopen(files[i], "r");
            assert(f != NULL);
            while (fgets(line, sizeof(line), f)) {
                int r = virt_module_loader_parse_line(line, name, sizeof(name), params, sizeof(params));

                assert(r >= 0);
                entries += r;
            }
            fclose(f);
        }
        assert(entries == 15);
        assert(virt_module_loader_parse_line("evil/mod.ko\n", name, sizeof(name), params, sizeof(params)) < 0);
        assert(virt_module_loader_parse_line("../mod.ko\n", name, sizeof(name), params, sizeof(params)) < 0);
        assert(virt_module_loader_parse_line("a\\b.ko\n", name, sizeof(name), params, sizeof(params)) < 0);
        assert(virt_module_loader_parse_line("ok.ko x=1/2\n", name, sizeof(name), params, sizeof(params)) < 0);
        printf("[PASS] conf_slash_module_rejected: %d example entries parse; names containing '/' or '..' are rejected\n", entries);
    }
}

static void test_gpio_request(void)
{
    struct gpio_v2_line_request r;
    unsigned int lines[GPIO_V2_LINES_MAX + 1];
    static const unsigned int four[4] = { 92, 93, 98, 99 };

    assert(virt_gpio_build_request(&r, four, 4, 1) == 0);
    assert(r.num_lines == 4 && r.offsets[0] == 92 && r.offsets[3] == 99);
    assert(r.config.flags == GPIO_V2_LINE_FLAG_OUTPUT && r.config.num_attrs == 1);
    assert(r.config.attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
    assert(r.config.attrs[0].attr.values == 0xFULL && r.config.attrs[0].mask == 0xFULL);
    assert(virt_gpio_build_request(&r, four, 4, 0) == 0 && r.config.attrs[0].attr.values == 0);
    for (unsigned int i = 0; i <= GPIO_V2_LINES_MAX; i++)
        lines[i] = i;
    assert(virt_gpio_build_request(&r, lines, GPIO_V2_LINES_MAX, 1) == 0);
    assert(r.config.attrs[0].mask == ~0ULL);
    assert(virt_gpio_build_request(&r, lines, GPIO_V2_LINES_MAX + 1, 1) == -EINVAL);
    assert(virt_gpio_build_request(&r, lines, 0, 1) == -EINVAL);
    assert(virt_gpio_build_request(&r, NULL, 4, 1) == -EINVAL);
    printf("[PASS] gpio_request_built: output request, initial value, 64 line limit, and empty set checked\n");
}

static void test_pipeline_text_checks(void)
{
    char out[64];
    static const char *const full =
        "starting\n"
        "AAA prepare done\n"
        "ADJ parameter version: 1.2\n"
        "AEB parameter version: 3.4\n";

    assert(virt_camera_log_has_handshake(full) == 1);
    assert(virt_camera_log_has_handshake("AAA prepare done\nADJ parameter version\n") == 0);
    assert(virt_camera_log_has_handshake("ADJ parameter version\nAEB parameter version\n") == 0);
    assert(virt_camera_log_has_handshake("") == 0);
    assert(virt_camera_log_has_handshake(NULL) == 0);

    assert(virt_camera_log_fell_back("Can't find file: /usr/share/ambarella/idsp/robot/os08a10.rgb.linear.liso.adj_param\n") == 1);
    assert(virt_camera_log_fell_back("ok\nCan't find file: /x/os08a10.rgb.linear.liso.aeb_param\nmore\n") == 1);
    assert(virt_camera_log_fell_back("Can't find file: /x/some_other_sensor.adj_param\n") == 0);
    assert(virt_camera_log_fell_back("loaded /x/os08a10.rgb.linear.liso.adj_param\n") == 0);
    assert(virt_camera_log_fell_back("") == 0);
    assert(virt_camera_log_fell_back(NULL) == 0);

    assert(virt_camera_kmsg_has_failure("6,1,2,-;ambnl: no active user for port 27\n") == 1);
    assert(virt_camera_kmsg_has_failure("6,1,2,-;imgproc: empty ISO cfg\n") == 1);
    assert(virt_camera_kmsg_has_failure("6,1,2,-;nothing wrong here\n") == 0);
    assert(virt_camera_kmsg_has_failure(NULL) == 0);

    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware",
                                   "/usr/lib/amba-virt/firmware/ambarella/n1_655/dsp/x/orccode.bin",
                                   out, sizeof(out)) == 0);
    assert(strcmp(out, "ambarella/n1_655/dsp/x/orccode.bin") == 0);
    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware/", "/usr/lib/amba-virt/firmware/cavalry.bin",
                                   out, sizeof(out)) == 0 && strcmp(out, "cavalry.bin") == 0);
    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware", "/usr/lib/amba-virt/firmware",
                                   out, sizeof(out)) == -EINVAL);
    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware", "/usr/lib/amba-virt/firmware/",
                                   out, sizeof(out)) == -EINVAL);
    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware", "/usr/lib/amba-virt/firmware2/x.bin",
                                   out, sizeof(out)) == -EINVAL);
    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware", "/etc/passwd", out, sizeof(out)) == -EINVAL);
    assert(virt_camera_fw_rel_path("/usr/lib/amba-virt/firmware", "/usr/lib/amba-virt/firmware/cavalry.bin",
                                   out, 5) == -EINVAL);
    assert(virt_camera_fw_rel_path(NULL, "/a", out, sizeof(out)) == -EINVAL);
    printf("[PASS] pipeline_text_checks: 3A handshake, tuning fallback, kernel message, and firmware path checks\n");
}

int main(void)
{
    /* An assert aborts without flushing: keep the log of passed cases complete. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== Running Camera Test Suite ===\n");
    test_camera_conf();
    test_gpio_request();
    test_pipeline_text_checks();
    printf("=== All Camera Tests Passed ===\n\n");
    return 0;
}
