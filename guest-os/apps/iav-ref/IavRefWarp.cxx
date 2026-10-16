/*
 * IavRefWarp.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, warp class (plan_iav_usb_yolo Envelope F): the warp configuration calls with the bare-metal
 * pattern. QUERY_MEM_LAYOUT_INFO gives the slot of channel 0 in the user part of IAV_PART_WARP; the application maps
 * the partition at the address QUERY_MEMBLOCK reports, writes an identity table (all vectors zero) into area 0 of that
 * slot, and configures it with CFG_WARP_CTRL (MAIN) and APPLY_WARP_CTRL; then the DPTZ and maximum-output classes. Each
 * call's return code and errno is a row: a pipeline whose channel has no lens warp answers the same refusal natively and
 * in a guest, which is what this application compares (the picture is not measured). The slot is restored.
 *
 *   IavRefWarp [--channel <id>]
 */

#include "IavRefCommon.hxx"

using namespace iavref;

int main(int argc, char **argv)
{
    const unsigned int chan = (unsigned int)argValue(argc, argv, "--channel", 0);
    Device dev;
    unsigned long addr = 0, len = 0;
    struct iav_mem_layout_info *lay = (struct iav_mem_layout_info *)calloc(1, sizeof(*lay));
    struct iav_warp_ctrl *w = (struct iav_warp_ctrl *)calloc(1, sizeof(*w));
    struct iav_apply_flag a;
    long long seq = 0;
    int rc, err, cid;

    if (!dev.open() || lay == NULL || w == NULL || dev.partition(IAV_PART_WARP, &addr, &len) != 0 || len == 0) {
        fprintf(stderr, "no IAV_PART_WARP\n");
        return 1;
    }
    rc = dev.call(IAV_IOC_QUERY_MEM_LAYOUT_INFO, lay, "IAV_IOC_QUERY_MEM_LAYOUT_INFO", true);
    callRow("call", seq++, "query_mem_layout_info", rc, errno);
    if (rc != 0 || chan >= IAV_MAX_CHANNEL_NUM) {
        return 1;
    }
    const struct iav_mem_layout &slot = lay->warp_layout.buf_layout[chan];
    Row("layout").add("total_user_size", (long long)lay->warp_layout.total_user_size).add("slot_offset", (long long)slot.offset)
        .add("slot_size", (long long)slot.size).add("unit_size", (long long)slot.unit_size)
        .add("unit_num", (long long)slot.unit_num).add("partition_length", (long long)len).print();

    /* without a mapping (a guest whose aperture does not carry WARP) the calls still run: they are what is compared */
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)addr);
    const int mapErr = p == MAP_FAILED ? errno : 0;
    size_t tbl = slot.unit_size ? slot.unit_size : 4096;
    unsigned char *saved = NULL;

    Row("map").add("mapped", (long long)(p != MAP_FAILED)).add("errno", (long long)mapErr).print();
    if (p != MAP_FAILED && slot.offset + tbl <= len) {
        saved = (unsigned char *)malloc(tbl);
        unsigned char *zero = (unsigned char *)calloc(1, tbl);

        windowCopy(saved, (const volatile unsigned char *)p + slot.offset, tbl);
        windowWrite((volatile unsigned char *)p + slot.offset, zero, tbl);
        free(zero);
    }

    /* the stored configuration of the channel, then MAIN with area 0 over the whole input, identity vectors */
    memset(w, 0, sizeof(*w));
    w->channel_id = (u16)chan;
    rc = dev.call(IAV_IOC_GET_WARP_CTRL, w, "IAV_IOC_GET_WARP_CTRL", true);
    err = errno;
    callRow("call", seq++, "get_warp_ctrl_main", rc, err);
    cid = IAV_WARP_CTRL_MAIN;
    memcpy(&w->cid, &cid, sizeof(cid));
    w->channel_id = (u16)chan;
    w->warp_area_behavior = 0;
    w->arg.main.area[0].enable = 1;
    w->arg.main.area[0].h_map.enable = 1;
    w->arg.main.area[0].v_map.enable = 1;
    w->arg.main.area[0].h_map.data_addr_offset = slot.offset;
    w->arg.main.area[0].v_map.data_addr_offset = slot.offset;
    rc = dev.call(IAV_IOC_CFG_WARP_CTRL, w, "IAV_IOC_CFG_WARP_CTRL main", true);
    err = errno;
    callRow("call", seq++, "cfg_warp_ctrl_main", rc, err);
    memset(&a, 0, sizeof(a));
    a.apply = IAV_WARP_CTRL_MAIN;
    a.param = 1u << chan;
    rc = dev.call(IAV_IOC_APPLY_WARP_CTRL, &a, "IAV_IOC_APPLY_WARP_CTRL main", true);
    err = errno;
    callRow("call", seq++, "apply_warp_ctrl_main", rc, err);

    memset(w, 0, sizeof(*w));
    cid = IAV_WARP_CTRL_DPTZ;
    memcpy(&w->cid, &cid, sizeof(cid));
    w->channel_id = (u16)chan;
    w->arg.dptz.buf_id = IAV_SRCBUF_PC;
    rc = dev.call(IAV_IOC_GET_WARP_CTRL, w, "IAV_IOC_GET_WARP_CTRL dptz", true);
    err = errno;
    callRow("call", seq++, "get_warp_ctrl_dptz", rc, err);
    rc = dev.call(IAV_IOC_CFG_WARP_CTRL, w, "IAV_IOC_CFG_WARP_CTRL dptz", true);
    err = errno;
    callRow("call", seq++, "cfg_warp_ctrl_dptz", rc, err);

    memset(w, 0, sizeof(*w));
    cid = IAV_WARP_CTRL_MAX_CFG;
    memcpy(&w->cid, &cid, sizeof(cid));
    w->channel_id = (u16)chan;
    w->arg.max_cfg.max_warp_output_width[0] = 1920;
    rc = dev.call(IAV_IOC_CFG_WARP_CTRL, w, "IAV_IOC_CFG_WARP_CTRL max", true);
    err = errno;
    callRow("call", seq++, "cfg_warp_ctrl_max_cfg", rc, err);

    if (saved) {
        windowWrite((volatile unsigned char *)p + slot.offset, saved, tbl);
        free(saved);
    }
    if (p != MAP_FAILED) {
        munmap(p, len);
    }
    free(lay);
    free(w);
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
