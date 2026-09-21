/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * cavalry_ioctl.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef __CAVALRY_IOCTL_H__
#define __CAVALRY_IOCTL_H__

/*! @file cavalry_ioctl.h
 *  @brief This file defines cavalry driver ioctl api.
 */

#if defined(__QNXNTO__)
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
#elif defined(__has_include)
#if __has_include(<linux/ioctl.h>)
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif
#else
#include <linux/ioctl.h>
#endif

/*! @addtogroup cavalry-ioctl-helper
 * @{
 */

/*! @macros CAVALRY_DEV_NODE
 *  @brief define the device node when call open().
 */
#if defined (BUILD_AMBARELLA_AMBACV_DRV) && defined (BUILD_AMBARELLA_CAVALRY_DRV)
#error "Can not enable ambacv and cavalry at the same time"
#elif defined (BUILD_AMBARELLA_AMBACV_DRV)
#define CAVALRY_DEV_NODE	"/dev/ambacv"
#else
#define CAVALRY_DEV_NODE	"/dev/cavalry"
#endif

#define CAVALRY_PROFILE_DEV_NODE	"/dev/cavalry_profile"

/*! @macros CAVALRY_PORT_PITCH_ALIGN
 *  @brief define port pitch alignment size for different chips .
 */
#define CAVALRY_PORT_PITCH_ALIGN (128)

#if defined (AMBA_SOC_N1)
#define CAVALRY_NVP_CNT (6)
#define CAVALRY_GVP_CNT (2)
#define CAVALRY_FEX_CNT (2)
#define CAVALRY_FMA_CNT (2)
#elif defined (AMBA_SOC_CV3AD685)
#define CAVALRY_NVP_CNT (3)
#define CAVALRY_GVP_CNT (2)
#define CAVALRY_FEX_CNT (1)
#define CAVALRY_FMA_CNT (1)
#elif defined (AMBA_SOC_CV72)
#define CAVALRY_NVP_CNT (1)
#define CAVALRY_GVP_CNT (0)
#define CAVALRY_FEX_CNT (0)
#define CAVALRY_FMA_CNT (0)
#elif defined (AMBA_SOC_CV75)
#define CAVALRY_NVP_CNT (1)
#define CAVALRY_GVP_CNT (0)
#define CAVALRY_FEX_CNT (0)
#define CAVALRY_FMA_CNT (0)
#elif defined(AMBA_SOC_N1_655)
#define CAVALRY_NVP_CNT (1)
#define CAVALRY_GVP_CNT (0)
#define CAVALRY_FEX_CNT (1)
#define CAVALRY_FMA_CNT (0)
#elif defined (AMBA_SOC_CV7)
#define CAVALRY_NVP_CNT (1)
#define CAVALRY_GVP_CNT (0)
#define CAVALRY_FEX_CNT (0)
#define CAVALRY_FMA_CNT (0)
#elif defined (AMBA_SOC_CV8)
#define CAVALRY_NVP_CNT (1)
#define CAVALRY_GVP_CNT (0)
#define CAVALRY_FEX_CNT (0)
#define CAVALRY_FMA_CNT (0)

#else
#error "Unsupported CHIP ID, please check sdk config"
#endif

#define CV_HW_TOTAL_NUM (CAVALRY_NVP_CNT + CAVALRY_GVP_CNT + CAVALRY_FEX_CNT + CAVALRY_FMA_CNT)

#define CAVALRY_NVP_AFFINITY_MASK	((1 << CAVALRY_NVP_CNT) - 1)
#define CAVALRY_GVP_AFFINITY_MASK	((1 << CAVALRY_GVP_CNT) - 1)
#define CAVALRY_FEX_AFFINITY_MASK	((1 << CAVALRY_FEX_CNT) - 1)
#define CAVALRY_FMA_AFFINITY_MASK	((1 << CAVALRY_FMA_CNT) - 1)
#define CAVALRY_ANY_AFFINITY_MASK	(0xFFFFFFFFU)

/* Type define physical address for different arch.
* CV2x access memory limited in 4 GByte, but CV5 can access memory more than 4 GByte. */

typedef unsigned long cv_daddr_t;
typedef unsigned long cv_doffset_t;
typedef long cv_rlt_offset_t;

/*! @macros CV2X_ARM_ACCESS_MEM_BOUNDARY
 *  @brief define the memory boundary that CPU can access, it's 3.5GByte on CV2x.
 */
#define CV2X_ARM_ACCESS_MEM_BOUNDARY (0xE0000000)

/*! @macros CAVALRY_DMABUF_FD_REPRESENT_PHYS
 *  @brief define the magic number that dmabuf-fd's offset represent absolute physical address.
 *  Special use case for extra high memory that bigger than @CV2X_ARM_ACCESS_MEM_BOUNDARY.
 *  Because dmabuf-fd can not represent extra high memory.
 */
#define CAVALRY_DMABUF_FD_REPRESENT_PHYS (-1133789)

/*! @macros CAVALRY_MONITOR_ERR_PATH
 *  @brief define the path to monitor cavalry error event.
 */
#define CAVALRY_MONITOR_ERR_PATH "/sys/devices/platform/sub_scheduler0/monitor/cavalry_err"

/*! @macros CAVALRY_MONITOR_INFO_PATH
 *  @brief define the path to monitor cavalry info event.
 */
#define CAVALRY_MONITOR_INFO_PATH "/sys/devices/platform/sub_scheduler0/monitor/cavalry_info"

/*! @macros MAX_PORT_CNT
 *  @brief define max port number on one dag.
 */
#define MAX_PORT_CNT	(256)

#define MAX_POKE_CNT	(128)

#define MAX_DEP_CNT	(256)

/*! @macros VERSION_INFO_OFFSET
 *  @brief define offset of ucode version in cavalry ucode file.
 */
#define VERSION_INFO_OFFSET	(0x40)

/*! @macros CAVALRY_HARRIS_H_BLOCKS
 *  @brief define horizontal block number for harris.
 */
#define CAVALRY_HARRIS_H_BLOCKS	(8)

/*! @macros CAVALRY_MAX_SUB_SESSION_CNT
 *  @brief define max sub session number that can register on one session id.
 */
#define CAVALRY_MAX_SUB_SESSION_CNT    (32)

/*! @macros CAVALRY_HARRIS_V_BLOCKS
 *  @brief define vertical block number for harris.
 */
#define CAVALRY_HARRIS_V_BLOCKS	(8)

/*! @macros CAVALRY_HARRIS_MAX_POINTS_PER_BLOCK
 *  @brief define the max number of harris points in one block
 */
#define CAVALRY_HARRIS_MAX_POINTS_PER_BLOCK	(32)

/*! @macros FEX_MIN_NMS_WINDOW
 *  @brief define the min FEX NMS window size.
 */
#define FEX_MIN_NMS_WINDOW (1)

/*! @macros FEX_MAX_NMS_WINDOW
 *  @brief define the max FEX NMS window size.
 */
#define FEX_MAX_NMS_WINDOW (8)

/*! @macros DEFAULT_AUDIO_CLK_HZ
 *  @brief define the default audio clock value in Hz units
 */
#define DEFAULT_AUDIO_CLK_HZ (12288000)

/*! @macros DEFAULT_DRAM_BURST_SIZE
 *  @brief define the default DRAM burst size
 */
#define DEFAULT_DRAM_BURST_SIZE (64)

/*! @macros CAVALRY_MONITOR_MSG_SIZE
 *  @brief define cavalry monitor message max length
 */
#define CAVALRY_MONITOR_MSG_SIZE (128)

/*! @enum chip_type_t
 *  @brief CV chip type enumeration.
 */
typedef enum {
	CHIP_TYPE_CV22 = 0,  /*!< CV22 */
	CHIP_TYPE_CV2 = 1,   /*!< CV2 */
	CHIP_TYPE_CV25 = 2,  /*!< CV25 */
	CHIP_TYPE_CV28 = 3,  /*!< CV28 */
	CHIP_TYPE_CV5 = 4,   /*!< CV5 */
	CHIP_TYPE_CV52 = 5,  /*!< CV52 */
	CHIP_TYPE_CV3 = 6,   /*!< CV3 */
	CHIP_TYPE_N1 = 6,    /*!< N1 */
	CHIP_TYPE_CV72 = 7,  /*!< CV72 */
	CHIP_TYPE_CV75 = 8,  /*!< CV75 */
	CHIP_TYPE_CV3AD685 = 9,  /*!< CV3AD685 */
	CHIP_TYPE_N1_655 = 10,  /*!< N1_655 */
	CHIP_TYPE_CV7 = 11,  /*!< CV7 */
	CHIP_TYPE_CV8 = 12,  /*!< CV8 */
} chip_type_t;

/*! @enum cavalry_log_level_t
 *  @brief Cavalry ucode level enumeration.
 */
typedef enum {
	CAVALRY_LOG_LEVEL_SILENT = 0,/*!< 0 */
	CAVALRY_LOG_LEVEL_MINIMAL,  /*!< 1 */
	CAVALRY_LOG_LEVEL_NORMAL,   /*!< 2 */
	CAVALRY_LOG_LEVEL_VERBOSE,  /*!< 3 */
	CAVALRY_LOG_LEVEL_DEBUG,    /*!< 4 */
	CAVALRY_LOG_LEVEL_NUM,
	CAVALRY_LOG_LEVEL_FIRST = CAVALRY_LOG_LEVEL_SILENT,
	CAVALRY_LOG_LEVEL_LAST = CAVALRY_LOG_LEVEL_DEBUG,
} cavalry_log_level_t;

typedef enum {
	CAVALRY_DAG_TYPE_NORMAL = 0,
	CAVALRY_DAG_TYPE_BUILDINGBLOCK = 1,
	CAVALRY_DAG_TYPE_NUM,
} cavalry_dag_type_t;

struct cavalry_set_log_level {
	cavalry_log_level_t log_level;  /*!< cavalry ucode log level, @sa cavalry_log_level_t */
	uint32_t rval;  /*!< return code, @sa cavalry_msg_rval_t */
};

struct version_info_s {
	uint32_t chip;  /*!< cavalry ucode for chip version, @sa chip_type_t */
	uint32_t ucode_version;  /*!< cavalry ucode version */
	uint32_t build_date;  /*!< the date to build cavalry ucode */
	uint32_t git_hash;  /*!< the source code git commit hash-ID to build cavalry ucode */
	uint32_t hotlink_fw_version;  /*!< hotlink framework version */
};

