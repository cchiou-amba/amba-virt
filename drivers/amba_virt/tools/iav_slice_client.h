/*
 * iav_slice_client.h
 *
 * Encode and parse the private-IAV slice messages. No physical address.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef IAV_SLICE_CLIENT_H
#define IAV_SLICE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

int iav_slice_client_encode_query(uint8_t *buf, size_t cap, size_t *len);
int iav_slice_client_encode_release(uint8_t *buf, size_t cap, size_t *len, uint32_t window_id);
int iav_slice_client_parse_query(const uint8_t *buf, size_t len, int32_t *status,
				 uint32_t *window_id, uint32_t *usable_length);
int iav_slice_client_parse_release(const uint8_t *buf, size_t len, int32_t *status,
				   uint32_t *lease_retained);

#endif
