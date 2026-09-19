/*
 * host_mkimage.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "firmware.h"

#define RED_COLOR   "\033[31m"
#define RESET_COLOR "\033[0m"

extern unsigned int crc32(const void *buf, unsigned int size);

#define IMAGE_MAX_BIN_NUM		128

static u32 build_fw = 0;
static const char *partition_str = "";
static const char *image_name = "";
static const char *image_flag = "";
static const char *dst_file;
static const char *src_file[IMAGE_MAX_BIN_NUM];
static const char *src_label[IMAGE_MAX_BIN_NUM];
static u64 load_addr[IMAGE_MAX_BIN_NUM];
static u64 jump_addr[IMAGE_MAX_BIN_NUM];
static int src_file_num = 0;
static int src_label_num = 0;
static int load_addr_num = 0;
static int jump_addr_num = 0;

static struct option long_options[] = {
	{"help", 0, 0, 'h'},
	{"input", 1, 0, 'i'},
	{"name", 1, 0, 'n'},
	{"flag", 1, 0, 'f'},
	{"label", 1, 0, 's'},
	{"load_addr", 1, 0, 'l'},
	{"jump_addr", 1, 0, 'j'},
	{"build_fw", 0, 0, 0x1001},
	{"partition", 1, 0, 0x1002},
	{0, 0, 0, 0}
};

static const char *short_options = "hi:n:f:l:j:s:";

static void usage(void)
{
	printf("\nUSAGE: host_mkimage [OPTIONS] out_file ...\n");
	printf("\t-h, --help        Help\n");
	printf("\t-b, --build_fw    Create firmware\n");
	printf("\t-p, --partition   Partition layout string\n");
	printf("\t-i, --input       Input file\n");
	printf("\t-n, --name        Image name\n");
	printf("\t-f, --flag        Image flag\n");
	printf("\t-s, --label       Image label\n");
	printf("\t-l, --load_addr   Image load address\n");
	printf("\t-j, --jump_addr   Image jump address\n");
	printf("\n");
}

static int init_param(int argc, char **argv)
{
	int ch, option_index = 0;

	opterr = 0;

	while ((ch = getopt_long(argc, argv,
			short_options, long_options, &option_index)) != -1) {
		switch (ch) {
		case 'h':
			usage();
			exit(0);

		case 'n':
			image_name = optarg;
			break;

		case 'i':
			src_file[src_file_num++] = optarg;
			break;
		case 's':
			src_label[src_label_num++] = optarg;
			break;

		case 'l':
			load_addr[load_addr_num++] = strtoull(optarg, NULL, 0);
			break;

		case 'j':
			jump_addr[jump_addr_num++] = strtoull(optarg, NULL, 0);
			break;

		case 'f':
			image_flag = optarg;
			break;

		case 0x1001:
			build_fw = 1;
			break;

		case 0x1002:
			partition_str = optarg;
			break;

		default:
			fprintf(stderr, RED_COLOR"unknown option found: %c\n"RESET_COLOR, ch);
			return -1;
		}
	}

	dst_file = argv[optind];

	return 0;
}

static u32 get_build_time(void)
{
	time_t t;
	struct tm *lt;

	time(&t);
	lt = localtime(&t);

	return (lt->tm_year + 1900) << 16 | (lt->tm_mon + 1) << 8 | lt->tm_mday;
}

static u32 parse_flag(const char *str)
{
	u32 flag = 0;

	if (strstr(str, "verify"))
		flag |= IMAGE_FLAG_VERIFY;
	if (strstr(str, "raw"))
		flag |= IMAGE_FLAG_RAW;
	if (strstr(str, "backup"))
		flag |= IMAGE_FLAG_BACKUP;
	if (strstr(str, "compressed"))
		flag |= IMAGE_FLAG_COMPRESSED;
	if (strstr(str, "signed"))
		flag |= IMAGE_FLAG_SIGNED;

	if (flag & IMAGE_FLAG_RAW) {
		if (flag & (IMAGE_FLAG_VERIFY | IMAGE_FLAG_COMPRESSED)) {
			fprintf(stderr, RED_COLOR"Bad combination raw with");
			if (flag & IMAGE_FLAG_VERIFY)
				fprintf(stderr, " %s ", "verify");
			if (flag & IMAGE_FLAG_COMPRESSED)
				fprintf(stderr, " %s ", "compressed");
			fprintf(stderr, "\n Check IMAGE_n_FLAG in the profile\n");
			fprintf(stderr, RESET_COLOR "\n");
			exit(1);
		}
	}

	return flag;
}

static int create_image(void)
{
	struct image_header *img_hdr;
	u32 i, hdr_length, appendsz;
	FILE *dst_fp;
	int rval = 0;

	/* Sanity check */
	if ((sizeof(*img_hdr) % 32) != 0) {
		fprintf(stderr, "Invalid header size: %ld!\n", sizeof(*img_hdr));
		return -1;
	}

	hdr_length = sizeof(*img_hdr) + sizeof(struct fwbin) * src_file_num;
	assert(!(hdr_length & 7));	/* Notice: must align to 8 bytes */

	img_hdr = malloc(hdr_length);
	if (img_hdr == NULL) {
		fprintf(stderr, "Out of memory!\n");
		return -1;
	}

	/* initialize image header */
	memset(img_hdr, 0, sizeof(struct image_header));

	img_hdr->flag = parse_flag(image_flag);

	/* if this image has labels,
	 * then the number of files and labels must match;
	 * otherwise, set all labels to "default"
	 */
	if (src_label_num  > 0) {
		if (src_file_num != src_label_num) {
			fprintf(stderr,
				RED_COLOR"Error: The number of files and labels "
				"must match for image %s, pls check the profile\n"RESET_COLOR, image_name);
			rval = -1;
			goto exit;
		}
	} else {
		for (i = 0; i < src_file_num; i++)
			src_label[i] = "default";
	}

	/*
	 * Parse particular flag for bin file, it has higher priority than
	 * the flag specified in IMAGE_n_FLAG.
	 */
	for (i = 0; i < src_file_num; i++) {
		char *str = strchr(src_file[i], ':');
		if (str != NULL) {
			*str++ = '\0';
			img_hdr->bin[i].bin_flag = parse_flag(str);
		}
	}

	strncpy(img_hdr->name, image_name, sizeof(img_hdr->name) - 1);
	img_hdr->magic = IMAGE_HEADER_MAGIC;
	img_hdr->hdr_length = hdr_length;
	img_hdr->hdr_version = IMAGE_HEADER_VER_MAJOR << 16 | IMAGE_HEADER_VER_MINOR;
	img_hdr->build_date = get_build_time();
	img_hdr->bin_num = src_file_num;

	dst_fp = fopen(dst_file, "wb");
	if (dst_fp == NULL) {
		fprintf(stderr, "Can't open %s: %s\n", dst_file, strerror(errno));
		return -1;
	}

	if (fseek(dst_fp, hdr_length, SEEK_SET) < 0) {
		fprintf(stderr, "fseek error %s: %s\n", dst_file, strerror(errno));
		rval = -1;
		goto exit;
	}

	for (i = 0; i < src_file_num; i++) {
		struct stat statbuf;
		int fd;
		u8 *ptr;

		if (src_file[i] == NULL || src_file[i][0] == '\0') {
			fprintf(stderr, "Invalid file name for image %s\n", image_name);
			rval = -1;
			goto exit;
		}

		if (src_label[i] == NULL || src_label[i][0] == '\0') {
			fprintf(stderr, "Invalid label for image %s\n", image_name);
			rval = -1;
			goto exit;
		}

		if (strlen(src_label[i]) > (FWBIN_LABEL_LENGTH - 1)) {
			fprintf(stderr, RED_COLOR"Label %s is too long(maxlength:%d) for image %s\n"RESET_COLOR,
				src_label[i], (FWBIN_LABEL_LENGTH - 1), image_name);
			rval = -1;
			goto exit;
		}

		fd = open(src_file[i], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Can't open %s: %s\n", src_file[i], strerror(errno));
			rval = -1;
			goto exit;
		}

		if (fstat(fd, &statbuf) < 0) {
			fprintf(stderr, "fstat error %s: %s\n", src_file[i], strerror(errno));
			rval = -1;
			goto exit;
		}

		ptr = mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
		if (ptr == MAP_FAILED) {
			fprintf(stderr, "Can't read %s\n", src_file[i]);
			rval = -1;
			goto exit;
		}

		/* bin_offset align to 8 bytes */
		img_hdr->bin[i].bin_crc32 = crc32(ptr, statbuf.st_size);
		img_hdr->bin[i].bin_offset = ALIGNUP(ftell(dst_fp), 8);
		img_hdr->bin[i].bin_length = statbuf.st_size;
		memset(img_hdr->bin[i].label, 0, sizeof(img_hdr->bin[i].label));
		strncpy(img_hdr->bin[i].label, src_label[i], sizeof(img_hdr->bin[i].label) - 1);

		if (fseek(dst_fp, img_hdr->bin[i].bin_offset, SEEK_SET) < 0) {
			fprintf(stderr, "fseek error %s: %s\n", dst_file, strerror(errno));
			rval = -1;
			goto exit;
		}

		if (fwrite(ptr, 1, statbuf.st_size, dst_fp) != statbuf.st_size) {
			fprintf(stderr, "Write error on %s: %s\n", dst_file, strerror(errno));
			munmap(ptr, statbuf.st_size);
			rval = -1;
			goto exit;
		}

		munmap(ptr, statbuf.st_size);
		close(fd);

		/* use the first load_addr/jump_addr if it's not specified for this binary */
		img_hdr->bin[i].load_addr = (i >= load_addr_num) ? load_addr[0] : load_addr[i];
		img_hdr->bin[i].jump_addr = (i >= jump_addr_num) ? jump_addr[0] : jump_addr[i];
	}

	/* image size align to 8 bytes */
	img_hdr->partition_size = ALIGNUP(ftell(dst_fp), 8);
	appendsz = img_hdr->partition_size - ftell(dst_fp);
	if (appendsz) {	/* dummy data */
		if (fwrite(img_hdr, 1, appendsz, dst_fp) != appendsz) {
			fprintf(stderr, "Append File error on %s: %s\n", dst_file, strerror(errno));
			return -1;
		}
	}

	if (fseek(dst_fp, 0, SEEK_SET) < 0) {
		fprintf(stderr, "fseek error %s: %s\n", dst_file, strerror(errno));
		return -1;
	}

	if (fwrite(img_hdr, 1, hdr_length, dst_fp) != hdr_length) {
		fprintf(stderr, "Write error on %s: %s\n", dst_file, strerror(errno));
		return -1;
	}