struct cavalry_driver_version {
	int major;
	int minor;
	int patch;
	uint32_t mod_time;
	char description[64];
};

/*! @enum CAVALRY_MEM
 *  @brief cavalry memory paration id enumeration.
 *  memory layout:
 *  |SHARE_TO_DSP|PRIVATE|USER_STATIC|USER|
 *
 *  private memory layout:
 *  |UCODE|Hotlink|CMD|MSG|LOG|CMD_FEX|MSG_FEX|CMD_FMA|MSG_FMA|
 *  |CMD_IDSP|MSG_IDSP|STATS_INFO|PROFILE|
 */
typedef enum {
	CAVALRY_MEM_ALL = 0x00,     /*!< 0 */
	CAVALRY_MEM_UCODE,          /*!< 1 */
	CAVALRY_MEM_HOTLINK,        /*!< 2 */
	CAVALRY_MEM_CMD,            /*!< 3 */
	CAVALRY_MEM_MSG,            /*!< 4 */
	CAVALRY_MEM_LOG,            /*!< 5 */
	CAVALRY_MEM_CMD_FEX,        /*!< 6 */
	CAVALRY_MEM_CMD_FMA,        /*!< 7 */
	CAVALRY_MEM_CMD_IDSP,       /*!< 8 */
	CAVALRY_MEM_MSG_IDSP,       /*!< 9 */
	CAVALRY_MEM_STATS_INFO,     /*!< 10 */
	CAVALRY_MEM_USER_STATIC,    /*!< 11 */
	CAVALRY_MEM_USER,           /*!< 12 */
	CAVALRY_MEM_PROFILE,        /*!< 13 */
	CAVALRY_MEM_SHARE_TO_DSP,        /*!< 14 */
	CAVALRY_MEM_NUM,
	CAVALRY_MEM_FIRST = CAVALRY_MEM_ALL,
	CAVALRY_MEM_LAST = CAVALRY_MEM_SHARE_TO_DSP,
} CAVALRY_MEM;

#define HOTLINK_SLOT_CAVALRY_FRAMEWORK	(0xC0DEBA5E)

#define CAVALRY_LOG_ENTRY_SIZE (64)
struct cavalry_log_entry {
	uint32_t seq_num;
	uint8_t	thread_id;
	uint8_t reserved0;
	uint8_t	orc_type;
	uint8_t core_id;
	uint32_t hotlink_uuid;
	uint32_t format_offset;
	uint32_t arg1;
	uint32_t arg2;
	uint32_t arg3;
	uint32_t arg4;
	uint32_t arg5;

	uint32_t reserved[CAVALRY_LOG_ENTRY_SIZE / sizeof(uint32_t) - 9];
};

struct cavalry_querybuf {
	CAVALRY_MEM buf;  /*!< query buffer id */
	unsigned long length;  /*!< buffer size */
	unsigned long offset;  /*!< buffer absolute physical address */
};

struct cavalry_port_desc {
	cv_daddr_t port_dram_addr;  /*!< port absolute physical address;
		* offset when use dma-buf:fd;
		* absolute physical address if dma-buf:fd equal @CAVALRY_DMABUF_FD_REPRESENT_PHYS */
	unsigned long port_dram_size;  /*!< port dram size, equal to port_size x 1 */
	uint32_t port_boffset_in_dag;  /*!< port bit offset in VMEM address */
	int32_t port_daddr_increment;  /*!< the port address increases the offset if the dag_loop_cnt is larger than 1;
		* sign bit decicde increase direction.  postive: forward;  negative: backward (reverse) */
};

struct cavalry_poke_desc {
	uint32_t poke_val;
	uint32_t poke_vaddr;
	uint32_t poke_bsize;
	uint32_t reserved;
};

struct extra_dag_desc_preload {
	uint64_t preload_daddr;
	uint32_t preload_vaddr;
	uint32_t preload_size;
	uint32_t reserved[12];
};

struct extra_dag_desc_runtime {
	uint64_t runtime_daddr;
	union {
		struct {
			uint32_t runtime_vaddr;
			uint32_t runtime_size;
		} general;
		struct {
			uint32_t smb_desc_vaddr;
			uint32_t smb_dbase_vaddr;
		} smb;
	} vmem_desc;
	uint32_t after_dag_run : 1;
	uint32_t is_smb : 1;
	uint32_t direction : 1;
	uint32_t reserved_0 : 29;
	uint32_t reserved[11];
};

struct extra_dag_desc_buildingblock {
	uint64_t arg_daddr;   /*!< The dram address of argument for Buildingblock */
	uint32_t slot_daddr;  /*!< Hotlink slot id for Buildingblock */
	uint32_t reserved[5]; /*!< Reserved field */
};

struct extra_dag_desc_common {
	uint32_t reserved[128];
};

struct extra_dag_desc_list {
	uint32_t preload_list_cnt;
	uint32_t runtime_list_cnt;
	uint32_t extra_port_list_cnt;
	uint32_t bb_list_cnt;
	uint32_t reserved_for_cnt[60];

	uint64_t preload_list_daddr;
	uint64_t runtime_list_daddr;
	uint64_t extra_port_list_daddr;  /*!< The content is struct cavalry_port_desc, the num is dag_loop_cnt * extra_port_list_cnt */
	uint64_t bb_list_daddr;          /*!< The content is struct extra_dag_desc_buildingblock, the num is dag_loop_cnt * extra_dag_desc_buildingblock.
		* This field was used when dag_type = CAVALRY_DAG_TYPE_BUILDINGBLOCK */
	uint64_t reserved_for_daddr[60];
};

struct cavalry_dag_desc {
	uint32_t dag_loop_cnt : 16;  /*!< the loop count for dag loop, load dag once, and exec dag N times */
	uint32_t use_ping_pong_vmem : 1;  /*!< use ping pong vmem for parallel load and exec dag */
	uint32_t run_with_checksum : 1;  /*!< Enable checksum, for internal test only */
	uint32_t is_orc_pdxs_set : 1;  /*!< Is set orc pdxs flag */
	uint32_t orc_pdxs : 3;  /*!< orc preferred dram xfer size */
	uint32_t dag_type : 3;  /*!< the type of dag_run_cmd. 0: DAG_NORMAL; 1: DAG_BuildingBlock */
	uint32_t reserved : 7;
	uint32_t dep_cnt;  /*!< This Dag depends on other DAGs */
	uint32_t dvi_dag_size;
	cv_daddr_t dvi_dram_addr;  /*!< the absolute physical address of the DVI */
	uint32_t dvi_img_vaddr;  /*!< the VMEM address of the DVI (*.dvi), *_image_start in vas.h */
	uint32_t dvi_img_size;  /*!< the size of the DVI, *_image_size in vas.h */
	uint32_t dvi_dag_vaddr;  /*!< the VMEM address of the DAG bin (*.dagbin), *_dagbin_start in vas.h */
	uint32_t private_scratchpad_offset;

	uint32_t reverse_dep_dag_cnt;  /*!< the Dag is needed by other Dag */
	uint32_t port_cnt;  /*!< the total port number of port_desc */
	uint32_t poke_cnt;
	uint32_t extra_poke_list_cnt;
	cv_daddr_t extra_poke_list_daddr;

	cv_daddr_t extra_dag_desc_common_daddr;
	cv_daddr_t extra_dag_desc_list_daddr;

	uint16_t reverse_dep_dag_id[MAX_DEP_CNT]; /*!< This Dag is needed by other DAGs ID */
	struct cavalry_port_desc port_desc[MAX_PORT_CNT];  /*!< port description in array */
	struct cavalry_poke_desc poke_desc[MAX_POKE_CNT];
};

struct cavalry_dag_desc_mfd {
	uint32_t dag_loop_cnt : 16;  /*!< the loop count for dag loop, load dag once, and exec dag N times */
	uint32_t use_ping_pong_vmem : 1;  /*!< use ping pong vmem for parallel load and exec dag */
	uint32_t run_with_checksum : 1;  /*!< Enable checksum, for internal test only */
	uint32_t is_orc_pdxs_set : 1;  /*!< Is set orc pdxs flag */
	uint32_t orc_pdxs : 3;  /*!< orc preferred dram xfer size */
	uint32_t dag_type : 3;  /*!< the type of dag_run_cmd. 0: DAG_NORMAL; 1: DAG_BuildingBlock */
	uint32_t reserved : 7;
	uint32_t dep_cnt;  /*!< This Dag depends on other DAGs */
	uint32_t dvi_dag_size;
	cv_doffset_t dvi_dram_addr_offset;  /*!< the offset base on dma-buf:fd for DVI;
		* it's absolute physical address if @dvi_dram_addr_fd equal @CAVALRY_DMABUF_FD_REPRESENT_PHYS */
	uint32_t dvi_img_vaddr;  /*!< the VMEM address of the DVI (*.dvi) */
	uint32_t dvi_img_size;  /*!< the size of the DVI */
	uint32_t dvi_dag_vaddr;  /*!< the VMEM address of the DAG bin (*.dagbin) */
	uint32_t private_scratchpad_offset;

	uint32_t reverse_dep_dag_cnt;  /*!< the Dag is needed by other Dag */
	uint32_t port_cnt;  /*!< the total port number of port_desc */
	uint32_t poke_cnt;
	uint32_t extra_poke_list_cnt;
	cv_doffset_t extra_poke_list_dram_offset;

	cv_doffset_t extra_dag_desc_common_offset;
	cv_doffset_t extra_dag_desc_list_offset;

	uint16_t reverse_dep_dag_id[MAX_DEP_CNT]; /*!< This Dag is needed by other DAGs ID */
	struct cavalry_port_desc port_desc[MAX_PORT_CNT];  /*!< port description in array */
	struct cavalry_poke_desc poke_desc[MAX_POKE_CNT];

	int port_dram_addr_fd[MAX_PORT_CNT];  /*!< dma-buf:fd for port in arrary */
};

/*! @enum cavalry_msg_rval_t
 *  @brief cavalry ucode return code enumeration.
 */
