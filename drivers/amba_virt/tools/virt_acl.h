/*
 * virt_acl.h
 *
 * Access Control List and Policy Engine for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_ACL_H_
#define _VIRT_ACL_H_

#include <stdint.h>
#include <stddef.h>
#include "amba_virt.h"

#define VIRT_ACL_MAX_RULES      16
#define VIRT_ACL_POLICY_FILE    "/persist/etc/amba-virt/policies.json"
#define VIRT_ACL_FALLBACK_FILE  "/etc/amba-virt/policies.json"

#define VIRT_PRIORITY_LOW       0
#define VIRT_PRIORITY_NORMAL    1
#define VIRT_PRIORITY_HIGH      2

struct virt_acl_entry {
	uint32_t cid;
	uint32_t caps;
	uint32_t quota_mb;
	uint32_t priority;
	char tenant_name[64];
	int in_use;
};

int virt_acl_init(void);
void virt_acl_cleanup(void);

int virt_acl_has_cap(uint32_t cid, uint32_t cap);
uint32_t virt_acl_get_caps(uint32_t cid);

int virt_acl_set_rule(uint32_t cid, const char *name, uint32_t caps, uint32_t quota_mb, uint32_t priority);
int virt_acl_get_rule(uint32_t cid, struct virt_acl_entry *out_entry);
int virt_acl_remove_rule(uint32_t cid);

int virt_acl_load_policies(const char *path);
int virt_acl_save_policies(const char *path);

int virt_acl_get_all_entries(struct virt_acl_entry *entries, int max_entries);

#endif /* _VIRT_ACL_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
