/*
 * virt_query.h
 *
 * Guest Introspection & Topology Query API Subsystem for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_QUERY_H_
#define _VIRT_QUERY_H_

#include <stdint.h>
#include <stddef.h>
#include "amba_virt.h"

int virt_query_handle_req(uint32_t caller_cid,
			  const struct amba_virt_query_req *req,
			  struct amba_virt_query_resp *resp);

#endif /* _VIRT_QUERY_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