typedef enum {
	MSG_RVAL_NONE = 0,                      /*!< 0x0 */
	MSG_RVAL_INVALID_CMD,                   /*!< 0x1 */
	MSG_RVAL_INVALID_DAGCNT,                /*!< 0x2 */
	MSG_RVAL_INVALID_DAGDESC,               /*!< 0x3 */
	MSG_RVAL_INVALID_SLOT_ID,               /*!< 0x4 */
	MSG_RVAL_FEX_INSUFFICIENT_DRAM,         /*!< 0x5 */
	MSG_RVAL_FMA_INSUFFICIENT_DRAM,         /*!< 0x6 */
	MSG_RVAL_VP_RESERVED_VMEM_TRASHED,      /*!< 0x7 */
	MSG_RVAL_UUID_NOT_READY,                /*!< 0x8 */
	MSG_RVAL_INSUFFICIENT_SESSION_SLOT,     /*!< 0x9 */
	MSG_RVAL_INVALID_SESSION_ID,            /*!< 0xa */
	MSG_RVAL_ULP_NOT_INITIALIZED,           /*!< 0xb */
	MSG_RVAL_FAIL_VERIFY_CERTIFICATE,       /*!< 0xc */
	MSG_RVAL_FAIL_VERIFY_LICENSE,           /*!< 0xd */
	MSG_RVAL_FAIL_VERIFY_DIK,               /*!< 0xe */
	MSG_RVAL_INVALID_KEY_EXCHANGE_TYPE,     /*!< 0xf */
	MSG_RVAL_INVALID_SUB_SESSION_ID,     /*!< 0x10 */
	MSG_RVAL_CONVERT_AFTER_ENCRYPT_CHECKSUM_FAIL,  /*!< 0x11 */
	MSG_RVAL_NMP_CONVERT_KEY_VALID_ONCE,           /*!< 0x12 */

	/* Below MSG_RVALs are from Driver, not Ucode itself. */
	MSG_RVAL_VP_HANG = 0x80000000,          /*!< 0x80000000 */
	MSG_RVAL_VP_RUN_TIMEOUT = 0x80000001,   /*!< 0x80000001 */

	MSG_RVAL_NOT_AVAILABLE = 0xFFFFFFFF,    /*!< 0xFFFFFFFF */
} cavalry_msg_rval_t;

/*! @enum cavalry_fma_mode_t
 *  @brief cavalry FMA mode enumeration.
 */
typedef enum {
	CAVALRY_FMA_MODE_TEMPORAL = 0,  /*!< 0 */
	CAVALRY_FMA_MODE_STEREO,        /*!< 1 */
	CAVALRY_FMA_MODE_NUM,
	CAVALRY_FMA_MODE_FIRST = CAVALRY_FMA_MODE_TEMPORAL,
	CAVALRY_FMA_MODE_LAST = CAVALRY_FMA_MODE_STEREO,
} cavalry_fma_mode_t;

/*! @enum cavalry_priority_t
 *  @brief run network priority range enumeration.
 */
typedef enum {
	CAVALRY_PRIORITY_LOWEST = 0,  /*!< 0: Lowest priority */
	CAVALRY_PRIORITY_HIGHEST = 31,  /*!< 31: Highest priority */
	CAVALRY_PRIORITY_NUM,
	CAVALRY_PRIORITY_FIRST = CAVALRY_PRIORITY_LOWEST,
	CAVALRY_PRIORITY_LAST = CAVALRY_PRIORITY_HIGHEST,
} cavalry_priority_t;

/*! @enum cavalry_job_type_t
 *  @brief the job type send to cavalry ucode enumeration.
 */
typedef enum {
	CAVALRY_JOB_TYPE_ARM = 0,  /*!< 0 */
	CAVALRY_JOB_TYPE_IDSP = 1,  /*!< 1 */
	CAVALRY_JOB_TYPE_NUM,
	CAVALRY_JOB_TYPE_FIRST = CAVALRY_JOB_TYPE_ARM,
	CAVALRY_JOB_TYPE_LAST = CAVALRY_JOB_TYPE_IDSP,
} cavalry_job_type_t;

typedef enum {
	CAVALRY_NMP_KEY_EXCHANGE_TYPE_PEK = 0,  /*!< normal world */
	CAVALRY_NMP_KEY_EXCHANGE_TYPE_AEK = 1,  /*!< security world */
	CAVALRY_NMP_KEY_EXCHANGE_TYPE_NUM,
	CAVALRY_NMP_KEY_EXCHANGE_TYPE_FIRST = CAVALRY_NMP_KEY_EXCHANGE_TYPE_PEK,
	CAVALRY_NMP_KEY_EXCHANGE_TYPE_LAST = CAVALRY_NMP_KEY_EXCHANGE_TYPE_AEK,
} cavalry_nmp_key_exchange_type_t;

struct cavalry_run_dags {
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t finish_dags;  /*!< finished dag cnt when ioctl return */
	uint32_t load_total_ticks;  /*!< total dag loading time, record by cavalry ucode */
	uint32_t exec_total_ticks;  /*!< total dag execution time, record by cavalry ucode */
	uint32_t dag_cnt;  /*!< feed total dag cnt when call ioctl */
	uint32_t store_total_ticks;  /*!< used for SMB output */

	/* Fields for encrypt DAG Start */
	uint32_t is_encrypt : 1;  /*!< if encrypted for all dag */
	uint32_t is_resume : 1;  /*!< if resume net for current ioctl */
	uint32_t reserved1 : 22;
	uint32_t sub_session_id : 8;  /*!< sub session id that registered on one session id */
	uint32_t session_id;  /*!< session id for encrypt dag */
	/* Fields for encrypt DAG End */

	int32_t nid;  /*!< Network ID */
	uint32_t affinity;  /*!< Network Affinity */

	uint8_t priority;  /*!< the priority of network when running, range is @sa cavalry_priority_t */
	uint8_t hw_type;	/*!< the DAGs type running at HW type, value from @cavalry_hw_type_t */
	uint8_t reserved2[2];
	uint32_t no_auto_resume : 1;  /*!< 0: auto resume current net if other high priority net preempt;
							* 1: abort run current net if other high priority net preempt */
	uint32_t reserved2_0 : 31;
	uint32_t reserved3[18];

	cv_daddr_t ucode_cmd_phys;	/*!< the absolute physical address of the Ucode cmd */
	struct cavalry_dag_desc dag_desc[0]; /*!< dag description, zero-length array */
};

struct cavalry_run_dags_mfd {
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t finish_dags;  /*!< finished dag cnt when ioctl return */
	uint32_t load_total_ticks;  /*!< total dag loading time, record by cavalry ucode */
	uint32_t exec_total_ticks;  /*!< total dag execution time, record by cavalry ucode */
	uint32_t dag_cnt;   /*!< feed total dag cnt when call ioctl */
	uint32_t store_total_ticks;  /*!< used for SMB output */

	/* Fields for encrypt DAG Start */
	uint32_t is_encrypt : 1;  /*!< if encrypted for all dag */
	uint32_t is_resume : 1;  /*!< if resume net for current ioctl */
	uint32_t reserved1 : 22;
	uint32_t sub_session_id : 8;  /*!< sub session id that registered on one session id */
	uint32_t session_id;  /*!< session id for encrypt dag */
	/* Fields for encrypt DAG End */

	int32_t nid;  /*!< Network ID */
	uint32_t affinity;  /*!< Network Affinity */

	uint8_t priority;  /*!< the priority of network when running, range is @sa cavalry_priority_t */
	uint8_t hw_type;	/*!< the DAGs type running at HW type, value from @cavalry_hw_type_t */
	uint8_t reserved2[2];
	uint32_t no_auto_resume : 1; /*!< 0: auto resume current net if other high priority net preempt;
							* 1: abort run current net if other high priority net preempt */
	uint32_t reserved2_0 : 31;
	uint32_t reserved3[16];

	int dvi_dram_addr_fd;  /*!< dma-buf:fd for dvi */
	int ucode_cmd_addr_fd;  /*!< the dmabuf:fd of ucode cmd memory */
	cv_doffset_t ucode_cmd_addr_offset;  /*!< the offset base on @ucode_cmd_addr_fd */
	struct cavalry_dag_desc_mfd dag_desc[0];  /*!< dag description, zero-lenght array */
};

/*! @macros cavalry_offsetof
 *  @brief define offset of a structure member.
 */
#ifndef cavalry_offsetof
#define cavalry_offsetof(TYPE, MEMBER)	((size_t)&((TYPE *)0)->MEMBER)
#endif

/*! @macros RUN_DAG_SIZE
 *  @brief define struct cavalry_run_dags size according to dag number.
 */
#ifndef RUN_DAG_SIZE
#define RUN_DAG_SIZE(x) (cavalry_offsetof(struct cavalry_run_dags, dag_desc) + \
	(sizeof(struct cavalry_dag_desc) * (x)))
#endif

/*! @macros RUN_DAG_MFD_SIZE
 *  @brief define struct cavalry_run_dags_mfd size according to dag number.
 */
#ifndef RUN_DAG_MFD_SIZE
#define RUN_DAG_MFD_SIZE(x) (cavalry_offsetof(struct cavalry_run_dags_mfd, dag_desc) + \
	(sizeof(struct cavalry_dag_desc_mfd) * (x)))
#endif

struct cavalry_early_quit {
	uint32_t early_quit_all : 1;  /*!< early quit is deprecated, please set priority for high priority network. */
	uint32_t reserved : 31;
};

struct cavalry_mem {
	uint32_t cache_en : 1;  /*!< enables or disables cache. 0: non-cached;  1: cached */
	uint32_t auto_recycle : 1;  /*!< enables or disables the auto recycle memory.
				* 0: memory is persist (leakage) if have not free it before process exit;
				* 1: auto recycle memory if no user access it after process abnormal exit */
	uint32_t share_to_dsp : 1;  /*!< if physical memory can be access by DSP */
	uint32_t reserved : 29;

	unsigned long length;  /*!< input: asked memory size;
			return: the real size of the allocated memory for PAGE (4 KB) alignment */
	unsigned long offset;  /*!< the absolute physical address of the allocated memory */
};

struct cavalry_cache_mem {
	uint32_t clean : 1;  /*!< clean ARM cache. do it after arm write: cache -> dram */
	uint32_t invalid : 1; /*!< invalidate ARM cache. do it before arm read: dram -> cache */
	uint32_t reserved : 30;

	unsigned long length;  /*!< sync cache size */
	unsigned long offset; /*!< the absolute physical address of the memory */
};

struct cavalry_mem_attr {
	uint32_t cache_en : 1;  /*!< enables or disables cache. 0: non-cached;  1: cached */
	uint32_t no_recycle : 1;  /*!< enables or disables the auto recycle memory.
				* 1: memory is persist (leakage) if have not free it before process exit;
				* 0: auto recycle memory if no user access it after process abnormal exit. */
	uint32_t share_to_dsp : 1;  /*!< if physical memory can be access by DSP */
	uint32_t reserved1 : 29;

