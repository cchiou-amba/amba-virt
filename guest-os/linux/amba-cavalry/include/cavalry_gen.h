/*
 * cavalry_gen.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef __CAVALRY_GEN_H__
#define __CAVALRY_GEN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define CAVALRY_GEN_VER_MAJOR		(0x3)
#define CAVALRY_GEN_VER_MINOR		(0x0)
#define CAVALRY_GEN_VER_PATCH		(0x4)
#define CAVALRY_GEN_MAGIC_ID		(0xFEEDBEEF)

#define CAVALRY_VAR_NAME_MAX				(128)
#define CAVALRY_IO_NAME_MAX				(256)
#define CAVALRY_IO_DEMNGL_NAME_MAX		(256)
#define CAVALRY_IO_PARENT_NAME_MAX		(64)
#define CAVALRY_VPROC_VAR_NAME_MAX		(32)
#define CAVALRY_VPROC_SMB_NAME_MAX		(16)

/* cavalry_gen will set invalid value if not found data_format */
#define DATASIZE_INVALID			(0xFF)

typedef enum {
	CAVALRY_BIN_UNENCRYPTED = 0,
	CAVALRY_BIN_ENCRYPTED_BY_PEK = 1
} cavalry_bin_state;

typedef enum {
	CAVALRY_VDG_ARCH_BYPASS = 0x0,
	CAVALRY_VDG_ARCH_CV22 = 0x1,
	CAVALRY_VDG_ARCH_CV2 = 0x2,
	CAVALRY_VDG_ARCH_CV25 = 0x3,
	CAVALRY_VDG_ARCH_CV28 = 0x4,
	CAVALRY_VDG_ARCH_CV5 = 0x5,
	CAVALRY_VDG_ARCH_CV52 = 0x6,
	CAVALRY_VDG_ARCH_CV3 = 0x7,
	CAVALRY_VDG_ARCH_N1 = 0x7,
	CAVALRY_VDG_ARCH_CV72 = 0x8,
	CAVALRY_VDG_ARCH_CV75 = 0x9,
	CAVALRY_VDG_ARCH_CV3AD685 = 0xA,
	CAVALRY_VDG_ARCH_CV3AD655 = 0xB,
	CAVALRY_VDG_ARCH_N1_655 = 0xB,
	CAVALRY_VDG_ARCH_CV7 = 0xC,
	CAVALRY_VDG_ARCH_CV8 = 0xD
} cavalry_vdg_arch_t;


typedef enum {
	CAVALRY_BIN_NORM = 0x0,
	CAVALRY_BIN_SLIM = 0x1,
	CAVALRY_BIN_COMM = 0x2,
} cavalry_bin_type_t;

/* header contains version and data struct size info */
typedef struct cavalry_gen_header_s {
	uint8_t version_major;
	uint8_t version_minor;
	uint8_t version_patch;
	uint8_t reserve_0;
	uint32_t version_hash;
	uint32_t magic_ident;
	uint32_t vdg_arch;

	uint32_t graph_body_num;
	uint32_t graph_body_desc_size;
	uint32_t graph_node_num;
	uint32_t graph_node_desc_size;
	uint32_t graph_node_joint_num;
	uint32_t graph_node_joint_desc_size;
	uint32_t jsonf_num;
	uint32_t jsonf_desc_size;

	uint32_t dvi_desc_size;
	uint32_t io_desc_size;
	uint32_t const_port_desc_size;
	uint32_t loop_port_desc_size;
	uint32_t ext_lib_desc_size;

	uint32_t vproc_desc_size;
	uint32_t func_var_size;
	uint32_t smb_desc_size;
	uint32_t vproc_info_offset;

	uint32_t usr_info_offset;
	uint32_t usr_info_size;

	uint32_t group_id;
	uint8_t encrypt_state;
	uint8_t bin_type;					/* cavalry_bin_type_t */
	uint8_t reserve_1[2];

	uint32_t dvi_chunk_desc_size;
	uint32_t dvi_comm_weight_desc_size;
	uint32_t comm_bin_weight_num;	/* number of comm_weight_info_t in comm.bin */
	uint64_t slim_comm_pair_id;		/* sha256, slim.bin & comm.bin */

	uint32_t reserve_2[98];
} cavalry_gen_header_t;

