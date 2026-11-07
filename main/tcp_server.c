/**
 * @file tcp_server.c
 * @brief Handle main tcp tasks
 * @version 0.1
 * @date 2020-01-22
 *
 * @copyright Copyright (c) 2020
 *
 */
#include <string.h>
#include <stdint.h>
#include <sys/param.h>

#include "main/wifi_configuration.h"
#include "main/usbip_server.h"
#include "main/DAP_handle.h"

#include "components/elaphureLink/elaphureLink_protocol.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

extern TaskHandle_t kDAPTaskHandle;
extern int kRestartDAPHandle;

uint8_t kState = ACCEPTING;
int kSock = -1;

/*
 * 每收到一条 CMD_SUBMIT 就自增。板载状态灯用它判断链路是否真的在传数据：
 * 差值 > 0 就是"正在调试"，短时间内没变化就是"已连接但空闲"。
 */
volatile uint32_t g_usbip_cmd_count = 0;

/* Wi-Fi 断开时置 1：让 TCP 循环把当前客户端关掉，回到 accept。
 * 不做这一步的话，掉线后 kSock 一直 >= 0，状态灯会永远显示"已挂载"。 */
volatile int kForceCloseClient = 0;

/* Wi-Fi 掉线时置 1：让 TCP 任务连监听 socket 一起重建（旧 socket 绑在失效地址上） */
volatile int kRestartServer = 0;

/*
 * USBIP over TCP is a byte stream: one recv() may deliver a partial stage2
 * packet, or several packets at once. The original code handed every recv()
 * straight to emulate(), which breaks as soon as a header and its payload
 * arrive in different segments (or as soon as two packets get coalesced).
 * That silently desynchronises the stream, so commands would disappear and
 * the host would only ever see empty replies.
 *
 * Buffer the stream here and dispatch exactly one complete stage2 packet at
 * a time. A separate copy is used for the self-contained packet because
 * fast_reply() writes its answer right behind the 48 byte header.
 */
#define S2_HEADER_SIZE ((uint32_t)sizeof(usbip_stage2_header))
#define S2_STAGE_SIZE  (4 * 1024)
#define S2_PACKET_MAX  (S2_HEADER_SIZE + 1024)

static uint8_t s2_stage[S2_STAGE_SIZE];
static uint32_t s2_stage_len = 0;
static uint8_t s2_packet[S2_PACKET_MAX];

static void s2_stream_reset(void)
{
    s2_stage_len = 0;
}

static void s2_stream_feed(const uint8_t *data, uint32_t len)
{
    if (len > S2_STAGE_SIZE - s2_stage_len)
    {
        os_printf("s2 stage overflow (%u+%u), resync\r\n",
                  (unsigned)s2_stage_len, (unsigned)len);
        s2_stage_len = 0;
        return;
    }

    memcpy(&s2_stage[s2_stage_len], data, len);
    s2_stage_len += len;

    while (s2_stage_len >= S2_HEADER_SIZE)
    {
        usbip_stage2_header *header = (usbip_stage2_header *)s2_stage;
        uint32_t command = ntohl(header->base.command);
        uint32_t need = S2_HEADER_SIZE;

        if (command == USBIP_STAGE2_REQ_SUBMIT)
        {
            uint32_t direction = ntohl(header->base.direction);
            int32_t data_length = (int32_t)ntohl((uint32_t)header->u.cmd_submit.data_length);
            if (direction == USBIP_DIR_OUT && data_length > 0)
            {
                need += (uint32_t)data_length;
            }
        }

        if (need > S2_PACKET_MAX)
        {
            os_printf("s2 packet too large (%u), resync\r\n", (unsigned)need);
            s2_stage_len = 0;
            return;
        }

        if (s2_stage_len < need)
        {
            break; // wait for the rest of this packet
        }

        memcpy(s2_packet, s2_stage, need);
        emulate(s2_packet, need);

        s2_stage_len -= need;
        if (s2_stage_len > 0)
        {
            memmove(s2_stage, &s2_stage[need], s2_stage_len);
        }
    }
}