	uint32_t reserved2[15];
};

struct cavalry_query_mem_attr {
	unsigned long offset;    /*!< the absolute physical address of the memory */
	struct cavalry_mem_attr attr;

	uint32_t reserved1[4];
};

struct cavalry_usage_mem {
	unsigned long used_length;  /*!< the total size of the allocated CV user memory */
	unsigned long free_length;  /*!< the total size of the free CV user memory */
};

enum cavalry_query_mem_id {
	CAVALRY_QUERY_MEM_ID_ALL = 0,
	CAVALRY_QUERY_MEM_ID_USER = 1,
	CAVALRY_QUERY_MEM_ID_SHARE_TO_DSP = 2,
	CAVALRY_QUERY_MEM_ID_LAST = CAVALRY_QUERY_MEM_ID_SHARE_TO_DSP,
};

struct cavalry_mem_usage_query {
	uint32_t mem_id;                    /*!< enum cavalry_query_mem_id */
	uint32_t reserved0;
	unsigned long used_length;          /*!< allocated size for specified partition */
	unsigned long free_length;          /*!< free size for specified partition */
	unsigned long total_length;         /*!< total size for specified partition */
	uint32_t reserved1[16];
};

struct cavalry_mfd_alloc {
	unsigned long length;  /*!< asked memory size */

	uint32_t cache_en : 1; /*!< enables or disables cache. 0: non-cached;  1: cached */
	uint32_t share_to_dsp : 1;
	uint32_t reserved0 : 30;

	int fd;  /* return dma-buf:fd of the allocated memory */
	uint32_t reserved1[2];
};

struct cavalry_mfd_sync {
	unsigned long length;  /*!< sync cache size */
	unsigned long offset;  /*!< the offset base on dma-buf:fd */

	uint32_t clean : 1;  /*!< clean ARM cache. do it after arm write: cache -> dram */
	uint32_t invalid : 1;  /*!< invalidate ARM cache. do it before arm read: dram -> cache */
	uint32_t reserved0 : 30;

	int fd;  /*!< specify dma-buf:fd for sync cached memory */
	uint32_t reserved1[2];
};

struct cavalry_mfd_desc {
	void *virt_addr;  /*!< the virtual address in Linux User Space */
	unsigned long mem_size;  /*!< the memory size */

	int fd;  /*!< dma-buf:fd */
	uint32_t attribute;  /*!< reserved field */
	uint32_t extra_high_mem_phys;
	uint32_t reserved0[5];
};

struct cavalry_buddy_mem {
	unsigned long length;  /*!< input: asked memory size; */
	unsigned long phys_addr;  /*!< the absolute physical address of the append memory */

	uint32_t cache_en : 1; /*!< enables or disables cache. 0: non-cached;  1: cached */
	uint32_t no_recycle : 1; /*!< enables or disables the auto recycle memory.
				* 1: memory is persist (leakage) if have not free it before process exit;
				* 0: auto recycle memory if no user access it after process abnormal exit. */
	uint32_t reserved1 : 30;

	uint32_t reserved0[11];
};

struct cavalry_hotlink_slot_cfg {
	uint32_t per_slot_size;             /*!< hotlink per slot memory size */
	uint32_t slot_num;                  /*!< hotlink total slot number */
	uint32_t slot_total_size;           /*!< hotlink total slot memory size */
	uint32_t reserved0;                 /*!< reserved field */
	uint32_t history_slot_num;          /*!< hotlink total history slot number */
	uint32_t history_slot_total_size;   /*!< hotlink total history slot size */
	cv_daddr_t history_slot_phys;       /*!< hotlink history slot physical address */
	uint32_t reserved1[24];             /*!< reserved field */
};

struct cavalry_run_hotlink_slot {
	cv_daddr_t arg_daddr;  /*!< the absolute physical address of parameter */
	uint8_t slot_id;  /*!< hotlink slot id */
	uint8_t priority;  /*!< priority of hotlink when running */
	uint8_t hw_type;  /*!< value from @cavalry_hw_type_t */
	uint8_t reserved0;
	uint32_t affinity;   /*!< HW Affinity */

	uint32_t slot_rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t reserved1[9];
};

struct cavalry_hotlink_header_info {
	uint32_t code_start_addr;          /*!< hotlink code start address */
	uint32_t entry_func_addr;          /*!< hotlink entry function address */
	uint32_t slot_size;                /*!< hotlink slot size */
	uint32_t uuid;                     /*!< hotlink uuid */
	uint32_t bin_size;                 /*!< hotlink binary size */
	uint32_t framework_version;        /*!< hotlink framework version */
	uint32_t chip_type;                /*!< hotlink chip type */
};

struct cavalry_hotlink_slot_desc {
	void *virt_addr;                   /*!< the virtual address of hotlink.bin in Linux User Space */
	uint32_t hotlink_size;             /*!< the size of hotlink.bin */
	uint32_t slot_id;                  /*!< hotlink slot_id.
	                                    * get it for cavalry_assign_hotlink_slot ioctl;
	                                    * set it for cavalry_release_hotlink_slot ioctl */
	uint32_t reserved[12];             /*!< reserved field */
};

/*! @macros MAX_FEX_WIDTH
 *  @brief define max width for harris FEX.
 */
#define MAX_FEX_WIDTH	(2047)

/*! @macros MAX_FEX_HEIGHT
 *  @brief define max height for harris FEX.
 */
#define MAX_FEX_HEIGHT	(2048)

/*! @macros MAX_STEREO_WIDTH
 *  @brief define max width for stereo.
 */
#define MAX_STEREO_WIDTH	(1920)

/*! @macros MAX_STEREO_HEIGHT
 *  @brief define max height for stereo.
 */
#define MAX_STEREO_HEIGHT	(2048)

struct cavalry_fex_query {
	/* Input */
	uint16_t img_width;  /*!< input image width */
	uint16_t img_height;  /*!< input image height */
	uint16_t img_pitch;  /*!< input image pitch */
	uint16_t harris_en : 2;  /*!<  If the Harris point detector is enabled.
		* 0: Disable; 1: Enable on left image; 2: Enable on right image; 3: Enable on both images */
	uint16_t stereo_en : 1;  /*!< enable stereo engine */
	uint16_t dump_harris_score : 1;  /*!< enable dumping Harris point score */
	uint16_t reserved0 : 12;

	/* Output */
	uint32_t dram_required;  /*!< Memory size needed by FEX returned from Cavalry driver */

	/* Input */
	uint16_t disparity_force_pitch;  /*!< 0 means disparity_pitch is picked automatically. */
	uint32_t affinity;  /*!< FEX Affinity */
	uint8_t priority;  /*!< the priority of FEX when running, range is @sa cavalry_priority_t */
	uint8_t reserved1[45];
};

/*! @enum fex_cfg_mask_t
 *  @brief the FEX configuration bit mask enumeration.
 */
typedef enum {
	FEX_CFG_MASK_NMS_THRESHOLD = (1 << 0),  /*!< 0x1 */
	FEX_CFG_MASK_NMS_WINDOW_FOR_HARRIS = (1 << 1),  /*!< 0x2 */
	FEX_CFG_MASK_DISPARITY_FORCE_PITCH = (1 << 2),  /*!< 0x4 */
	FEX_CFG_MASK_K_FOR_HARRIS = (1 << 3),  /*!< 0x8 */
	FEX_CFG_MASK_ALL = (FEX_CFG_MASK_NMS_THRESHOLD | FEX_CFG_MASK_NMS_WINDOW_FOR_HARRIS |
		FEX_CFG_MASK_DISPARITY_FORCE_PITCH | FEX_CFG_MASK_K_FOR_HARRIS),  /*!< 0xF */
} fex_cfg_mask_t;

struct fex_user_cfg {
	uint32_t cfg_mask;  /*!< Configuration mask. Set the corresponding bits to 1 if the user config-field is enabled */
	uint16_t nms_threshold;  /*!< NMS threshold on Harris score. 4 MSB are exponent bit, 12 LSB are mantissa bit */
	uint16_t nms_window_for_harris : 4;  /*!< NMS window size */
	uint16_t harris_k_exponent : 3;
	uint16_t reserved0 : 1;
	uint16_t harris_k_mantissa : 8;
	uint16_t disparity_force_pitch;  /*!< 0 means disparity_pitch is picked automatically. */
	uint16_t reserved1[3];
};

/*! @enum stereo_profile_t
 *  @brief the Stereo profile enumeration.
 */
typedef enum {
	STEREO_PROFILE_DEFAULT = 0,  /*!< 0 */
	STEREO_PROFILE_1,  /*!< 1 */
	STEREO_PROFILE_2,  /*!< 2 */
	STEREO_PROFILE_3,  /*!< 3 */
	STEREO_PROFILE_NUM,
} stereo_profile_t;

struct cavalry_fex_run {
	/* Input */
	uint16_t img_width;  /*!< Inputs the image width */
	uint16_t img_height;  /*!< Inputs the image height */
	uint16_t img_pitch;  /*!< Inputs the image pitch */
	uint16_t harris_en : 2;  /*!< If the Harris point detector is enabled.
		* 0: Disable; 1: Enables it on the left image; 2: Enables it on the right image; 3: Enables it on the both images. */
	uint16_t stereo_en : 1;  /*!< If the stereo engine is enabled */
	uint16_t stereo_profile : 5;  /*!< Stereo profile number, which corresponds to different disparity styles. @sa stereo_profile_t */
	uint16_t dump_harris_score : 1;  /*!< Enables the dumping Harris score */
	uint16_t img_hflip : 1;  /*!< Enable horizontal flip input image */
	uint16_t reserved1 : 6;
	struct fex_user_cfg fex_cfg;  /*!< User configuration */
	cv_daddr_t output_daddr;  /*!< Specifies the output DRAM address */
	cv_daddr_t luma_daddr[2];  /*!< Luma address for the left image [0]; Luma address for the right image [1] */
	cv_daddr_t config_daddr;  /*!< Specifies the address of stereo engine configuration */
	uint32_t output_size;  /*!< Specifies the output size */
	uint32_t affinity;  /*!< FEX Affinity */
	uint8_t priority;  /*!< the priority of FEX when running, range is @sa cavalry_priority_t */
	uint8_t reserved2[63];

