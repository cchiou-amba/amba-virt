/*
 * firmware.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef __FIRMWARE_H__
#define __FIRMWARE_H__

/*===========================================================================*/

typedef unsigned char			u8;	/**< UNSIGNED 8-bit data type */
typedef unsigned short			u16;	/**< UNSIGNED 16-bit data type */
typedef unsigned int			u32;	/**< UNSIGNED 32-bit data type */
typedef unsigned long long		u64;	/**< UNSIGNED 64-bit data type */

#define ALIGNUP(x, a)			(((x) + ((a) - 1)) & ~((a) - 1))

/*===========================================================================*/

#define FIRMWARE_MAGIC			0x414d4241 /* 'A' 'M' 'B' 'A' */

#define IMAGE_HEADER_MAGIC		0x616d6261 /* 'a' 'm' 'b' 'a' */
#define IMAGE_HEADER_VER_MAJOR		1
#define IMAGE_HEADER_VER_MINOR		2
#define IMAGE_HEADER_MAX_SIZE		2048

#define IMAGE_FLAG_NONE			0x00
#define IMAGE_FLAG_VERIFY		0x01
#define IMAGE_FLAG_RAW			0x02
#define IMAGE_FLAG_COMPRESSED		0x04
#define IMAGE_FLAG_BACKUP		0x08
#define IMAGE_FLAG_SIGNED		0x10

#define FWBIN_LABEL_LENGTH		16

#ifndef __ASM__

struct fwbin {
	u32 bin_crc32;		/* Binary CRC32 Checksum */
	u32 bin_flag;		/* Binary flag */
	u64 bin_offset;		/* Binary Location offset to header */
	u64 bin_length;		/* Binary length */
	u64 load_addr;		/* Binary Loaded address in memory */
	u64 jump_addr;		/* Binary Entry address in memory */
	char label[FWBIN_LABEL_LENGTH];		/* Binary Label */
	u8 rsvd[128-40-FWBIN_LABEL_LENGTH];	/* Reserved for use in future */
};

struct image_header {
	char name[32];		/* Image name */
	u32 magic;		/* The magic number */
	u32 hdr_length;		/* Header Length */
	u32 hdr_version;	/* Version number */
	u32 build_date;		/* Version date */
	u32 bin_num;		/* Image Binary number */
	u32 flag;		/* Image Flag */
	u64 partition_size; 	/* Image totoal len */
	u8 rsvd[128-64];	/* Reserved for use in future */
	struct fwbin bin[0];	/* Image Binary information */
};

struct firmware_header {
	u32 magic;		/* The magic number */
	u32 num;		/* Image number */
	u8 rsvd[1024-8];	/* Reserved for future */
	u8 part_layout[1024];	/* Partition layout string */
	struct {
		u64 offset;	/* Image Location offset to header */
		u64 length;	/* Image Location offset to header */
	} image[128];
};

/*===========================================================================*/

#define MEMFWPROG_CMD_VERIFY	0x00007E57
#define MEMFWPROG_CMD_MASK	0x0000FFFF

typedef struct fwprog_cmd_s {
	u32	cmd[8];
	u32	error;
	u32	result_data;
	u32	result_str_len;
	u32	result_str_addr;
	u8	data[0x10000 - 12 * sizeof(u32)];
} fwprog_cmd_t;

/*
 * The following data structure is used by the memfwprog program to output
 * the flash programming results to a memory area.
 */
#define FWPROG_RESULT_FLAG_LEN_MASK	0x00ffffff
#define FWPROG_RESULT_FLAG_CODE_MASK	0xff000000

#define FWPROG_RESULT_MAGIC	0xb0329ac3
#define FWPROG_RESULT_MAKE(code, len) \
	((code) << 24) | ((len) & FWPROG_RESULT_FLAG_LEN_MASK)

typedef struct fwprog_result_s {
	u32	magic;
	u32	bad_blk_info;
	u32	flag[128];
	char	*part_name[128];
} fwprog_result_t;

/*===========================================================================*/

#endif  /* !__ASM__ */

#endif
