/*
 * amba_virt.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_AMBA_VIRT_H
#define _UAPI_AMBA_VIRT_H

#if defined(__QNXNTO__)
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
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



#define AMBA_VIRT_DEV_NAME	"amba_virt"
#define AMBA_VIRT_DEV_PATH	"/dev/amba_virt"
#define AMBA_VIRT_SHM_DEV_NAME	"amba_virt_shm"
#define AMBA_VIRT_SHM_DEV_PATH	"/dev/amba_virt_shm"

#define AMBA_VIRT_PROTO		3u

/* Host CID on EVE/QEMU vhost-vsock. Do not use port 2000 (VComLink). */
#define AMBA_VIRT_VSOCK_CID	2u
#define AMBA_VIRT_VSOCK_PORT	5555u

#define AMBA_VIRT_MAX_MSG	4096u

#define AMBA_VIRT_ROLE_GUEST	0u
#define AMBA_VIRT_ROLE_HOST	1u

struct amba_virt_info {
	__u32 proto;
	__u32 role;
	__u32 shm_size;
	__u32 connected;
	__u32 vsock_cid;
	__u32 vsock_port;
	__u64 shm_phys;		/* Host physical base address of CVMEM window */
};

struct amba_virt_xfer {
	__u32 len;
	__s32 timeout_ms;	/* RECV only; 0 = 5000 ms; <0 = wait forever */
	__u32 client_cid;	/* Host RECV: caller CID; Host SEND: target CID */
	__u32 flags;
	__u8  data[AMBA_VIRT_MAX_MSG];
};

struct amba_virt_dmabuf_slice {
	__u32 slice_idx;
	__u32 offset;
	__u32 size;
	__s32 fd;
};

/* Userspace payload (inside xfer.data), not interpreted by the kmod. */
#define AMBA_VIRT_MSG_PING		1u
#define AMBA_VIRT_MSG_PONG		2u
#define AMBA_VIRT_MSG_SHM_NOTIFY	3u
#define AMBA_VIRT_MSG_SHM_ACK		4u
#define AMBA_VIRT_MSG_GDMA_COPY_REQ	11u
#define AMBA_VIRT_MSG_GDMA_COPY_RESP	12u
#define AMBA_VIRT_MSG_GDMA_PITCH_REQ	13u
#define AMBA_VIRT_MSG_GDMA_PITCH_RESP	14u
#define AMBA_VIRT_MSG_CAVALRY_REQ	15u
#define AMBA_VIRT_MSG_CAVALRY_RESP	16u

#define AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ	22u
#define AMBA_VIRT_MSG_DEV_SET_BOUNDS_RESP	23u
#define AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_REQ	24u
#define AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_RESP	25u
#define AMBA_VIRT_MSG_QUERY_REQ			26u
#define AMBA_VIRT_MSG_QUERY_RESP		27u
#define AMBA_VIRT_MSG_MEM_ALLOC_REQ		28u
#define AMBA_VIRT_MSG_MEM_ALLOC_RESP		29u
#define AMBA_VIRT_MSG_ACL_GET_REQ		30u
#define AMBA_VIRT_MSG_DEV_STATE_EVENT		40u

/* Virtual Peripheral DMA Messages */
#define AMBA_VIRT_MSG_DMA_SLAVE_CFG_REQ		50u
#define AMBA_VIRT_MSG_DMA_SLAVE_CFG_RESP	51u
#define AMBA_VIRT_MSG_DMA_SUBMIT_REQ		52u
#define AMBA_VIRT_MSG_DMA_SUBMIT_RESP		53u
#define AMBA_VIRT_MSG_DMA_TERMINATE_REQ		54u
#define AMBA_VIRT_MSG_DMA_TERMINATE_RESP		55u
#define AMBA_VIRT_MSG_DMA_COMPLETE_EVENT		56u

enum vcav_opcode {
	VCAV_OP_GET_VERSION	= 1,
	VCAV_OP_GET_CHIP_ID	= 2,
	VCAV_OP_GET_STATUS	= 3,
	VCAV_OP_QUERY_BUF	= 4,
	VCAV_OP_ALLOC_MEM	= 5,
	VCAV_OP_FREE_MEM	= 6,
	VCAV_OP_SYNC_CACHE	= 7,
	VCAV_OP_START_VP	= 8,
	VCAV_OP_STOP_VP		= 9,
	VCAV_OP_RUN_DAGS	= 10,
	VCAV_OP_QUERY_UCODE_CMD_SIZE = 11,