	/* Output */
	cv_daddr_t disparity_daddr;  /*!< Outputs the disparity's DRAM address */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t disparity_size;  /*!< Outputs the disparity's size */
	uint32_t invalid_disparities;  /*!< Number of invalid disparities */
	uint16_t disparity_width;  /*!< Disparity width */
	uint16_t disparity_height;  /*!< Disparity height */
	uint16_t disparity_pitch;  /*!< Disparity pitch, in byte(s) */
	uint16_t reserved3[3];

	cv_daddr_t harris_count_daddr[2];  /*!< DRAM address that gives a list of Harris point count for each block.
		* This list has in total CAVALRY_HARRIS_V_BLOCKS * CAVALRY_HARRIS_H_BLOCKS entries.  Each element is type uint8_t.
		* [0] - DRAM address for the left image Harris point count list; [1] - DRAM address for the right image Harris point count list. */

	uint32_t harris_count_size[2];  /*!< [0] - Size of the Harris count list for the left image; [1] - Size of the Harris count list for the right image */
	cv_daddr_t harris_point_daddr[2];  /*!< The DRAM address points to a list of Harris point coordinates.
		* This list has in total CAVALRY_HARRIS_V_BLOCKS * CAVALRY_HARRIS_H_BLOCKS * CAVALRY_HARRIS_MAX_POINTS_PER_BLOCK entries.
		* Each element's type depends on whether or not the Harris score dumping is enabled.  Refer to the API description above.
		* [0] - DRAM address for the left image's Harris point list; [1] - DRAM address for the right image's Harris point list. */

	uint32_t harris_point_size[2];  /*!< [0] - Size of the Harris points list of the left image; [1] - Size of the Harris points list of the right image */
	cv_daddr_t brief_descriptor_daddr[2];  /*!< DRAM address to a list of BRIEF descriptors.
		* This list has in total CAVALRY_HARRIS_V_BLOCKS * CAVALRY_HARRIS_H_BLOCKS * CAVALRY_HARRIS_MAX_POINTS_PER_BLOCK entries.
		* Each element is a 32-byte BRIEF descriptor.
		* [0] - The DRAM address left image's BRIEF descriptor list; [1] - The DRAM address right image's BRIEF descriptor list. */

	uint32_t brief_descriptor_size[2];  /*!< [0] - Size of the brief descriptor list for the left image; [1] - Size of the brief descriptor list for the right image */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t reserved4[16];
};

struct cavalry_fex_run_mfd {
	/* Input */
	uint16_t img_width;  /*!< Inputs the image width */
	uint16_t img_height;  /*!< Inputs the image height */
	uint16_t img_pitch;  /*!< Inputs the image pitch */
	uint16_t harris_en : 2;  /*!< If the Harris point detector is enabled.
		* 0: Disable; 1: Enables it on the left image; 2: Enables it on the right image; 3: Enables it on the both images. */
	uint16_t stereo_en : 1;  /*!< If the stereo engine is enabled */
	uint16_t stereo_profile : 5;  /*!< Stereo profile number, which corresponds to different disparity styles. @sa stereo_profile_t */
	uint16_t dump_harris_score : 1;  /*!< Enables the dumping Harris score */
	uint16_t img_hflip : 1;  /*!< Enable horizontal flip input image */
	uint16_t reserved1 : 6;
	struct fex_user_cfg fex_cfg;  /*!< User configuration */
	cv_doffset_t output_addr_offset;  /*!< Specifies memory offset base on output_addr_fd (dma-buf:fd) */
	cv_doffset_t luma_addr_offset[2];  /*!< Specifies memory offset base on luma_addr_fd (dma-buf:fd) */
	cv_doffset_t config_daddr_offset;  /*!< Specifies the memory offset based on config_fd (dma-buf:fd) */
	int output_addr_fd;  /*!< Specifies the output dma-buf:fd */
	uint32_t output_size;  /*!< Specifies the output size */
	int luma_addr_fd[2];  /*!< Luma dma-buf:fd for the left image [0]; Luma dma-buf:fd for the right image [1] */
	int config_fd;  /*!< Specifies the dma-buf:fd of stereo engine configuration */
	uint32_t affinity;  /*!< FEX Affinity */
	uint8_t priority;  /*!< the priority of FEX when running, range is @sa cavalry_priority_t */
	uint8_t reserved2[63];

	/* Output */
	cv_doffset_t disparity_addr_offset; /* offset base on output_addr_fd (dma-buf:fd) */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t disparity_size;  /*!< Outputs the disparity's size */
	uint32_t invalid_disparities;  /*!< Number of invalid disparities */
	uint16_t disparity_width;  /*!< Disparity width */
	uint16_t disparity_height;  /*!< Disparity height */
	uint16_t disparity_pitch;  /*!< Disparity pitch, in byte(s) */
	uint16_t reserved3[3];
	cv_doffset_t harris_count_addr_offset[2]; /* memory offset base on output_addr_fd (dma-buf:fd) that gives a list of Harris point count for each block */
	uint32_t harris_count_size[2];  /*!< [0] - Size of the Harris count list for the left image; [1] - Size of the Harris count list for the right image */
	cv_doffset_t harris_point_addr_offset[2]; /* memory offset base on output_addr_fd (dma-buf:fd) points to a list of Harris point coordinates*/
	uint32_t harris_point_size[2];  /*!< [0] - Size of the Harris points list of the left image; [1] - Size of the Harris points list of the right image */
	cv_doffset_t brief_descriptor_addr_offset[2]; /* memory offset base on output_addr_fd (dma-buf:fd) list of BRIEF descriptors */
	uint32_t brief_descriptor_size[2];  /*!< [0] - Size of the brief descriptor list for the left image; [1] - Size of the brief descriptor list for the right image */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t reserved4[16];
};

struct cavalry_fma_query {
	/* Output */
	uint32_t dram_required;  /*!< Memory size needed by FMA returned from Cavalry driver */
	uint32_t affinity;  /*!< FMA Affinity */
	uint8_t priority;  /*!< the priority of FMA when running, range is @sa cavalry_priority_t */
	uint8_t reserved[23];
};

/*! @enum fma_cfg_mask_t
 *  @brief the FMA configuration bit mask enumeration.
 */
typedef enum {
	FMA_CFG_MASK_MIN_THRESHOLD = (1 << 0),   /*!< 0x01 */
	FMA_CFG_MASK_RATIO_THRESHOLD = (1 << 1), /*!< 0x02 */
	FMA_CFG_MASK_WIN_WIDTH = (1 << 2),       /*!< 0x04 */
	FMA_CFG_MASK_WIN_HEIGHT = (1 << 3),      /*!< 0x08 */
	FMA_CFG_MASK_X_THRESHOLD_R = (1 << 4),   /*!< 0x10 */
	FMA_CFG_MASK_X_THRESHOLD_L = (1 << 5),   /*!< 0x20 */
	FMA_CFG_MASK_Y_THRESHOLD_U = (1 << 6),   /*!< 0x40 */
	FMA_CFG_MASK_Y_THRESHOLD_D = (1 << 7),   /*!< 0x80 */
	FMA_CFG_MASK_ALL = FMA_CFG_MASK_MIN_THRESHOLD | FMA_CFG_MASK_RATIO_THRESHOLD |
		FMA_CFG_MASK_WIN_WIDTH | FMA_CFG_MASK_WIN_HEIGHT |
		FMA_CFG_MASK_X_THRESHOLD_R | FMA_CFG_MASK_X_THRESHOLD_L |
		FMA_CFG_MASK_Y_THRESHOLD_U | FMA_CFG_MASK_Y_THRESHOLD_D,  /*!< 0xFF */
} fma_cfg_mask_t;

struct fma_user_cfg {
	uint32_t cfg_mask;  /*!<Configuration mask. Sets the corresponding bits to 1 if a user configuration field is configured. */
	uint32_t min_threshold;  /*!< The minimum matching score of a descriptor pair. Range: [0, 255] */

	uint16_t ratio_threshold;  /*!< The minimum ratio between the best match and the runner up match.  6 MSB are integer bit, 10 LSB are fractional bit. */
	uint16_t reserved;

	uint16_t win_width;  /*!< Horizontal search radius (in block units) */
	uint16_t win_height;  /*!< Vertical search radius (in block units) */

	uint16_t x_threshold_r;  /*!< Considers only the feature point pairs that the X coordinate satisfies:
		* -x_threshold_l <= distx <= x_threshold_r  14 MSB are integer bit, 2 LSB are fractional bit. */
	uint16_t x_threshold_l;  /*!< Considers only the feature point pairs that their X coordinate satisfies:
		* -x_threshold_l <= distx <= x_threshold_r 14 MSB are integer bit, 2 LSB are fractional bit. */
	uint16_t y_threshold_u;  /*!< Considers only the feature point pairs that their Y coordinate satisfies:
		* -y_threshold_u <= disty <= y_threshold_d 14 MSB are integer bit, 2 LSB are fractional bit. */
	uint16_t y_threshold_d;  /*!< Considers only the feature point pairs that their Y coordinate satisfies:
		* -y_threshold_u <= disty <= y_threshold_d 14 MSB are integer bit, 2 LSB are fractional bit. */
};

struct cavalry_fma_run {
	/* Input */
	cv_daddr_t output_daddr;  /*!< The absolute physical address for the FMA output buffer */
	cv_daddr_t target_coord_daddr;  /*!< The absolute physical address of the target list's coordinate list */
	cv_daddr_t target_descriptor_daddr;  /*!< The absolute physical address of the target list's descriptor list */
	cv_daddr_t reference_coord_daddr;  /*!< The absolute physical address of the reference list's coordinate list */
	cv_daddr_t reference_descriptor_daddr;  /*!< The absolute physical address of the reference list's descriptor list */

	uint32_t output_size;  /*!< Size reserved for the FMA output buffer */
	uint32_t mode : 1;	  /*!< FMA mode:  0 - Temporal,  1 - Stereo */
	uint32_t reserved1 : 31;
	struct fma_user_cfg stereo_cfg;  /*!< User configuration for the stereo matching mode */
	struct fma_user_cfg temporal_cfg;  /*!< User configuration for the temporal matching mode */
	uint32_t affinity;  /*!< FMA Affinity */
	uint8_t priority;  /*!< the priority of FMA when running, range is @sa cavalry_priority_t */
	uint8_t reserved2[59];

