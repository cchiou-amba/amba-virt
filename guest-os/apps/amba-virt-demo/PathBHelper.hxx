/*
 * PathBHelper.hxx
 *
 * Ambarella Path B HVM Execution Context Helper (C++ inline)
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_PATH_B_HELPER_HXX
#define AMBA_VIRT_PATH_B_HELPER_HXX

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#pragma push_macro("new")
#undef new
#define new _sdk_new
extern "C" {
#include <nnctrl.h>
#include "nnctrl_priv.h"
struct net_desc *get_net_desc(struct nnctrl_info *pctl, int net_id);
}
#pragma pop_macro("new")

struct path_b_info {
    bool is_hvm;
    uint32_t dag_id;
    uint32_t in_handle_id;
    uint32_t out_handle_id;
    void *out_virt_addr;
};

static inline int get_path_b_info(int net_id, struct path_b_info *info)
{
    if (!info) return -1;
    memset(info, 0, sizeof(*info));

    struct nnctrl_info *pctl = get_nnctrl_global_context();
    if (!pctl) return -1;

    struct net_desc *pnet = get_net_desc(pctl, net_id);
    if (!pnet) return -1;

    info->is_hvm = pnet->path_b.is_hvm;
    info->dag_id = pnet->path_b.dag_id;
    info->in_handle_id = pnet->path_b.in_ports[0].handle_id;
    info->out_handle_id = pnet->path_b.out_ports[0].handle_id;
    info->out_virt_addr = pnet->path_b.out_ports[0].virt_addr;

    return 0;
}

#endif /* AMBA_VIRT_PATH_B_HELPER_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