exit:
	fclose(dst_fp);
	return rval;
}

static int create_firmware(void)
{
	struct firmware_header fw_hdr;
	FILE *dst_fp;
	int i, rval = 0;

	/* Sanity check */
	if ((sizeof(fw_hdr) % 32) != 0) {
		fprintf(stderr, "Invalid header size: %ld!\n", sizeof(fw_hdr));
		return -1;
	}

	/* initialize image header */
	memset(&fw_hdr, 0, sizeof(fw_hdr));

	fw_hdr.magic = FIRMWARE_MAGIC;
	fw_hdr.num = src_file_num;
	strncpy(fw_hdr.part_layout, partition_str, sizeof(fw_hdr.part_layout) - 1);

	dst_fp = fopen(dst_file, "wb");
	if (dst_fp == NULL) {
		fprintf(stderr, "Can't open %s: %s\n", dst_file, strerror(errno));
		return -1;
	}

	if (fseek(dst_fp, sizeof(fw_hdr), SEEK_SET) < 0) {
		fprintf(stderr, "fseek error %s: %s\n", dst_file, strerror(errno));
		rval = -1;
		goto exit;
	}

	for (i = 0; i < src_file_num; i++) {
		struct stat statbuf;
		int fd;
		u8 *ptr;

		if (src_file[i] == NULL || src_file[i][0] == '\0') {
			fprintf(stderr, "Invalid file name: %s\n", src_file[i]);
			rval = -1;
			goto exit;
		}

		fd = open(src_file[i], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Can't open %s: %s\n", src_file[i], strerror(errno));
			rval = -1;
			goto exit;
		}

		if (fstat(fd, &statbuf) < 0) {
			fprintf(stderr, "fstat error %s: %s\n", src_file[i], strerror(errno));
			rval = -1;
			goto exit;
		}

		ptr = mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
		if (ptr == MAP_FAILED) {
			fprintf(stderr, "Can't read %s\n", src_file[i]);
			rval = -1;
			goto exit;
		}

		/* image.offset align to 8 bytes */
		fw_hdr.image[i].offset = ALIGNUP(ftell(dst_fp), 8);
		fw_hdr.image[i].length = statbuf.st_size;

		if (fseek(dst_fp, fw_hdr.image[i].offset, SEEK_SET) < 0) {
			fprintf(stderr, "fseek error %s: %s\n", dst_file, strerror(errno));
			rval = -1;
			goto exit;
		}

		if (fwrite(ptr, 1, statbuf.st_size, dst_fp) != statbuf.st_size) {
			fprintf(stderr, "Write error on %s: %s\n", dst_file, strerror(errno));
			munmap(ptr, statbuf.st_size);
			rval = -1;
			goto exit;
		}

		munmap(ptr, statbuf.st_size);
		close(fd);
	}

	if (fseek(dst_fp, 0, SEEK_SET) < 0) {
		fprintf(stderr, "fseek error %s: %s\n", dst_file, strerror(errno));
		return -1;
	}

	if (fwrite(&fw_hdr, 1, sizeof(fw_hdr), dst_fp) != sizeof(fw_hdr)) {
		fprintf(stderr, "Write error on %s: %s\n", dst_file, strerror(errno));
		return -1;
	}

exit:
	fclose(dst_fp);
	return rval;
}

int main(int argc, char **argv)
{
	if (init_param(argc, argv) < 0) {
		usage();
		return -1;
	}

	if (build_fw)
		create_firmware();
	else
		create_image();

	return 0;
}
