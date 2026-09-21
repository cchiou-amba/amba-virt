/*
 * cavalry_ioctl_path_b.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _CAVALRY_IOCTL_PATH_B_H_
#define _CAVALRY_IOCTL_PATH_B_H_

#if defined(__QNXNTO__)
#include <stdint.h>
#include <sys/ioctl.h>
#elif defined(__has_include)
#if __has_include(<linux/types.h>)
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
#endif
#else
#include <linux/types.h>
#include <linux/ioctl.h>
#endif
#include "amba_virt.h"
#include "cavalry_ioctl.h"

struct cavalry_reg_dag_user {
	__u32 staging_bar_offset;
	__u32 staging_size;
	__u32 dvi_offset_in_slice;
	__u32 extra_dag_list_offset_in_slice;
	__u32 extra_dag_common_offset_in_slice;
	__u32 extra_poke_list_offset_in_slice;
	__u8  sha256[CAVALRY_SHA256_LEN];
	__u32 dag_id;       /* returned */
	__u32 run_dags_bytes;
	__u64 run_dags_ptr; /* userspace pointer to struct cavalry_run_dags */
};

struct cavalry_unreg_dag_user {
	__u32 dag_id;
};

struct cavalry_alloc_handle_user {
	__u32 size;
	__u32 handle_id;    /* returned */
	__u32 bar_offset;   /* returned */
};

struct cavalry_free_handle_user {
	__u32 handle_id;
};

struct cavalry_run_reg_user {
	__u32 dag_id;
	__u32 port_cnt;
	struct amba_virt_cavalry_port_bind ports[CAVALRY_MAX_PORTS];
	__u32 rval;         /* returned */
	__u32 exec_ticks;   /* returned */
};

#define CAVALRY_IOC_REGISTER_DAG        _IOWR('C', 0xC0, struct cavalry_reg_dag_user)
#define CAVALRY_IOC_UNREGISTER_DAG      _IOWR('C', 0xC1, struct cavalry_unreg_dag_user)
#define CAVALRY_IOC_ALLOC_HANDLE        _IOWR('C', 0xC2, struct cavalry_alloc_handle_user)
#define CAVALRY_IOC_FREE_HANDLE         _IOWR('C', 0xC3, struct cavalry_free_handle_user)
#define CAVALRY_IOC_RUN_REGISTERED_DAG  _IOWR('C', 0xC4, struct cavalry_run_reg_user)

#endif /* _CAVALRY_IOCTL_PATH_B_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