	/* Path B & Session Management */
	VCAV_OP_CLOSE_SESSION	= 20,
	VCAV_OP_REGISTER_DAG	= 21,
	VCAV_OP_UNREGISTER_DAG	= 22,
	VCAV_OP_ALLOC_HANDLE	= 23,
	VCAV_OP_FREE_HANDLE	= 24,
	VCAV_OP_RUN_REGISTERED_DAG = 25,
};

struct amba_virt_cavalry_rpc {
	__u32 opcode;		/* enum vcav_opcode */
	__s32 status;		/* 0 on success, negative errno on error */
	__u32 session_id;	/* Guest session token (starts at 1, never 0) */
	__u32 dag_id;		/* Registered DAG ID or handle_id */
	__u32 bar_offset;	/* BAR offset returned or freed */
	__u32 size;		/* Size for ALLOC / length */
	__u32 arena_len;	/* Length of serialized blob in RPC Arena */
	__u32 rval;		/* VP completion return code */
	__u32 exec_ticks;	/* VP hardware execution ticks */
	__u32 chip_id;		/* Hardware Chip ID from host */
};

#define CAVALRY_MAX_PORTS	32
#define CAVALRY_SHA256_LEN	32

struct amba_virt_cavalry_port_bind {
	__u32 dag_idx;
	__u32 port_idx;
	__u32 handle_id;
	__u32 offset;
	__u32 size;
};

struct amba_virt_cavalry_reg_dag_desc {
	__u32 staging_bar_offset;
	__u32 staging_size;
	__u32 dvi_offset_in_slice;
	__u32 extra_dag_list_offset_in_slice;
	__u32 extra_dag_common_offset_in_slice;
	__u32 extra_poke_list_offset_in_slice;
	__u8  sha256[CAVALRY_SHA256_LEN];
	__u32 run_dags_bytes;
};

struct amba_virt_cavalry_run_reg_desc {
	__u32 dag_id;
	__u32 port_cnt;
	struct amba_virt_cavalry_port_bind ports[CAVALRY_MAX_PORTS];
};

/* Virtual Device Types */
#define AMBA_VIRT_DEV_TYPE_NONE        0u
#define AMBA_VIRT_DEV_TYPE_CAVALRY     1u
#define AMBA_VIRT_DEV_TYPE_GDMA        2u
#define AMBA_VIRT_DEV_TYPE_IAV         3u
#define AMBA_VIRT_DEV_TYPE_SCRATCH     4u
#define AMBA_VIRT_MAX_DEV_TYPES        8u

/* Automatic Offset Placement Sentinel */
#define AMBA_VIRT_OFFSET_AUTO          0xFFFFFFFFU

/* Request Flags */
#define AMBA_VIRT_DEV_F_NONE           0x00000000U
#define AMBA_VIRT_DEV_F_EXACT          0x00000001U
#define AMBA_VIRT_DEV_F_BEST_EFFORT    0x00000002U
#define AMBA_VIRT_DEV_F_REPLACE        0x00000004U
#define AMBA_VIRT_DEV_F_ZERO_INIT      0x00000008U

/* Granular Error / Reason Codes */
enum amba_virt_err_code {
	AMBA_VIRT_ERR_NONE             = 0,
	AMBA_VIRT_ERR_PERM_DENIED      = 1,
	AMBA_VIRT_ERR_QUOTA_EXCEEDED   = 2,
	AMBA_VIRT_ERR_HOST_OOM         = 3,
	AMBA_VIRT_ERR_OFFSET_COLLISION = 4,
	AMBA_VIRT_ERR_DEV_ALREADY_REG  = 5,
	AMBA_VIRT_ERR_INVALID_ALIGN    = 6,
	AMBA_VIRT_ERR_UNKNOWN_DEVICE   = 7,
	AMBA_VIRT_ERR_BAR_OVERFLOW     = 8,
};

/* In-Band Device Boundary Request */
struct amba_virt_dev_bounds_req {
	__u32 dev_type;
	__u32 requested_size;
	__u32 preferred_offset;
	__u32 rpc_arena_size;
	__u32 align;
	__u32 flags;
};

