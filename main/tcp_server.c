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
            os_printf("Unable to create socket: errno %d\r\n", errno);
            break;
        }
        os_printf("Socket created\r\n");

        setsockopt(listen_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
        setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on));

        int err = bind(listen_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0)
        {
            os_printf("Socket unable to bind: errno %d\r\n", errno);
            break;
        }
        os_printf("Socket binded\r\n");

        err = listen(listen_sock, 1);
        if (err != 0)
        {
            os_printf("Error occured during listen: errno %d\r\n", errno);
            break;
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
            kSock = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
            if (kSock < 0)
            {
                os_printf("Unable to accept connection: errno %d\r\n", errno);
                break;
            }
            setsockopt(kSock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
            setsockopt(kSock, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on));
            os_printf("Socket accepted\r\n");

            while (1)
            {
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
                if (kState == EMULATING || kState == EL_DATA_PHASE)
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
    }
    vTaskDelete(NULL);
}