/* coprocessor type */
typedef enum {
	COPROC_NVP = 0,
	COPROC_GVP = 1,
	COPROC_ARM = 2,
	COPROC_GPU = 3,
	COPROC_VP = 4,
} coproc_type_t;

/* logic control type */
typedef enum {
	LOGIC_NORM = 0,
	LOGIC_IF = 1,
	LOGIC_LOOP = 2,
	LOGIC_LSTM = 3,
	LOGIC_BATCHCALL = 4,
	LOGIC_CALL = 5,
} logic_type_t;

/* lstm network loop direction */
typedef enum {
	LSTM_FWD = 0,					/* forward */
	LSTM_BWD = 1,					/* reverse */
	LSTM_BIDIRECTION = 2,			/* bidirectional */
} lstm_direction_t;

/* graph nested structure */
typedef struct graph_body_s {
	char name[CAVALRY_VAR_NAME_MAX];
	uint32_t id;
	uint32_t node_num;
	uint32_t reserve[32];
} graph_body_t;

typedef struct graph_node_s {
	char name[CAVALRY_VAR_NAME_MAX];
	uint32_t id;
	uint32_t reserve_2[33];
	uint32_t belong_to_body;
	uint32_t produce_body;			/* valid if logic_type is LOGIC_LOOP/LSTM/BATCHCALL/CALL */
	uint32_t produce_if_body;			/* valid if logic_type is LOGIC_IF */
	uint32_t produce_else_body;		/* valid if logic_type is LOGIC_IF */
	uint8_t coproc_type;
	uint8_t logic_type;
	uint8_t direction;					/* loop direction */
	uint8_t reserve_0;

	uint32_t loop_iter_cnt;				/* loop iteration */
	uint32_t const_port_num;			/* N x const_port_desc_t */
	uint32_t loop_port_pair_num;		/* N x port_pair_t */

	uint32_t dvi_num;					/* dvi or ext lib */
	uint32_t dvi_begin_id;
	uint32_t ext_lib_num;
	uint32_t reserve_1;
	uint64_t node_pkg_size;

	uint32_t checksum;
	uint32_t encrypt_tail_size;
} graph_node_t;

typedef enum {
	JOINT_NORM = 0,
	JOINT_DUPLI_IN = 1,
	JOINT_DUPLI_OUT = 2,
	JOINT_BATCHCALL_IN = 3,
	JOINT_BATCHCALL_OUT = 4,
} graph_node_joint_type_t;

/* node connection */
typedef struct graph_node_joint_s {
	uint32_t joint_type;
	uint32_t from_port_is_virt: 1;
	uint32_t from_port_is_main: 1;
	uint32_t to_port_is_virt: 1;
	uint32_t to_port_is_main: 1;
	uint32_t reserve_0: 28;

	uint32_t from_node_id;
	uint32_t to_node_id;
	char from_port_name[CAVALRY_IO_NAME_MAX];
	char to_port_name[CAVALRY_IO_NAME_MAX];

	uint32_t reserve_1[8];
} graph_node_joint_t;

