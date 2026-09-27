#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "amba_virt_qnx.h"

int main(void) {
    uint64_t phys = 0;
    void *virt = NULL;
    size_t size = 0;
    printf("[1] Calling amba_virt_get_window...\n");
    int ret = amba_virt_get_window(&phys, &virt, &size);
    printf("    ret=%d, phys=0x%llx, virt=%p, size=%zu MB\n",
           ret, (unsigned long long)phys, virt, size / (1024*1024));

    printf("[2] Sending PING RPC...\n");
    struct amba_virt_msg req, resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.type = AMBA_VIRT_MSG_PING;
    req.seq = 100;
    uint32_t resp_len = sizeof(resp);
    ret = amba_virt_rpc(&req, sizeof(req), &resp, &resp_len, 5000);
    printf("    ret=%d, resp.type=%u (expected PONG=%u), resp.seq=%u\n",
           ret, resp.type, (unsigned)AMBA_VIRT_MSG_PONG, resp.seq);

    printf("[3] Testing DMA request (UART3 TX 64 bytes)...\n");
    uint8_t req_buf[sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_dma_request)];
    uint8_t resp_buf[sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_dma_response)];
    struct amba_virt_msg *m_req = (struct amba_virt_msg *)req_buf;
    struct amba_virt_dma_request *d_req = (struct amba_virt_dma_request *)(req_buf + sizeof(*m_req));
    struct amba_virt_msg *m_resp = (struct amba_virt_msg *)resp_buf;
    struct amba_virt_dma_response *d_resp = (struct amba_virt_dma_response *)(resp_buf + sizeof(*m_resp));
    resp_len = sizeof(resp_buf);

    memset(req_buf, 0, sizeof(req_buf));
    m_req->type = AMBA_VIRT_MSG_DMA_REQUEST_REQ;
    m_req->seq = 101;
    d_req->cookie = 1;
    d_req->endpoint_id = AMBA_DMA_ENDPOINT_UART3;
    d_req->operation = AMBA_DMA_OP_MEM_TO_DEV;
    d_req->offset = 0x00100000;
    d_req->length = 64;

    ret = amba_virt_rpc(req_buf, sizeof(req_buf), resp_buf, &resp_len, 5000);
    printf("    ret=%d, m_resp.type=%u, d_resp->status=%d, d_resp->transferred=%u\n",
           ret, m_resp->type, d_resp->status, d_resp->transferred);

    return 0;
}