	/* Output */
	cv_daddr_t result_score_daddr;  /*!< The absolute physical address of the matching score list.
		* This list has CAVALRY_HARRIS_V_BLOCKS * CAVALRY_HARRIS_H_BLOCKS * CAVALRY_HARRIS_MAX_POINTS_PER_BLOCK entries
		* if the FEX feature point list is provided as the target list.
		* Each element is type uint8_t, and represents the matching score. */
	cv_daddr_t result_index_daddr;  /*!< The absolute physical address of matching index list.
		* This list has CAVALRY_HARRIS_V_BLOCKS * CAVALRY_HARRIS_H_BLOCKS * CAVALRY_HARRIS_MAX_POINTS_PER_BLOCK entries
		* if the FEX feature point list is provided as the target list.
		* Each element is type uint16_t, and indicates the index of the matched feature point in the reference list. */
	cv_daddr_t temporal_coord_daddr;  /*!< The absolute physical address of the updated feature point coordinate list.
		* It is valid only in the stereo matching mode. */

	uint32_t result_index_size;  /*!< Size of the output matching index list */
	uint32_t result_score_size;  /*!< Size of the matching score list */
	uint32_t temporal_coord_size;  /*!< Size of the feature point coordinate list.
		* It is valid only in the stereo matching mode. */

	uint32_t rval;	/*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t reserved3[16];
};

struct cavalry_fma_run_mfd {
	/* Input */
	int output_addr_fd;  /*!< The dma-buf:fd for the FMA output buffer */
	uint32_t output_size;  /*!< Size reserved for the FMA output buffer */
	cv_doffset_t output_addr_offset;  /*!< The offset base on output_addr_fd (dma-buf:fd) */

	int target_coord_addr_fd;  /*!< The dma-buf:fd of the target list's coordinate list */
	int target_descriptor_addr_fd;  /*!< The dma-buf:fd of the target list's descriptor list */
	cv_doffset_t target_coord_addr_offset;  /*!< The offset base on target_coord_addr_fd (dma-buf:fd) */
	cv_doffset_t target_descriptor_addr_offset;  /*!< The offset base on target_descriptor_addr_fd (dma-buf:fd) */
	int reference_coord_addr_fd;  /*!< The dma-buf:fd of the reference list's coordinate list */
	int reference_descriptor_addr_fd;   /*!< The dma-buf:fd of the reference list's descriptor list */
	cv_doffset_t reference_coord_addr_offset;  /*!< The offset base on reference_coord_addr_fd (dma-buf:fd) */
	cv_doffset_t reference_descriptor_addr_offset;  /*!< The offset base on reference_descriptor_addr_fd (dma-buf:fd) */

	struct fma_user_cfg stereo_cfg;  /*!< User configuration for the stereo matching mode */
	struct fma_user_cfg temporal_cfg;  /*!< User configuration for the temporal matching mode */
	uint32_t mode : 1;  /*!< FMA mode:  0 - Temporal,  1 - Stereo */
	uint32_t reserved1 : 31;
	uint32_t affinity;  /*!< FMA Affinity */
	uint8_t priority;  /*!< the priority of FMA when running, range is @sa cavalry_priority_t */
	uint8_t reserved2[63];

	/* Output */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	cv_doffset_t result_score_addr_offset;  /*!< The offset base on output_addr_fd (dma-buf:fd) */
	uint32_t result_score_size;  /*!< Size of the matching score list */
	cv_doffset_t result_index_addr_offset;  /*!< The offset base on output_addr_fd (dma-buf:fd) */
	uint32_t result_index_size;  /*!< Size of the output matching index list */
	cv_doffset_t temporal_coord_addr_offset;  /*!< The offset base on output_addr_fd (dma-buf:fd) */
	uint32_t temporal_coord_size;  /*!< Size of the feature point coordinate list.
		* It is valid only in the stereo matching mode. */
	uint32_t start_tick;  /*!< start audio tick, record by cavalry ucode when received this job */
	uint32_t end_tick;  /*!< end audio tick, record by cavalry ucode when finished this job */
	uint32_t reserved3[16];
};

struct cavalry_stats_get {
	cavalry_job_type_t job_type;  /*!< job type send to cavalry ucode, @sa cavalry_job_type_t */
	int current_pid;  /*!< current HW running pid */
	int32_t current_nid;  /*!< current HW running network ID */

	uint32_t hw_type;  /*!< enum of @cavalry_hw_type_t */
	uint32_t hw_id;  /*!< id of current HW */
	uint32_t is_hw_idle : 1;  /*!< if HW is idle */
	uint32_t submitter_hw_id : 7;  /*!< id of the HW who submitted the job */
	uint32_t is_hw_hang : 1;
	uint32_t reserved : 23;
};

struct cavalry_stats_all {
	struct cavalry_stats_get stats_get[CV_HW_TOTAL_NUM];
	uint32_t reserved[4];
};

struct cavalry_status {
	uint32_t is_cavalry_started : 1;  /*!< 0: Stopped;  1: Started after issue ioctl @CAVALRY_START_VP */
	uint32_t reserved0 : 31;
	uint32_t reserved[7];
};

struct cavalry_hw_core_dump {
	cv_daddr_t core_dump_daddr;  /*!< the absolute phsical address for cavalry core dump */
	uint32_t core_dump_size;  /*!< the memory size for cavalry core dump */

	uint32_t hw_type;  /*!< enum of @cavalry_hw_type_t */
	uint32_t hw_id;  /*!< id of current HW */
	uint32_t is_hw_hang : 1;  /*!< if HW is hang */
	uint32_t reserved0 : 31;
};

struct cavalry_core_dump {
	struct cavalry_hw_core_dump hw_core[CV_HW_TOTAL_NUM];  /*!< arrary for cv hardware */
	uint32_t reserved[4];
};

/*! @enum cavalry_monitor_err_t
 *  @brief cavalry monitor error event enumeration.
 */
typedef enum {
	MONITOR_ERR_VP_HANG = 0,   /*!< 0 */
	MONITOR_ERR_FEX_HANG = 1,  /*!< 1 */
	MONITOR_ERR_FMA_HANG = 2,  /*!< 2 */
	MONITOR_ERR_NUM,
	MONITOR_ERR_FIRST = MONITOR_ERR_VP_HANG,
	MONITOR_ERR_LAST = MONITOR_ERR_FMA_HANG,
} cavalry_monitor_err_t;

/*! @enum cavalry_monitor_info_t
 *  @brief cavalry monitor info event enumeration.
 */
typedef enum {
	MONITOR_INFO_VP_TIMEOUT = 0,  /*!< 0 */
	MONITOR_INFO_FEX_TIMEOUT = 1,  /*!< 1 */
	MONITOR_INFO_FMA_TIMEOUT = 2,  /*!< 2 */
	MONITOR_INFO_NUM,
	MONITOR_INFO_FIRST = MONITOR_INFO_VP_TIMEOUT,
	MONITOR_INFO_LAST = MONITOR_INFO_FMA_TIMEOUT,
} cavalry_monitor_info_t;

struct cavalry_monitor_msg_err {
	uint64_t tv_sec;  /*!< seconds of time stamp */
	uint64_t tv_usec;  /*!< micro seconds of time stamp */
	cavalry_monitor_err_t err_code;  /*!< error code from cavalry monitor */
	uint32_t line_num;  /*!< source file line number when error event happened */
	char func_name[CAVALRY_MONITOR_MSG_SIZE];  /*!< source file function name when error event happened */
	char msg_string[CAVALRY_MONITOR_MSG_SIZE];  /*!< error message string */
	uint32_t  extra_size;  /*!< payload size */
	char extra_data[0];  /*!< payload buffer pointer */
};

struct cavalry_monitor_msg_info {
	uint64_t tv_sec;  /*!< seconds of time stamp */
	uint64_t tv_usec;  /*!< micro seconds of time stamp */
	cavalry_monitor_info_t info_code;  /*!< info code from cavalry monitor */
	uint32_t line_num;  /*!< source file line number when info event happened */
	char func_name[CAVALRY_MONITOR_MSG_SIZE];  /*!< source file function name when info event happened */
	char msg_string[CAVALRY_MONITOR_MSG_SIZE];  /*!< info message string */
	uint32_t  extra_size;  /*!< payload size */
	char extra_data[0];  /*!< payload buffer pointer */
};

struct cavalry_dram_cfg {
	uint32_t burst_size;  /*!< DRAM burst size */

	uint32_t reserved[5];
};

struct cavalry_clock_cfg {
	uint32_t vp_clk_flag : 1;  /*!< If the VP clock is set */
	uint32_t reserved_1 : 31;

	uint32_t vp_clk_numerator;  /*!< Numerator for percentage */
	uint32_t vp_clk_denominator;  /*!< Numerator for percentage */

	uint32_t reserved_2[4];
};

struct cavalry_create_session {
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t session_id;  /*!< session id */

	uint32_t no_recycle : 1;  /*!< enables or disables the auto recycle session resource.
		* 0: auto recycle session resource if process abnormal exit;  1: session resource is persist if process exit */
	uint32_t reserved_0 : 31;
	uint32_t reserved_1;
};

struct cavalry_free_session {
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t session_id;  /*!< session id */
	uint32_t reserved_0[2];
};

struct cavalry_dma_copy {
	unsigned long src_phys;  /*!< source physical address */
	unsigned long dst_phys;  /*!< destination physical address */
	unsigned long size;  /*! size to copy */
	uint32_t reserved_0[8];
};

/*! @macros ULP_VERIFY_LICENSE_OK
 *  @brief define if ulp_verify_license pass.
 */
#define ULP_VERIFY_LICENSE_OK  (0)

/*! @macros PUBLIC_DIK_SIZE
 *  @brief define public dik size for query ULP dik.
 */
#define PUBLIC_DIK_SIZE	(32)

/*! @macros CAVALRY_DIGEST_LENGTH
 *  @brief define digest length for encrypt.
 */
#define CAVALRY_DIGEST_LENGTH	(32)

struct customer_certificate {  /* size 256 bytes */
	uint8_t pub_vlk[32];  /*!< Public Vendor License Key */
	uint8_t reserved[32];  /*!< reserved field */
	uint8_t pub_apk[32];  /*!< Public Amba Platform Key,
		* which is used to guarantee information is from real chip, not a SW simulator */

	uint8_t vendor_name[32];  /*!< Vendor name */
	uint8_t product_info[32];  /*!< product infomation */
	uint8_t metadata[32];  /*!< metadata */

	uint8_t amba_signature[64];  /*!< ambarella signature */
};

struct ulp_license {  /* size 256 bytes */
	uint8_t pub_dik[32];  /*!< Public device identification key */
	uint8_t metadata[160];  /*!< Vendor's metadata */