/* In-Band Device Boundary Response */
struct amba_virt_dev_bounds_resp {
	__s32 status;
	__u32 err_code;
	__u32 granted_offset;
	__u32 granted_size;
	__u32 rpc_arena_offset;
	__u32 max_avail_size;
	__u32 suggested_offset;
	__u32 colliding_dev;
};

/* Memory Request Types */
enum amba_virt_mem_op {
	AMBA_VIRT_MEM_OP_ALLOC   = 1,
	AMBA_VIRT_MEM_OP_FREE    = 2,
	AMBA_VIRT_MEM_OP_RESIZE  = 3,
	AMBA_VIRT_MEM_OP_CARVE   = 4,
};

struct amba_virt_mem_req {
	__u32 op;
	__u32 size;
	__u32 align;
	__u32 dev_affinity;
	__u32 bar_offset;
	__u32 flags;
};

struct amba_virt_mem_resp {
	__s32 status;
	__u32 bar_offset;
	__u32 allocated_size;
	__u64 phys_addr;
	__u32 total_allocated;
	__u32 total_free;
};

enum amba_virt_query_op {
	AMBA_VIRT_QUERY_SELF         = 1,
	AMBA_VIRT_QUERY_PEERS        = 2,
	AMBA_VIRT_QUERY_DEV_TOPOLOGY = 3,
	AMBA_VIRT_QUERY_DEV_MEM      = 4,
	AMBA_VIRT_QUERY_DRIVER_CAPS  = 5,
};

#define AMBA_VIRT_DEV_STATE_OFFLINE   0u
#define AMBA_VIRT_DEV_STATE_ONLINE    1u

struct amba_virt_dev_state_event {
	__u32 dev_id;         /* AMBA_VIRT_DEV_TYPE_* */
	__u32 state;          /* AMBA_VIRT_DEV_STATE_OFFLINE / ONLINE */
	__u32 reason_code;    /* 0 = Normal, 1 = Host Unloaded, 2 = Drain Timeout */
	__u32 host_mod_mask;  /* Current live host module bitmask */
	__u64 timestamp_ns;   /* Monotonic host timestamp */
};

struct amba_virt_driver_cap_entry {
	__u32 dev_id;
	__u32 state;
	__u32 module_mask;
	char  dev_name[32];
};

struct amba_virt_driver_caps_resp {
	__u32 count;
	__u32 host_mod_mask;
	struct amba_virt_driver_cap_entry entries[8];
};

struct amba_virt_query_req {
	__u32 query_op;
	__u32 target_cid;
	__u32 dev_id;
	__u32 flags;
};

struct amba_virt_peer_desc {
	__u32 cid;
	__u32 tenant_idx;
	__u32 status;
	__u32 mem_allocated_mb;
	__u32 active_sessions;
	__u32 active_dags;
	__u32 active_handles;
	__u32 caps;
};

struct amba_virt_dev_mem_desc {
	__u32 dev_type;
	__u32 base_offset;
	__u32 size;
	__u32 rpc_arena_offset;
	__u32 rpc_arena_size;
	__u32 flags;
};

struct amba_virt_topo_desc {
	__u32 chip_id;
	__u32 npu_core_cnt;
	__u32 npu_freq_mhz;
	__u32 gdma_channels;
	__u32 total_cvmem_mb;
	__u64 host_phys_addr;
};

struct amba_virt_query_resp {
	__u32 query_op;
	__s32 status;
	__u32 count;
	__u8  payload[3900];
};

#define AMBA_VIRT_CAP_NONE              0x00000000U
#define AMBA_VIRT_CAP_PING              0x00000001U
#define AMBA_VIRT_CAP_QUERY_SELF        0x00000002U
#define AMBA_VIRT_CAP_QUERY_PEERS       0x00000004U
#define AMBA_VIRT_CAP_QUERY_TOPO        0x00000008U
#define AMBA_VIRT_CAP_MEM_ALLOC         0x00000010U
#define AMBA_VIRT_CAP_MEM_RESIZE        0x00000020U
#define AMBA_VIRT_CAP_DEV_CONFIG        0x00000040U
#define AMBA_VIRT_CAP_GDMA_COPY         0x00000080U
#define AMBA_VIRT_CAP_GDMA_PITCH        0x00000100U
#define AMBA_VIRT_CAP_CAVALRY_PATH_B    0x00000200U
#define AMBA_VIRT_CAP_CAVALRY_PATH_A    0x00000400U
#define AMBA_VIRT_CAP_CAVALRY_REGISTER  0x00000800U
#define AMBA_VIRT_CAP_IAV_STREAM        0x00001000U
#define AMBA_VIRT_CAP_DMA_SLAVE         0x00002000U