/* dvi descriptor */
typedef struct dvi_desc_s {
	uint32_t dvi_id;
	uint32_t graph_node_id;
	uint32_t vproc_id;

	uint32_t dvi_ppv: 8;
	uint32_t use_prefer_xfer: 1;
	uint32_t use_orc_prefer_xfer: 1;
	uint32_t reserve_0: 22;
	uint8_t coproc_type;
	uint8_t preferred_dram_xfer_size;
	uint8_t orc_preferred_dram_xfer_size;
	uint8_t reserve_1;
	uint32_t dvi_img_vaddr;
	uint32_t dvi_img_size;
	uint32_t dvi_dag_vaddr;
	uint32_t input_num;
	uint32_t output_num;
	uint32_t ddi_img_size;
	uint32_t dvi_pkg_size;
	char dag_name[CAVALRY_VAR_NAME_MAX];

	uint32_t dvi_runtime_vaddr;
	uint32_t dvi_runtime_size;
	uint32_t dvi_preload_vaddr;
	uint32_t dvi_preload_size;
	uint32_t dvi_dag_size;

	uint32_t dvi_squeeze_size;				/* trim comm weight from dvi_img_size */
	uint16_t dvi_chunk_num;				/* N x dvi_chunk_info_t */
	uint16_t dvi_comm_weight_num;		/* M x comm_weight_info_t */
	uint32_t squeeze_dagbin_offset;		/* dag offset in dvi */

	uint32_t reserve_2[6];
} dvi_desc_t;

/* dvi chunk info */
typedef struct dvi_chunk_info_s {
	uint32_t squeez_offset_in_dvi;
	uint32_t origin_offset_in_dvi;
	uint32_t size;

	uint32_t reserve_0[5];
} dvi_chunk_info_t;

typedef struct comm_weight_info_s {
	uint64_t unique_id;					/* sha256 */
	uint32_t byte_offset_in_dvi;				/* 0xFFFFFFFF in comm.bin */
	uint32_t vmem_byte_size;

	uint32_t reserve_0[6];
} comm_weight_info_t;

/* port descriptor (HMB_input HMB_output) */
typedef struct io_descriptor_s{
	uint64_t port_dim_p;
	uint64_t port_dim_d;
	uint64_t port_dim_h;
	uint64_t port_dim_w;
	uint64_t port_pitch;		/* dpitch_num_bytes */
	uint32_t port_pitch_offset;

	/* bit variable only */
	uint32_t port_drotate: 1;
	uint32_t port_hflip: 1;
	uint32_t port_vflip: 1;
	uint32_t port_dflip: 1;
	uint32_t port_pflip: 1;
	uint32_t port_is_main_io: 1;
	uint32_t port_scalar_init : 1;
	uint32_t port_scalar_variable : 1;
	uint32_t port_dskip_padding: 1;
	uint32_t port_chaining_en: 1;
	uint32_t port_xfer_disable: 1;
	uint32_t port_is_smb: 1;			/* SMB main io */
	uint32_t port_dplane_pitch_en: 1;
	uint32_t reserve_0: 19;
	uint32_t port_scalar_var_default;

	uint8_t port_direction;
	uint8_t port_tile_width;
	uint8_t port_tile_height;
	uint8_t port_dtilemode;

	uint8_t port_dram_format;
	uint8_t port_pitch_bsize;
	uint8_t reserve_2[2];
	uint32_t port_slice_byte_offset;
	char port_slice_parent_name[CAVALRY_IO_PARENT_NAME_MAX];	/* _parent_demangle_name */

	uint64_t port_parent_p;
	uint64_t port_parent_d;
	uint64_t port_parent_h;
	uint64_t port_parent_w;
	uint64_t port_parent_pitch;
	uint64_t port_total_parent_size;

	uint32_t port_drotate_bit_offset;
	uint32_t port_hflip_bit_offset;
	uint32_t port_vflip_bit_offset;
	uint32_t port_dflip_bit_offset;
	uint32_t port_pflip_bit_offset;

	uint8_t port_data_sign;		/*data format: sign, datasize, expoffset, expbits*/
	uint8_t port_data_size;
	int8_t port_data_expoffset;
	uint8_t port_data_expbits;

	/* data format bit variable */
	uint32_t port_data_float_flag: 1;
	uint32_t port_data_bitvector: 1;
	uint32_t port_data_four_bit: 1;
	uint32_t port_data_ten_bit: 1;
	uint32_t port_data_twelve_bit: 1;
	uint32_t port_data_fourteen_bit: 1;
	uint32_t reserve_3: 2;
	uint32_t port_data_bits: 8;
	uint32_t port_dram_bit_pack_mode: 8;
	uint32_t reserve_4: 8;

	uint32_t port_ddi_size;
	uint32_t port_ddi_offset;

	uint32_t port_byte_offset;		/* dbase_byte_offset */
	uint64_t port_size;
	uint64_t port_xfer_size;
	char port_name[CAVALRY_IO_NAME_MAX];  /* HMB name */
	char port_demangled_name[CAVALRY_IO_DEMNGL_NAME_MAX];  /* _cnngen_demangled_name */
	char port_chaining_name[CAVALRY_IO_NAME_MAX];  /* _chaining_name */
	char port_parent_name[CAVALRY_IO_NAME_MAX];  /* _parent_name */

	char port_rand_pair_name[CAVALRY_IO_NAME_MAX];
	int32_t port_rand_seed;

	uint32_t port_batch_size_p;			/* batchcall */
	uint32_t port_batch_size_d;
	uint32_t port_batch_size_h;
	uint32_t port_batch_size_w;

	uint32_t port_desc_byte_offset;	/* _byte_offset */
	uint32_t port_dplane_pitch;
	uint32_t port_dplane_pitch_offset;
	uint8_t port_dplane_pitch_bsize;
	uint8_t reserve_5[3];
	uint32_t reserve_6[11];
} io_descriptor_t;

