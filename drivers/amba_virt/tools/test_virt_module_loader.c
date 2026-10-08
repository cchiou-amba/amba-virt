/*
 * test_virt_module_loader.c
 *
 * Unit tests for virt_module_loader configuration and line parser.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virt_module_loader.h"

int main(void)
{
    printf("=== Running virt_module_loader Unit Tests ===\n");

    char name[64];
    char params[256];
    int ret;

    /* 1. amba_virt.ko yields that name and an empty parameter string */
    memset(name, 0, sizeof(name));
    memset(params, 0, sizeof(params));
    ret = virt_module_loader_parse_line("amba_virt.ko", name, sizeof(name), params, sizeof(params));
    assert(ret == 1);
    assert(strcmp(name, "amba_virt.ko") == 0);
    assert(strcmp(params, "") == 0);
    printf("[PASS] amba_virt.ko parsed correctly (name='%s', params='%s')\n", name, params);

    /* 2. iav.ko stream_buf_size=0x4000000 yields that name and parameter string */
    memset(name, 0, sizeof(name));
    memset(params, 0, sizeof(params));
    ret = virt_module_loader_parse_line("iav.ko stream_buf_size=0x4000000", name, sizeof(name), params, sizeof(params));
    assert(ret == 1);
    assert(strcmp(name, "iav.ko") == 0);
    assert(strcmp(params, "stream_buf_size=0x4000000") == 0);
    printf("[PASS] iav.ko with params parsed correctly (name='%s', params='%s')\n", name, params);

    /* 3. a comment and a blank line each yield no entry */
    memset(name, 0xaa, sizeof(name));
    memset(params, 0xaa, sizeof(params));
    ret = virt_module_loader_parse_line("# This is a comment", name, sizeof(name), params, sizeof(params));
    assert(ret == 0);
    assert(name[0] == '\0');
    assert(params[0] == '\0');

    ret = virt_module_loader_parse_line("   # Indented comment", name, sizeof(name), params, sizeof(params));
    assert(ret == 0);
    assert(name[0] == '\0');

    ret = virt_module_loader_parse_line("   \t  \n", name, sizeof(name), params, sizeof(params));
    assert(ret == 0);
    assert(name[0] == '\0');
    assert(params[0] == '\0');
    printf("[PASS] comments and blank lines correctly yield no entry\n");

    /* 4. ../evil.ko is rejected */
    ret = virt_module_loader_parse_line("../evil.ko", name, sizeof(name), params, sizeof(params));
    assert(ret < 0);

    ret = virt_module_loader_parse_line("sub/dir/test.ko", name, sizeof(name), params, sizeof(params));
    assert(ret < 0);
    printf("[PASS] path traversal ../evil.ko rejected\n");

    /* 5. a 64-byte name is rejected */
    /* 60 'a's + ".ko" = 63 bytes (valid) */
    char name_63[128];
    memset(name_63, 'a', 60);
    memcpy(name_63 + 60, ".ko", 4);
    ret = virt_module_loader_parse_line(name_63, name, sizeof(name), params, sizeof(params));
    assert(ret == 1);

    /* 61 'a's + ".ko" = 64 bytes (invalid: > 63 bytes) */
    char name_64[128];
    memset(name_64, 'a', 61);
    memcpy(name_64 + 61, ".ko", 4);
    ret = virt_module_loader_parse_line(name_64, name, sizeof(name), params, sizeof(params));
    assert(ret < 0);
    printf("[PASS] 64-byte name rejected\n");

    /* 6. a 256-byte parameter string is rejected */
    /* 255 valid chars (valid) */
    char line_255[512];
    snprintf(line_255, sizeof(line_255), "test.ko ");
    size_t prefix_len = strlen(line_255);
    memset(line_255 + prefix_len, 'x', 255);
    line_255[prefix_len + 255] = '\0';
    ret = virt_module_loader_parse_line(line_255, name, sizeof(name), params, sizeof(params));
    assert(ret == 1);
    assert(strlen(params) == 255);

    /* 256 valid chars (invalid: > 255 bytes) */
    char line_256[512];
    snprintf(line_256, sizeof(line_256), "test.ko ");
    prefix_len = strlen(line_256);
    memset(line_256 + prefix_len, 'x', 256);
    line_256[prefix_len + 256] = '\0';
    ret = virt_module_loader_parse_line(line_256, name, sizeof(name), params, sizeof(params));
    assert(ret < 0);
    printf("[PASS] 256-byte parameter string rejected\n");

    /* 7. a character outside the Section 2.2 parameter allowlist is rejected */
    /* Allowed: A-Z a-z 0-9 _ = . , space */
    ret = virt_module_loader_parse_line("test.ko opt=1;rm -rf /", name, sizeof(name), params, sizeof(params));
    assert(ret < 0);

    ret = virt_module_loader_parse_line("test.ko opt=`id`", name, sizeof(name), params, sizeof(params));
    assert(ret < 0);

    ret = virt_module_loader_parse_line("test.ko opt=$FOO", name, sizeof(name), params, sizeof(params));
    assert(ret < 0);

    ret = virt_module_loader_parse_line("test.ko opt=val&other=1", name, sizeof(name), params, sizeof(params));
    assert(ret < 0);
    printf("[PASS] Disallowed parameter characters rejected\n");

    printf("=== All virt_module_loader Tests Passed ===\n\n");
    return 0;
}