#define AMBA_VIRT_ROLE_UNTRUSTED \
	(AMBA_VIRT_CAP_PING | AMBA_VIRT_CAP_QUERY_SELF | AMBA_VIRT_CAP_CAVALRY_PATH_B | AMBA_VIRT_CAP_CAVALRY_REGISTER)

#define AMBA_VIRT_ROLE_STANDARD \
	(AMBA_VIRT_ROLE_UNTRUSTED | AMBA_VIRT_CAP_MEM_ALLOC | AMBA_VIRT_CAP_DEV_CONFIG | \
	 AMBA_VIRT_CAP_GDMA_COPY | AMBA_VIRT_CAP_GDMA_PITCH | AMBA_VIRT_CAP_DMA_SLAVE)

struct amba_virt_acl_rule {
	__u32 cid;
	__u32 caps;
	__u32 quota_mb;
	__u32 priority;
};

struct amba_virt_msg {
	__u32 type;
	__u32 seq;
	__u32 shm_off;
	__u32 shm_len;
};

struct amba_virt_gdma_copy {
	__u32 src_off;
	__u32 dst_off;
	__u32 len;
	__u32 flags;
	__s32 status;
	__u16 src_pitch;
	__u16 dst_pitch;
	__u16 width;
	__u16 height;
	__u32 reserved;
};

#define AMBA_VIRT_GDMA_F_NONE	0u
#define AMBA_VIRT_GDMA_F_PITCH	(1u << 0)

/* Backend IPC Opcodes (over persistent TCP on port 5556) */
#define BACKEND_MSG_AUTH_REQ           0x01u
#define BACKEND_MSG_AUTH_RESP          0x02u
#define BACKEND_OP_HEARTBEAT           0x03u
#define BACKEND_OP_HEARTBEAT_ACK       0x04u
#define BACKEND_OP_FULL_STATUS         0x05u
#define BACKEND_OP_FULL_STATUS_RESP    0x06u
#define BACKEND_OP_MODULE_LOAD         0x07u
#define BACKEND_OP_MODULE_LOAD_RESP    0x08u
#define BACKEND_OP_MODULE_UNLOAD       0x09u
#define BACKEND_OP_MODULE_UNLOAD_RESP  0x0Au
#define BACKEND_OP_HARDWARE_RESET      0x0Bu
#define BACKEND_OP_HARDWARE_RESET_RESP 0x0Cu
#define BACKEND_OP_FIRMWARE_VERSIONS   0x0Du
#define BACKEND_OP_FIRMWARE_VERSIONS_RESP 0x0Eu
#define BACKEND_EVENT_MODULE_CHANGED   0x10u

#define BACKEND_MSG_MAGIC              0x4156424BU /* 'AVBK' */
#define BACKEND_TOKEN_MAX_LEN          64

struct backend_msg_hdr {
	__u32 magic;       /* BACKEND_MSG_MAGIC */
	__u32 msg_type;    /* BACKEND_OP_* */
	__u32 seq;
	__u32 len;         /* Payload length following header */
	__s32 status;      /* 0 = success, negative = errno */
};

struct backend_firmware_entry {
	char  name[32];
	__u32 size;
	char  sha256[64];
	__u32 present;
};

struct backend_firmware_resp {
	__u32 count;
	struct backend_firmware_entry entries[8];
};

#define AMBA_VIRT_MAX_GUESTS 8

struct amba_virt_guest_list {
	__u32 count;
	__u32 cids[AMBA_VIRT_MAX_GUESTS];
};

struct amba_virt_push_msg {
	__u32 target_cid;   /* 0 = broadcast to all active guests */
	__u32 len;
	__u8  data[AMBA_VIRT_MAX_MSG];
};

