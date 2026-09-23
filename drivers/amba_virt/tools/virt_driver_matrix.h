/*
 * virt_driver_matrix.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_DRIVER_MATRIX_H
#define VIRT_DRIVER_MATRIX_H

#include <stdint.h>
#include <stdbool.h>

/* Subsystem Bitmasks */
#define HOST_MOD_AMBCMA       (1U << 0)
#define HOST_MOD_CAVALRY      (1U << 1)
#define HOST_MOD_DSP          (1U << 2)
#define HOST_MOD_IAV          (1U << 3)
#define HOST_MOD_AMBA_VIRT    (1U << 4)
#define HOST_MOD_AMBA_OTP     (1U << 5)
#define HOST_MOD_PVRSRVKM     (1U << 6)
#define HOST_MOD_AMBRG        (1U << 7)
#define HOST_MOD_MAX96712     (1U << 8)
#define HOST_MOD_HW_TIMER     (1U << 9)
#define HOST_MOD_MSG          (1U << 10)
#define HOST_MOD_IMGPROC      (1U << 11)
#define HOST_MOD_VIO_MONITOR  (1U << 12)

/* Functional Pipeline Preset Masks */
#define PIPELINE_MASK_NPU     (HOST_MOD_AMBCMA | HOST_MOD_CAVALRY)
#define PIPELINE_MASK_CAMERA  (HOST_MOD_HW_TIMER | HOST_MOD_AMBCMA | HOST_MOD_MSG | \
                               HOST_MOD_DSP | HOST_MOD_IMGPROC | HOST_MOD_IAV | \
                               HOST_MOD_VIO_MONITOR | HOST_MOD_AMBRG | HOST_MOD_MAX96712)
#define PIPELINE_MASK_GDMA    (HOST_MOD_AMBA_VIRT)
#define PIPELINE_MASK_OTP     (HOST_MOD_AMBA_OTP)

struct virt_module_dep {
    const char *module_name;     /* e.g. "cavalry.ko" */
    uint32_t module_mask;        /* HOST_MOD_CAVALRY */
    uint32_t prerequisite_mask;  /* e.g. HOST_MOD_AMBCMA */
    const char *device_node;     /* "/dev/cavalry" */
    const char *firmware_file;   /* "cavalry.bin" */
    const char *service_cmd;     /* Optional userspace helper daemon */
    uint32_t virt_dev_id;        /* Associated AMBA_VIRT_DEV_* ID */
};

/* Static dependency table */
static const struct virt_module_dep g_driver_matrix[] = {
    {
        .module_name = "ambcma.ko",
        .module_mask = HOST_MOD_AMBCMA,
        .prerequisite_mask = 0,
        .device_node = NULL,
        .firmware_file = NULL,
        .service_cmd = NULL,
        .virt_dev_id = 0,
    },
    {
        .module_name = "cavalry.ko",
        .module_mask = HOST_MOD_CAVALRY,
        .prerequisite_mask = HOST_MOD_AMBCMA,
        .device_node = "/dev/cavalry",
        .firmware_file = "cavalry.bin",
        .service_cmd = NULL,
        .virt_dev_id = 1, /* AMBA_VIRT_DEV_CAVALRY */
    },
    {
        .module_name = "dsp.ko",
        .module_mask = HOST_MOD_DSP,
        .prerequisite_mask = HOST_MOD_AMBCMA | HOST_MOD_HW_TIMER,
        .device_node = "/dev/dsp",
        .firmware_file = "orccode.bin",
        .service_cmd = NULL,
        .virt_dev_id = 0,
    },
    {
        .module_name = "imgproc.ko",
        .module_mask = HOST_MOD_IMGPROC,
        .prerequisite_mask = HOST_MOD_DSP,
        .device_node = "/dev/imgproc",
        .firmware_file = NULL,
        .service_cmd = NULL,
        .virt_dev_id = 0,
    },
    {
        .module_name = "iav.ko",
        .module_mask = HOST_MOD_IAV,
        .prerequisite_mask = HOST_MOD_DSP | HOST_MOD_IMGPROC | HOST_MOD_AMBCMA,
        .device_node = "/dev/iav",
        .firmware_file = "orccode.bin",
        .service_cmd = "load_ucode /persist/firmware",
        .virt_dev_id = 3, /* AMBA_VIRT_DEV_TYPE_IAV */
    },
    {
        .module_name = "amba_virt.ko",
        .module_mask = HOST_MOD_AMBA_VIRT,
        .prerequisite_mask = 0,
        .device_node = "/dev/amba_virt",
        .firmware_file = NULL,
        .service_cmd = NULL,
        .virt_dev_id = 2, /* AMBA_VIRT_DEV_TYPE_GDMA */
    },
    {
        .module_name = "amba_otp.ko",
        .module_mask = HOST_MOD_AMBA_OTP,
        .prerequisite_mask = 0,
        .device_node = "/dev/amba_otp",
        .firmware_file = NULL,
        .service_cmd = NULL,
        .virt_dev_id = 5, /* AMBA_VIRT_DEV_OTP */
    },
};

#define DRIVER_MATRIX_COUNT (sizeof(g_driver_matrix) / sizeof(g_driver_matrix[0]))

#endif /* VIRT_DRIVER_MATRIX_H */