	uint8_t signature[64];  /*!< signature signed by private VLK (Vendor License Key) */
};

struct ulp_challenge {  /* size 192 bytes */
	/* Input */
	uint8_t challenge[32];  /*!< challenge */

	/* Output */
	uint32_t result;  /*!<  result of ulp verfiy license, pass if it equal to @sa ULP_VERIFY_LICENSE_OK */
	uint8_t reserved[28];  /*!< reserved field */
	uint8_t amba_platform_signature[64];  /*!< ambarella platform signature */
	uint8_t device_signature[64];  /*!< device signature */
};

struct aes256_key {  /* size 64 bytes */
     uint32_t key[8];  /*!< key */
     uint32_t iv[4];  /*!< initial vector */
     uint32_t padding[4];  /*!< padding */
};

struct nmp_key_exchange { /* size 160 bytes*/
     uint8_t data[32];  /*!< data */

     uint8_t signature_1[64];  /*!< signature_1 */
     uint8_t signature_2[64];  /*!< signature_2 */
};

struct cavalry_ulp_init {
	/* Input */
	uint32_t session_id;  /*!< session id */
	struct customer_certificate certificate;  /*!< certificate */

	/* Output */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t reserved[15];
};

struct cavalry_ulp_query_dik {
	/* Input */
	uint32_t session_id;  /*!< session id */

	/* Output */
	uint8_t pub_dik[PUBLIC_DIK_SIZE];  /*!< Public device identification key */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t reserved[15];
};

struct cavalry_ulp_verfiy_license {
	/* Input */
	uint32_t session_id;  /*!< session id */
	struct ulp_license license;  /*!< ulp license */

	/* Output */
	struct ulp_challenge challenge;  /*!< struct ulp_challenge.challenge is IN and OUT */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t reserved[15];
};

struct cavalry_nmp_key_exchange {
	/* Input */
	uint32_t session_id;  /*!< session id */
	cavalry_nmp_key_exchange_type_t type;  /*!< PEK: normal world,  AEK: security world */
	struct nmp_key_exchange key_exchange_remote;  /*!< specify by application */

	/* Output */
	struct nmp_key_exchange key_exchange_self;  /*!< feed back from VP */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t reserved[15];
};

struct cavalry_nmp_convert_key {
	/* Input */
	uint32_t session_id;  /*!< session id */
	struct aes256_key transfered_pek;  /*!< transfered pek */

	/* Output */
	struct aes256_key dek_encrypted_pek;  /*!< re-encrypted key */
	uint8_t digest[CAVALRY_DIGEST_LENGTH];  /*!< returned digest */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t reserved[15];
};

struct cavalry_convert_after_encrypt {
	/* Input */
	uint32_t session_id;  /*!< session id */
	struct aes256_key dek_encrypted_pek;  /*!< re-encrypted key */
	uint8_t digest[CAVALRY_DIGEST_LENGTH];  /*!< digest */
	uint32_t input_checksum;  /*!< original data checksum */
	uint32_t input_size;  /*!< input data size */
	unsigned long input_phys_daddr;  /*!< input data absolute physical address, should be 4K aligned */
	unsigned long output_phys_daddr;  /*!< output data absolute physical address, should be 4K aligned */

	/* Output */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t sub_session_id;  /*!< input sub session id */
	uint32_t reserved[14];
};

struct cavalry_convert_after_encrypt_mfd {
	/* Input */
	uint32_t session_id;  /*!< session id */
	struct aes256_key dek_encrypted_pek;  /*!< re-encrypted key */
	uint8_t digest[CAVALRY_DIGEST_LENGTH];  /*!< digest */
	uint32_t input_checksum;  /*!< original data checksum */
	uint32_t input_size;  /*!< input data size */
	int input_daddr_fd;  /*!< input data dma-buf:fd */
	int output_daddr_fd;  /*!< output data dambuf:fd */
	unsigned long input_daddr_offset;  /*!< memory offset base on input_daddr_fd (dma-buf:fd) */
	unsigned long output_daddr_offset;  /*!< memory offset base on output_daddr_fd (dma-buf:fd) */

	/* Output */
	uint32_t rval;  /*!< return code from cavalry ucode, @sa cavalry_msg_rval_t */
	uint32_t sub_session_id;  /*!< input sub session id */
	uint32_t reserved[14];
};

struct cavalry_ucode_cmd_size {
	uint32_t cmd_id;  /*!< input ucode cmd id */
	uint32_t dag_cnt;  /*!< input dag count */
	uint32_t reserved[4];

	uint32_t cmd_size;  /*!< output ucode cmd size */
};

/*! @macros CAVALRY_PROFILE_ENTRY_SIZE
 *  @brief define one profile entry size, it equals sizeof(struct cavalry_profile_entry) + max(payload_size).
 */
#define CAVALRY_PROFILE_ENTRY_SIZE (64)  /* 32 + 20 = 52 < 64 */

/*! @macros profile_payload_offset
 *  @brief define cavalry profile payload's offset base on cavalry profile entry.
 */
#define profile_payload_offset(x) (((void *)(x)) + sizeof(struct cavalry_profile_entry))

/*! @enum cavalry_hw_type_t
 *  @brief cavalry hw type enumeration.
 */
typedef enum {
	CAVALRY_HW_TYPE_NVP = 0,
	CAVALRY_HW_TYPE_GVP = 1,
	CAVALRY_HW_TYPE_FEX = 2,
	CAVALRY_HW_TYPE_FMA = 3,
	CAVALRY_HW_TYPE_NUM,
	CAVALRY_HW_TYPE_ANY = 0xFF,
	CAVALRY_HW_TYPE_FIRST = CAVALRY_HW_TYPE_NVP,
	CAVALRY_HW_TYPE_LAST = CAVALRY_HW_TYPE_FMA,
} cavalry_hw_type_t;

struct cavalry_profile_payload_run_dags {
	int nid;  /*!< network ID that report from nnctrl library */

	uint32_t dag_index;  /*!< finished dag cnt in current schedule,
		* network is finished when finish_dags=dag_cnt */
	uint32_t dag_cnt;  /*!< total dag cnt of current network */

	uint32_t dag_loop_index;  /*!< finished dag looping cnt in current interrupted dag,
		* dag looping is finished when finish_dag_loops=dag_loop_cnt */
	uint32_t dag_loop_cnt;  /*!< total dag looping run count */

	uint32_t dag_load_ticks; /*!< audio tick, record by cavalry ucode for load dag */
	uint32_t dag_exec_ticks; /*!< audio tick, record by cavalry ucode for execute dag */
	uint32_t dag_store_ticks;
};

struct cavalry_profile_entry {
	uint32_t seq_num;  /*!< the sequence number for cavalry log */
	uint32_t cmd_id;  /*!< issued ucode cmd id */

	uint8_t hw_type;  /*!< enum cavalry_hw_type */
	uint8_t hw_instance_id;  /*!< instance id of current hardware */
	uint8_t priority;  /*!< the priority of job, range is @sa cavalry_priority_t */
	uint8_t affinity;  /*!< the affinity */

	uint32_t pid;  /*!< process ID. 0: Job from IDSP;  others: Job from ARM */

	uint32_t submit_tick;  /*!< audio tick, record by cavalry ucode when submit this job */
	uint32_t start_tick;  /*!< audio tick, record by cavalry ucode when start execute this job */
	uint32_t end_tick;  /*!< audio tick, record by cavalry ucode when finish this job */
	uint8_t submitter_hw_id;  /*!< id of the HW who submitted the job */
	uint8_t reserved[3];

	uint8_t payload[0];  /*!< payload, like @struct cavalry_profile_payload_run_dags  */
};

/*! @} */  /* End of cavalry-ioctl-helper */


/*! @addtogroup cavalry-ioctl-api
 * @{
 */

/*!
 * This API queries the physical addresses for all memory partitions shared
 * between Cavalry and the user application and the memory blocks inside the VP buffer.
 * The start addresses and lengths of all memory partitions are returned in information.
 * The application should further map the physical addresses into
 * user space virtual addresses before operating the buffers.
 */