#define AMBA_VIRT_IOC_MAGIC	'A'
#define AMBA_VIRT_IOC_GET_INFO	_IOR(AMBA_VIRT_IOC_MAGIC, 1, struct amba_virt_info)
#define AMBA_VIRT_IOC_CONNECT	_IO(AMBA_VIRT_IOC_MAGIC, 2)
#define AMBA_VIRT_IOC_SEND	_IOW(AMBA_VIRT_IOC_MAGIC, 3, struct amba_virt_xfer)
#define AMBA_VIRT_IOC_RECV	_IOWR(AMBA_VIRT_IOC_MAGIC, 4, struct amba_virt_xfer)
#define AMBA_VIRT_IOC_HOST_GDMA_COPY \
	_IOWR(AMBA_VIRT_IOC_MAGIC, 5, struct amba_virt_gdma_copy)
#define AMBA_VIRT_IOC_RPC	_IOWR(AMBA_VIRT_IOC_MAGIC, 6, struct amba_virt_xfer)
#define AMBA_VIRT_IOC_EXPORT_DMABUF _IOR(AMBA_VIRT_IOC_MAGIC, 7, __s32)
#define AMBA_VIRT_IOC_EXPORT_DMABUF_SLICE \
	_IOWR(AMBA_VIRT_IOC_MAGIC, 8, struct amba_virt_dmabuf_slice)
#define AMBA_VIRT_IOC_HOST_DMA_SLAVE \
	_IOWR(AMBA_VIRT_IOC_MAGIC, 9, struct amba_virt_dma_slave_xfer)
#define AMBA_VIRT_IOC_LIST_GUESTS \
	_IOR(AMBA_VIRT_IOC_MAGIC, 0x20, struct amba_virt_guest_list)
#define AMBA_VIRT_IOC_PUSH \
	_IOW(AMBA_VIRT_IOC_MAGIC, 0x21, struct amba_virt_push_msg)

/* ---- Virtual Peripheral DMA Protocol Structures ---- */

#define AMBA_VIRT_DMA_DIR_MEM_TO_DEV	1u
#define AMBA_VIRT_DMA_DIR_DEV_TO_MEM	2u

/* Host-side peripheral slave DMA dispatch ioctl structure */
struct amba_virt_dma_slave_xfer {
	__u32 channel;       /* Physical channel (13=UART2 TX, 15=UART3 TX) */
	__u32 direction;     /* AMBA_VIRT_DMA_DIR_MEM_TO_DEV (1) */
	__u64 buf_phys;      /* Host physical address of ivshmem buffer */
	__u32 buf_len;       /* Length in bytes */
	__u32 timeout_ms;    /* Timeout in ms (e.g. 1000) */
	__s32 status;        /* Response: 0 or -errno */
	__u32 transferred;   /* Bytes transferred */
};

/* CID-to-DMA-Channel ACL entry */
struct amba_virt_dma_channel_acl {
	__u32 cid;
	__u32 tx_channel;	/* DMA channel for MEM_TO_DEV */
	__u32 rx_channel;	/* DMA channel for DEV_TO_MEM */
};

/* Slave configuration request/response */
struct amba_virt_dma_slave_cfg {
	__u32 channel;		/* Physical DMA channel number */
	__u32 direction;	/* AMBA_VIRT_DMA_DIR_* */
	__u32 src_addr_lo;	/* Peripheral FIFO or memory phys (low 32) */
	__u32 dst_addr_lo;
	__u32 src_addr_width;	/* Transfer width: 1, 2, or 4 bytes */
	__u32 dst_addr_width;
	__u32 src_maxburst;	/* Burst length in units of addr_width */
	__u32 dst_maxburst;
	__s32 status;		/* Response: 0 or -errno */
};

/* DMA transfer submit request/response */
struct amba_virt_dma_submit {
	__u32 channel;
	__u32 direction;
	__u32 buf_offset;	/* Offset within guest's ivshmem window */
	__u32 buf_len;		/* Transfer length in bytes */
	__u32 cookie;		/* Guest-assigned transfer ID */
	__s32 status;		/* Response: 0 or -errno */
};

/* DMA terminate request/response */
struct amba_virt_dma_terminate {
	__u32 channel;
	__s32 status;		/* Response: 0 or -errno */
};

/* Asynchronous DMA completion notification (server -> guest push) */
struct amba_virt_dma_complete {
	__u32 channel;
	__u32 cookie;		/* Echoed from submit request */
	__u32 bytes_transferred;
	__s32 status;		/* 0 = success, -ETIMEDOUT = watchdog */
};

#endif /* _UAPI_AMBA_VIRT_H */

