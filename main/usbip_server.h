#ifndef __USBIP_SERVER_H__
#define __USBIP_SERVER_H__
#include <stdint.h>
#include <stddef.h>

#include "components/USBIP/usbip_defs.h"

enum state_t
{
    ACCEPTING,
    ATTACHING,
    EMULATING,
    EL_DATA_PHASE
};
extern uint8_t kState;
extern int kSock;

/* 累计收到的 CMD_SUBMIT 数量，供板载状态灯判断链路是否在传数据 */
extern volatile uint32_t g_usbip_cmd_count;
/* Wi-Fi 断开时置 1，TCP 循环据此丢弃当前客户端连接 */
extern volatile int kForceCloseClient;
/* Wi-Fi 掉线时置 1，TCP 任务据此重建监听 socket */
extern volatile int kRestartServer;

int attach(uint8_t *buffer, uint32_t length);
int emulate(uint8_t *buffer, uint32_t length);
void send_stage2_submit_data(usbip_stage2_header *req_header, int32_t status, const void * const data, int32_t data_length);
void send_stage2_submit(usbip_stage2_header *req_header, int32_t status, int32_t data_length);
void send_stage2_submit_data_fast(usbip_stage2_header *req_header, const void *const data, int32_t data_length);
// Reply to a stage2 SUBMIT whose header was captured in wire (network) order,
// e.g. a bulk-IN URB that was parked because its answer was not ready yet.
void send_stage2_submit_deferred(const uint8_t *raw_header, const void *data, int32_t data_length);
int usbip_network_send(int s, const void *dataptr, size_t size, int flags);

#endif