#if defined(__QNXNTO__)
#define CAVALRY_QUERY_BUF		_IOWR ('C', 0x0, struct cavalry_querybuf)
#define CAVALRY_START_VP		_IOWR ('C', 0x1, void *)
#define CAVALRY_STOP_VP			_IOWR ('C', 0x2, void *)
#define CAVALRY_RUN_DAGS		_IOWR ('C', 0x3, struct cavalry_run_dags)
#define CAVALRY_START_LOG		_IOWR ('C', 0x4, void *)
#define CAVALRY_STOP_LOG		_IOWR ('C', 0x5, void *)
#define CAVALRY_EARLY_QUIT	_IOWR ('C', 0x6, struct cavalry_early_quit)
#define CAVALRY_ALLOC_MEM		_IOWR ('C', 0x7, struct cavalry_mem)
#define CAVALRY_FREE_MEM		_IOWR ('C', 0x8, struct cavalry_mem)
#define CAVALRY_SYNC_CACHE_MEM	_IOWR ('C', 0x9, struct cavalry_cache_mem)
#define CAVALRY_GET_USAGE_MEM	_IOWR ('C', 0xA, struct cavalry_usage_mem)
#define CAVALRY_RUN_HOTLINK_SLOT  _IOWR ('C', 0xB, struct cavalry_run_hotlink_slot)
#define CAVALRY_ASSIGN_HOTLINK_SLOT	_IOWR ('C', 0xC, struct cavalry_hotlink_slot_desc)
#define CAVALRY_RELEASE_HOTLINK_SLOT	_IOWR ('C', 0xD, struct cavalry_hotlink_slot_desc)
#define CAVALRY_FEX_QUERY	_IOWR ('C', 0xE, struct cavalry_fex_query)
#define CAVALRY_FEX_RUN	_IOWR ('C', 0xF, struct cavalry_fex_run)
#define CAVALRY_FMA_QUERY	_IOWR ('C', 0x10, struct cavalry_fma_query)
#define CAVALRY_FMA_RUN	_IOWR ('C', 0x11, struct cavalry_fma_run)
#define CAVALRY_SET_LOG_LEVEL	_IOWR ('C', 0x12, struct cavalry_set_log_level)
#define CAVALRY_GET_STATS	_IOWR ('C', 0x13, struct cavalry_stats_all)
#define CAVALRY_QUERY_VP_CORE_DUMP	_IOWR ('C', 0x14, struct cavalry_core_dump)
#define CAVALRY_QUERY_UCODE_CMD_SIZE	_IOWR ('C', 0x15, struct cavalry_ucode_cmd_size)
#define CAVALRY_CREATE_SESSION	_IOWR ('C', 0x20, struct cavalry_create_session)
#define CAVALRY_FREE_SESSION	_IOWR ('C', 0x21, struct cavalry_free_session)
#define CAVALRY_ULP_INIT	_IOWR ('C', 0x22, struct cavalry_ulp_init)
#define CAVALRY_ULP_QUERY_DIK	_IOWR ('C', 0x23, struct cavalry_ulp_query_dik)
#define CAVALRY_NMP_KEY_EXCHANGE	_IOWR ('C', 0x24, struct cavalry_nmp_key_exchange)
#define CAVALRY_NMP_CONVERT_KEY	_IOWR ('C', 0x25, struct cavalry_nmp_convert_key)
#define CAVALRY_CONVERT_AFTER_ENCRYPT	_IOWR ('C', 0x27, struct cavalry_convert_after_encrypt)
#define CAVALRY_DMA_COPY	_IOWR ('C', 0x28, struct cavalry_dma_copy)
#define CAVALRY_ULP_VERIFY_LICENSE	_IOWR ('C', 0x29, struct cavalry_ulp_verfiy_license)
#define CAVALRY_ALLOC_MEMFD		_IOWR ('C', 0x40, struct cavalry_mfd_alloc)
#define CAVALRY_SYNC_CACHE_MEMFD	_IOWR ('C', 0x41, struct cavalry_mfd_sync)
#define CAVALRY_RUN_DAGS_MEMFD	_IOWR ('C', 0x42, struct cavalry_run_dags_mfd)
#define CAVALRY_CONVERT_AFTER_ENCRYPT_MEMFD	_IOWR ('C', 0x45, struct cavalry_convert_after_encrypt_mfd)
#define CAVALRY_FEX_RUN_MEMFD	_IOWR ('C', 0x46, struct cavalry_fex_run_mfd)
#define CAVALRY_FMA_RUN_MEMFD	_IOWR ('C', 0x47, struct cavalry_fma_run_mfd)
#define CAVALRY_GET_AUDIO_CLK	_IOR ('C', 0x80, uint64_t)
#define CAVALRY_SET_CAVALRY_CLK	_IOW ('C', 0x81, struct cavalry_clock_cfg)
#define CAVALRY_GET_CAVALRY_CLK	_IOWR ('C', 0x82, struct cavalry_clock_cfg)
#define CAVALRY_GET_DRAM_CFG	_IOWR ('C', 0x83, struct cavalry_dram_cfg)
#define CAVALRY_START_PROFILE		_IOWR ('C', 0x84, void *)
#define CAVALRY_STOP_PROFILE		_IOWR ('C', 0x85, void *)
#define CAVALRY_QUERY_MEM_ATTR	_IOWR ('C', 0x86, void *)
#define CAVALRY_GET_CV_CHIP_ID	_IOWR ('C', 0x87, uint32_t)
#define CAVALRY_GET_CAVALRY_STATUS	_IOWR ('C', 0x88, struct cavalry_status)
#define CAVALRY_QUERY_USAGE_MEM	_IOWR ('C', 0x89, struct cavalry_mem_usage_query)
#define CAVALRY_QUERY_HOTLINK_SLOT_BUF_CFG	_IOWR ('C', 0x90, struct cavalry_hotlink_slot_cfg)
#define CAVALRY_GET_ARM_CLUSTER_ID	_IOWR ('C', 0x91, uint32_t)
#define CAVALRY_GET_DRIVER_VERSION	_IOWR ('C', 0x92, struct cavalry_driver_version)
#define CAVALRY_APPEND_TO_MEMPOOL		_IOWR ('C', 0xA0, struct cavalry_buddy_mem)
#define CAVALRY_REMOVE_FROM_MEMPOOL		_IOWR ('C', 0xA1, struct cavalry_buddy_mem)
#else
#define CAVALRY_QUERY_BUF		_IOWR ('C', 0x0, struct cavalry_querybuf *)
#define CAVALRY_START_VP		_IOWR ('C', 0x1, void *)
#define CAVALRY_STOP_VP			_IOWR ('C', 0x2, void *)
#define CAVALRY_RUN_DAGS		_IOWR ('C', 0x3, struct cavalry_run_dags *)
#define CAVALRY_START_LOG		_IOWR ('C', 0x4, void *)
#define CAVALRY_STOP_LOG		_IOWR ('C', 0x5, void *)
#define CAVALRY_EARLY_QUIT	_IOWR ('C', 0x6, struct cavalry_early_quit *)
#define CAVALRY_ALLOC_MEM		_IOWR ('C', 0x7, struct cavalry_mem *)
#define CAVALRY_FREE_MEM		_IOWR ('C', 0x8, struct cavalry_mem *)
#define CAVALRY_SYNC_CACHE_MEM	_IOWR ('C', 0x9, struct cavalry_cache_mem *)
#define CAVALRY_GET_USAGE_MEM	_IOWR ('C', 0xA, struct cavalry_usage_mem *)
#define CAVALRY_RUN_HOTLINK_SLOT  _IOWR ('C', 0xB, struct cavalry_run_hotlink_slot *)
#define CAVALRY_ASSIGN_HOTLINK_SLOT	_IOWR ('C', 0xC, struct cavalry_hotlink_slot_desc *)
#define CAVALRY_RELEASE_HOTLINK_SLOT	_IOWR ('C', 0xD, struct cavalry_hotlink_slot_desc *)
#define CAVALRY_FEX_QUERY	_IOWR ('C', 0xE, struct cavalry_fex_query *)
#define CAVALRY_FEX_RUN	_IOWR ('C', 0xF, struct cavalry_fex_run *)
#define CAVALRY_FMA_QUERY	_IOWR ('C', 0x10, struct cavalry_fma_query *)
#define CAVALRY_FMA_RUN	_IOWR ('C', 0x11, struct cavalry_fma_run *)
#define CAVALRY_SET_LOG_LEVEL	_IOWR ('C', 0x12, struct cavalry_set_log_level *)
#define CAVALRY_GET_STATS	_IOWR ('C', 0x13, struct cavalry_stats_all *)
#define CAVALRY_QUERY_VP_CORE_DUMP	_IOWR ('C', 0x14, struct cavalry_core_dump *)
#define CAVALRY_QUERY_UCODE_CMD_SIZE	_IOWR ('C', 0x15, struct cavalry_ucode_cmd_size *)
#define CAVALRY_CREATE_SESSION	_IOWR ('C', 0x20, struct cavalry_create_session *)
#define CAVALRY_FREE_SESSION	_IOWR ('C', 0x21, struct cavalry_free_session *)
#define CAVALRY_ULP_INIT	_IOWR ('C', 0x22, struct cavalry_ulp_init *)
#define CAVALRY_ULP_QUERY_DIK	_IOWR ('C', 0x23, struct cavalry_ulp_query_dik *)
#define CAVALRY_NMP_KEY_EXCHANGE	_IOWR ('C', 0x24, struct cavalry_nmp_key_exchange *)
#define CAVALRY_NMP_CONVERT_KEY	_IOWR ('C', 0x25, struct cavalry_nmp_convert_key *)
#define CAVALRY_CONVERT_AFTER_ENCRYPT	_IOWR ('C', 0x27, struct cavalry_convert_after_encrypt *)
#define CAVALRY_DMA_COPY	_IOWR ('C', 0x28, struct cavalry_dma_copy *)
#define CAVALRY_ULP_VERIFY_LICENSE	_IOWR ('C', 0x29, struct cavalry_ulp_verfiy_license *)
#define CAVALRY_ALLOC_MEMFD		_IOWR ('C', 0x40, struct cavalry_mfd_alloc *)
#define CAVALRY_SYNC_CACHE_MEMFD	_IOWR ('C', 0x41, struct cavalry_mfd_sync *)
#define CAVALRY_RUN_DAGS_MEMFD	_IOWR ('C', 0x42, struct cavalry_run_dags_mfd *)
#define CAVALRY_CONVERT_AFTER_ENCRYPT_MEMFD	_IOWR ('C', 0x45, struct cavalry_convert_after_encrypt_mfd *)
#define CAVALRY_FEX_RUN_MEMFD	_IOWR ('C', 0x46, struct cavalry_fex_run_mfd *)
#define CAVALRY_FMA_RUN_MEMFD	_IOWR ('C', 0x47, struct cavalry_fma_run_mfd *)
#define CAVALRY_GET_AUDIO_CLK	_IOR ('C', 0x80, uint64_t *)
#define CAVALRY_SET_CAVALRY_CLK	_IOW ('C', 0x81, struct cavalry_clock_cfg *)
#define CAVALRY_GET_CAVALRY_CLK	_IOWR ('C', 0x82, struct cavalry_clock_cfg *)
#define CAVALRY_GET_DRAM_CFG	_IOWR ('C', 0x83, struct cavalry_dram_cfg *)
#define CAVALRY_START_PROFILE		_IOWR ('C', 0x84, void *)
#define CAVALRY_STOP_PROFILE		_IOWR ('C', 0x85, void *)
#define CAVALRY_QUERY_MEM_ATTR	_IOWR ('C', 0x86, void *)
#define CAVALRY_GET_CV_CHIP_ID	_IOWR ('C', 0x87, uint32_t *)
#define CAVALRY_GET_CAVALRY_STATUS	_IOWR ('C', 0x88, struct cavalry_status *)
#define CAVALRY_QUERY_USAGE_MEM	_IOWR ('C', 0x89, struct cavalry_mem_usage_query *)
#define CAVALRY_QUERY_HOTLINK_SLOT_BUF_CFG	_IOWR ('C', 0x90, struct cavalry_hotlink_slot_cfg *)
#define CAVALRY_GET_ARM_CLUSTER_ID	_IOWR ('C', 0x91, uint32_t *)
#define CAVALRY_GET_DRIVER_VERSION	_IOWR ('C', 0x92, struct cavalry_driver_version *)
#define CAVALRY_APPEND_TO_MEMPOOL		_IOWR ('C', 0xA0, struct cavalry_buddy_mem *)
#define CAVALRY_REMOVE_FROM_MEMPOOL		_IOWR ('C', 0xA1, struct cavalry_buddy_mem *)
#endif


/*! @} */  /* End of cavalry-ioctl-api */

#endif //__CAVALRY_IOCTL_H__