/* const port */
typedef struct const_port_desc_s {
	char port_name[CAVALRY_IO_NAME_MAX];
	uint64_t bin_size;

	/* append const bin buffer at last */
} const_port_desc_t;

/* io pairs in one loop node */
typedef struct loop_port_pair_s {
	char port_name[CAVALRY_IO_NAME_MAX];
	char src_port_name[CAVALRY_IO_NAME_MAX];
	uint64_t init_vect_size;

	/* append init vector buffer at last */
} loop_port_pair_t;

/* extern library for arm/gpu node */
typedef struct ext_lib_desc_s {
	uint32_t lib_id;
	uint32_t graph_node_id;
	uint32_t input_num;
	uint32_t output_num;
	uint32_t lib_size;				/* lib binary size */
	uint32_t attr_len;
	char lib_name[CAVALRY_VAR_NAME_MAX];
	char ext_type[CAVALRY_VAR_NAME_MAX];

	uint32_t reserve[8];

	/* append extern attribute string at last */
} ext_lib_desc_t;

/* json files */
typedef struct jsonf_desc_s {
	uint32_t file_size;
	uint32_t reserve;
	uint8_t file_buf[0];				/* DON'T add item after file_buf[0] */
} jsonf_desc_t;

/* vproc descriptors */
typedef struct vproc_desc_s {
	uint32_t var_num;
	uint32_t smb_num;
	uint32_t vproc_pkg_size;
	uint32_t reserve[5];
} vproc_desc_t;

/* functional parameters */
typedef struct func_variable_s{
	char var_name[CAVALRY_VPROC_VAR_NAME_MAX];
	uint32_t var_boffset;
	uint32_t var_bsize;
	uint32_t reserve[4];
} func_variable_t;

/* SMB descriptor */
typedef struct smb_descriptor_s{
	char smb_name[CAVALRY_VPROC_SMB_NAME_MAX];
	uint32_t vbase_byte_offset;
	uint32_t data_container_num_bytes;
	uint32_t data_bitvector: 1;
	uint32_t data_four_bit: 1;
	uint32_t data_ten_bit: 1;
	uint32_t data_twelve_bit: 1;
	uint32_t reserve_0: 28;
	uint32_t reserve_1[9];
} smb_descriptor_t;


#ifdef __cplusplus
}
#endif

#endif