void tcp_server_task(void *pvParameters)
{
    uint8_t tcp_rx_buffer[1500];
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    int on = 1;
    while (1)
    {

#ifdef CONFIG_EXAMPLE_IPV4
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
#else // IPV6
        struct sockaddr_in6 destAddr;
        bzero(&destAddr.sin6_addr.un, sizeof(destAddr.sin6_addr.un));
        destAddr.sin6_family = AF_INET6;
        destAddr.sin6_port = htons(PORT);
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
        inet6_ntoa_r(destAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
#endif

        int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (listen_sock < 0)
        {
            /* 不能 break：任务退出后 Wi-Fi 恢复也不会再起来（这正是
             * "ping 通但 3240 没人听" 的根因）。等一会儿重试即可。 */
            os_printf("Unable to create socket: errno %d, retry in 1s\r\n", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        os_printf("Socket created\r\n");

        setsockopt(listen_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
        setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on));

        int err = bind(listen_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0)
        {
            os_printf("Socket unable to bind: errno %d, retry in 1s\r\n", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        os_printf("Socket binded\r\n");

        err = listen(listen_sock, 1);
        if (err != 0)
        {
            os_printf("Error occured during listen: errno %d, retry in 1s\r\n", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        os_printf("Socket listening\r\n");

#ifdef CONFIG_EXAMPLE_IPV6
        struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
#else
        struct sockaddr_in sourceAddr;
#endif
        uint32_t addrLen = sizeof(sourceAddr);
        while (1)
        {
            /* Wi-Fi 掉过线：旧 listen socket 已经绑在失效的地址上，
             * 重建整个监听 socket。 */
            if (kRestartServer) {
                os_printf("wifi down, rebuilding listener\r\n");
                break;
            }

            /* 用 select 等新连接（200ms 超时），不要阻塞在 accept() 里：
             * 否则没有客户端时，Wi-Fi 掉线也永远醒不过来。 */
            fd_set lrfds;
            FD_ZERO(&lrfds);
            FD_SET(listen_sock, &lrfds);
            struct timeval ltv;
            ltv.tv_sec  = 0;
            ltv.tv_usec = 200 * 1000;

            int lready = select(listen_sock + 1, &lrfds, NULL, NULL, &ltv);
            if (lready < 0)
            {
                os_printf("listen select failed: errno %d\r\n", errno);
                break;
            }
            if (lready == 0)
            {
                continue;   // 超时，回去检查标志
            }

            kSock = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
            if (kSock < 0)
            {
                os_printf("Unable to accept connection: errno %d\r\n", errno);
                break;
            }
            setsockopt(kSock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
            setsockopt(kSock, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on));
            os_printf("Socket accepted\r\n");

            /* 新连接建立：清掉上一轮 Wi-Fi 掉线留下的关闭请求，
             * 否则刚接受的这个连接会被立刻踢掉。 */
            kForceCloseClient = 0;

            // 用来检测"连上了但一直不发 attach 请求"的死连接
            TickType_t last_rx = xTaskGetTickCount();

            while (1)
            {
                // Wi-Fi 掉了这条 TCP 已经不可能再工作，主动关掉回到 accept。
                // 不处理的话 kSock 会一直"看起来还连着"，状态灯也一直是绿色。
                if (kForceCloseClient) {
                    kForceCloseClient = 0;
                    os_printf("wifi down, dropping client\r\n");
                    break;
                }
                // Wake up regularly so a parked bulk-IN read can be answered as
                // soon as the DAP thread produced its response, instead of
                // blocking here and deadlocking the host's next transfer.
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(kSock, &readfds);
                struct timeval wait_tv;
                wait_tv.tv_sec = 0;
                wait_tv.tv_usec = 10 * 1000; // 10 ms

                int ready = select(kSock + 1, &readfds, NULL, NULL, &wait_tv);
                if (ready < 0)
                {
                    os_printf("select failed: errno %d\r\n", errno);
                    break;
                }

                dap_service_deferred_read();

                if (ready == 0)
                {
                    // 连接建立了却一直没发 attach 请求：多半是客户端超时后
                    // 留下的半开连接。它会一直占着 accept，导致真正的 attach
                    // 排在 backlog 里等 20 秒才被处理（现象：挂载后灯很久才变绿）。
                    if (kState == ATTACHING &&
                        (xTaskGetTickCount() - last_rx) > pdMS_TO_TICKS(5000)) {
                        os_printf("stale connection (no attach request in 5s), dropping\r\n");
                        break;
                    }
                    continue; // nothing to read, just serviced pending reads
                }

                int len = recv(kSock, tcp_rx_buffer, sizeof(tcp_rx_buffer), 0);
                // Error occured during receiving
                if (len < 0)
                {
                    os_printf("recv failed: errno %d\r\n", errno);
                    break;
                }
                // Connection closed
                else if (len == 0)
                {
                    os_printf("Connection closed\r\n");
                    break;
                }
                // Data received
                else
                {
                    last_rx = xTaskGetTickCount();
                    // #ifdef CONFIG_EXAMPLE_IPV6
                    //                     // Get the sender's ip address as string
                    //                     if (sourceAddr.sin6_family == PF_INET)
                    //                     {
                    //                         inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                    //                     }
                    //                     else if (sourceAddr.sin6_family == PF_INET6)
                    //                     {
                    //                         inet6_ntoa_r(sourceAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
                    //                     }
                    // #else
                    //                     inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                    // #endif

                    switch (kState)
                    {
                    case ACCEPTING:
                        kState = ATTACHING;
                        s2_stream_reset(); // fresh connection, drop stale bytes

                    case ATTACHING:
                        // elaphureLink handshake
                        if (el_handshake_process(kSock, tcp_rx_buffer, len) == 0) {
                            // handshake successed
                            kState = EL_DATA_PHASE;
                            kRestartDAPHandle = DELETE_HANDLE;
                            el_process_buffer_malloc();
                            break;
                        }

                        attach(tcp_rx_buffer, len);
                        break;

                    case EMULATING:
                        s2_stream_feed(tcp_rx_buffer, (uint32_t)len);
                        break;
                    case EL_DATA_PHASE:
                        el_dap_data_process(tcp_rx_buffer, len);
                        break;
                    default:
                        os_printf("unkonw kstate!\r\n");
                    }
                }
            }
            // kState = ACCEPTING;
            if (kSock != -1)
            {
                os_printf("Shutting down socket and restarting...\r\n");
                //shutdown(kSock, 0);
                close(kSock);
                // 必须复位：否则"有没有客户端"的判断（含状态灯）一直是旧的
                kSock  = -1;
                kState = ACCEPTING;

                // Restart DAP Handle
                el_process_buffer_free();
                s2_stream_reset();

                kRestartDAPHandle = RESET_HANDLE;
                if (kDAPTaskHandle)
                    xTaskNotifyGive(kDAPTaskHandle);

                //shutdown(listen_sock, 0);
                //close(listen_sock);
                //vTaskDelay(5);
            }
        }

        /* 离开 accept 循环：必须关掉监听 socket，否则每次重建都泄漏一个 fd
         * （LWIP_MAX_SOCKETS 只有 10 个，泄漏几次就再也建不出来了）。 */
        close(listen_sock);
        listen_sock = -1;

        if (kRestartServer) {
            kRestartServer    = 0;
            kForceCloseClient = 0;
            os_printf("listener will be rebuilt after wifi recovery\r\n");
            vTaskDelay(pdMS_TO_TICKS(500));   // 给 Wi-Fi 一点时间重新拿 IP
        }
    }
    vTaskDelete(NULL);
}
