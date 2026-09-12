/*
 * amba_virt_test.h
 *
 * Test and benchmark protocol definitions for amba-virt validation suite.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _UAPI_AMBA_VIRT_TEST_H
#define _UAPI_AMBA_VIRT_TEST_H

#include "amba_virt.h"



/* Extended test message types */
#define AMBA_VIRT_MSG_ECHO_REQ          5u
#define AMBA_VIRT_MSG_ECHO_RESP         6u
#define AMBA_VIRT_MSG_BENCH_BURST       7u
#define AMBA_VIRT_MSG_BENCH_ACK         8u
#define AMBA_VIRT_MSG_CAVALRY_MOCK_REQ  9u
#define AMBA_VIRT_MSG_CAVALRY_MOCK_RESP 10u

/* Benchmark magic identifier: 'AMBA' */
#define AMBA_VIRT_BENCH_MAGIC           0x414D4241u

/* Benchmark packet header embedded in xfer.data */
struct amba_virt_bench_hdr {
	__u32 magic;
	__u32 seq;
	__u64 send_time_ns;
	__u32 payload_len;
	__u32 flags;
	__u8  payload[];
};

/* Flags for benchmark transfers */
#define AMBA_VIRT_BENCH_FLAG_WARMUP     (1u << 0)
#define AMBA_VIRT_BENCH_FLAG_LAST       (1u << 1)
#define AMBA_VIRT_BENCH_FLAG_VERIFY     (1u << 2)

/* Simulated Cavalry job packet */
struct amba_virt_cavalry_mock {
	__u32 session_id;
	__u32 job_id;
	__u32 input_offset;
	__u32 input_size;
	__u32 output_offset;
	__u32 output_size;
	__u32 execution_delay_us; /* simulated hardware processing delay */
	__u32 status;
};

/* PRBS31 polynomial for pattern verification */
#define PRBS31_POLY                     0x10000001u

static inline __u32 amba_virt_prbs31_next(__u32 state)
{
	__u32 new_bit = ((state >> 30) ^ (state >> 27)) & 1u;
	return ((state << 1) | new_bit) & 0x7FFFFFFFu;
}

#endif /* _UAPI_AMBA_VIRT_TEST_H */
