/**
 * @file DAP_handle.c
 * @brief Handle DAP packets and transaction push
 * @version 0.5
 * @change: 2020.02.04 first version
 *          2020.11.11 support WinUSB mode
 *          2021.02.17 support SWO
 *          2021.10.03 try to handle unlink behavior
 *
 * @copyright Copyright (c) 2021
 *
 */

#include <stdint.h>
#include <string.h>

#include "main/usbip_server.h"
#include "main/DAP_handle.h"
#include "main/dap_configuration.h"
#include "main/wifi_configuration.h"

#include "components/USBIP/usb_descriptor.h"
#include "components/DAP/include/DAP.h"
#include "components/DAP/include/swo.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#if ((USE_MDNS == 1) || (USE_OTA == 1))
    #define DAP_BUFFER_NUM 10
#else
    #define DAP_BUFFER_NUM 20
#endif

#if (USE_WINUSB == 1)
typedef struct
{
    uint32_t length;
    uint8_t buf[DAP_PACKET_SIZE];
} DapPacket_t;
#else
typedef struct
{
    uint8_t buf[DAP_PACKET_SIZE];
} DapPacket_t;
#endif

#define DAP_HANDLE_SIZE (sizeof(DapPacket_t))


extern int kSock;
extern TaskHandle_t kDAPTaskHandle;

int kRestartDAPHandle = NO_SIGNAL;


static DapPacket_t DAPDataProcessed;
static int dap_respond = 0;

// SWO Trace
static uint8_t *swo_data_to_send = NULL;
static uint32_t swo_data_num;

// DAP handle
static RingbufHandle_t dap_dataIN_handle = NULL;
static RingbufHandle_t dap_dataOUT_handle = NULL;
static SemaphoreHandle_t data_response_mux = NULL;

// Signalled by the DAP thread every time a response is queued. The host
// pipelines USB transfers: its bulk-IN read usually arrives before the DAP
// thread has produced the answer, so the read side must wait for this
// instead of immediately completing the URB with zero bytes.
static SemaphoreHandle_t dap_response_ready = NULL;
#define DAP_RESPONSE_TIMEOUT_MS 100
#define DAP_DEFERRED_READ_MAX 8

// Parked bulk-IN URBs (defined further below). fast_reply() needs the depth to
// keep response pairing strictly FIFO.
static int deferred_read_count;


void malloc_dap_ringbuf() {
    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
    {
        if (dap_dataIN_handle == NULL) {
            dap_dataIN_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
        }
        if (dap_dataOUT_handle == NULL) {
            dap_dataOUT_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
        }

        xSemaphoreGive(data_response_mux);
    }
}

void free_dap_ringbuf() {
    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
        if (dap_dataIN_handle) {
            vRingbufferDelete(dap_dataIN_handle);
        }
        if (dap_dataOUT_handle) {
            vRingbufferDelete(dap_dataOUT_handle);
        }

        dap_dataIN_handle = dap_dataOUT_handle = NULL;
        xSemaphoreGive(data_response_mux);
    }

}


void handle_dap_data_request(usbip_stage2_header *header, uint32_t length)
{
    uint8_t *data_in = (uint8_t *)header;
    data_in = &(data_in[sizeof(usbip_stage2_header)]);
    // Point to the beginning of the URB packet

#if (USE_WINUSB == 1)
    // Report the real number of bytes the host wrote; a zero actual_length
    // makes the host keep the bulk OUT URB pending until it times out.
    send_stage2_submit(header, 0, (int32_t)(length - sizeof(usbip_stage2_header)));

    // always send constant size buf -> cuz we don't care about the IN packet size
    // and to unify the style, we set aside the length of the section
    xRingbufferSend(dap_dataIN_handle, data_in - sizeof(uint32_t), DAP_HANDLE_SIZE, portMAX_DELAY);
    xTaskNotifyGive(kDAPTaskHandle);

#else
    send_stage2_submit(header, 0, 0);

    xRingbufferSend(dap_dataIN_handle, data_in, DAP_HANDLE_SIZE, portMAX_DELAY);
    xTaskNotifyGive(kDAPTaskHandle);

#endif

    // dap_respond = DAP_ProcessCommand((uint8_t *)data_in, (uint8_t *)data_out);
    // //handle_dap_data_response(header);
    // send_stage2_submit(header, 0, 0);
}

void handle_dap_data_response(usbip_stage2_header *header)
{
    return;
    // int resLength = dap_respond & 0xFFFF;
    // if (resLength)
    // {

    //     send_stage2_submit_data(header, 0, (void *)DAPDataProcessed.buf, resLength);
    //     dap_respond = 0;
    // }
    // else
    // {
    //     send_stage2_submit(header, 0, 0);
    // }
}

void handle_swo_trace_response(usbip_stage2_header *header)
{
#if (SWO_FUNCTION_ENABLE == 1)
    if (kSwoTransferBusy)
    {
        // busy indicates that there is data to be send
        os_printf("swo use data\r\n");
        send_stage2_submit_data(header, 0, (void *)swo_data_to_send, swo_data_num);
        SWO_TransferComplete();
    }
    else
    {
        // nothing to send.
        send_stage2_submit(header, 0, 0);
    }
#else
    send_stage2_submit(header, 0, 0);
#endif
}

// SWO Data Queue Transfer
//   buf:    pointer to buffer with data
//   num:    number of bytes to transfer
void SWO_QueueTransfer(uint8_t *buf, uint32_t num)
{
    swo_data_to_send = buf;
    swo_data_num = num;
}

void DAP_Thread(void *argument)
{
    dap_dataIN_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
    dap_dataOUT_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
    data_response_mux = xSemaphoreCreateMutex();
    dap_response_ready = xSemaphoreCreateBinary();
    size_t packetSize;
    int resLength;
    DapPacket_t *item;

    if (dap_dataIN_handle == NULL || dap_dataIN_handle == NULL ||
        data_response_mux == NULL)
    {
        os_printf("Can not create DAP ringbuf/mux!\r\n");
        vTaskDelete(NULL);
    }
    for (;;)
    {

        while (1)
        {
            if (kRestartDAPHandle)
            {
                free_dap_ringbuf();

                if (kRestartDAPHandle == RESET_HANDLE) {
                    malloc_dap_ringbuf();
                    if (dap_dataIN_handle == NULL || dap_dataIN_handle == NULL)
                    {
                        os_printf("Can not create DAP ringbuf/mux!\r\n");
                        vTaskDelete(NULL);
                    }
                }

                kRestartDAPHandle = NO_SIGNAL;
            }

            ulTaskNotifyTake(pdFALSE, portMAX_DELAY); // wait event


            if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL) {
                continue; // may be use elaphureLink, wait...
            }


            packetSize = 0;
            item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataIN_handle, &packetSize,
                                                         pdMS_TO_TICKS(1), DAP_HANDLE_SIZE);
            if (packetSize == 0)
            {
                break;
            }

            else if (packetSize < DAP_HANDLE_SIZE)
            {
                os_printf("Wrong data in packet size:%d , data in remain: %d\r\n", packetSize, (int)xRingbufferGetMaxItemSize(dap_dataIN_handle));
                vRingbufferReturnItem(dap_dataIN_handle, (void *)item);
                break;
                // This may not happen because there is a semaphore acquisition
            }

            if (item->buf[0] == ID_DAP_QueueCommands)
            {
                item->buf[0] = ID_DAP_ExecuteCommands;
            }

            resLength = DAP_ProcessCommand((uint8_t *)item->buf, (uint8_t *)DAPDataProcessed.buf); // use first 4 byte to save length
            resLength &= 0xFFFF;                                                                   // res length in lower 16 bits

            vRingbufferReturnItem(dap_dataIN_handle, (void *)item); // process done.

            // now prepare to reply
#if (USE_WINUSB == 1)
            DAPDataProcessed.length = resLength;
#endif
            xRingbufferSend(dap_dataOUT_handle, (void *)&DAPDataProcessed, DAP_HANDLE_SIZE, portMAX_DELAY);

            if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
            {
                ++dap_respond;
                xSemaphoreGive(data_response_mux);
            }

            if (dap_response_ready)
            {
                xSemaphoreGive(dap_response_ready);
            }
        }
    }
}

int fast_reply(uint8_t *buf, uint32_t length)
{
    usbip_stage2_header *buf_header = (usbip_stage2_header *)buf;
    if (length == 48 &&
        buf_header->base.command == PP_HTONL(USBIP_STAGE2_REQ_SUBMIT) &&
        buf_header->base.direction == PP_HTONL(USBIP_DIR_IN) &&
        buf_header->base.ep == PP_HTONL(1))
    {
        // Strict FIFO: earlier reads must be answered before this one, otherwise
        // the host pairs every response with the wrong command ("command
        // mismatch. Sent 0x1 received 0x0").
        if (deferred_read_count == 0 && dap_respond > 0)
        {
            DapPacket_t *item;
            size_t packetSize = 0;
            item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataOUT_handle, &packetSize,
                                                         pdMS_TO_TICKS(10), DAP_HANDLE_SIZE);
            if (packetSize == DAP_HANDLE_SIZE)
            {
                // fast_reply() is called before read_stage2_command(), so the
                // header here is still in wire order. Always go through the
                // helper that unpacks first - packing a still-raw header
                // corrupts the seqnum and the host drops the reply.
                send_stage2_submit_deferred(buf, item->buf, item->length);

                if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
                {
                    --dap_respond;
                    xSemaphoreGive(data_response_mux);
                }

                vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
                return 1;
            }
            else if (packetSize > 0)
            {
                os_printf("Wrong data out packet size:%d!\r\n", packetSize);
            }
        }

        // The host pipelines its bulk transfers: the bulk-IN read often reaches
        // us before the bulk-OUT command it belongs to has been processed.
        // Completing the URB with zero bytes made "CMD_INFO failed" happen, so
        // park the URB and let the main TCP loop answer it in order.
        if (!dap_defer_read(buf))
        {
            os_printf("DAP read queue full, dropping one reply\r\n");
            send_stage2_submit_deferred(buf, NULL, 0);
        }
        return 1;
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////
// Parked bulk-IN URB
//
// The host keeps a bulk-IN transfer in flight while it submits the bulk-OUT
// command, so the read can arrive first. We keep the raw (wire order) header
// of that read and answer it from the main TCP loop once the DAP thread has
// produced the response.
//////////////////////////////////////////////////////////////////////////
static uint8_t deferred_reads[DAP_DEFERRED_READ_MAX][sizeof(usbip_stage2_header)];
static TickType_t deferred_read_tick[DAP_DEFERRED_READ_MAX];
static int deferred_read_head = 0;
static int deferred_read_count = 0;

int dap_defer_read(const uint8_t *raw_header)
{
    if (deferred_read_count >= DAP_DEFERRED_READ_MAX)
    {
        return 0;
    }

    int idx = (deferred_read_head + deferred_read_count) % DAP_DEFERRED_READ_MAX;
    memcpy(deferred_reads[idx], raw_header, sizeof(usbip_stage2_header));
    deferred_read_tick[idx] = xTaskGetTickCount();
    ++deferred_read_count;
    return 1;
}

int dap_service_deferred_read(void)
{
    int serviced = 0;

    while (deferred_read_count > 0 && dap_dataOUT_handle != NULL)
    {
        if (dap_respond <= 0)
        {
            // Safety net: never keep the oldest read blocked forever.
            if ((xTaskGetTickCount() - deferred_read_tick[deferred_read_head]) >
                pdMS_TO_TICKS(DAP_RESPONSE_TIMEOUT_MS))
            {
                send_stage2_submit_deferred(deferred_reads[deferred_read_head], NULL, 0);
                deferred_read_head = (deferred_read_head + 1) % DAP_DEFERRED_READ_MAX;
                --deferred_read_count;
                serviced = 1;
                continue;
            }
            break;
        }

        size_t packetSize = 0;
        DapPacket_t *item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataOUT_handle, &packetSize,
                                                                  pdMS_TO_TICKS(10), DAP_HANDLE_SIZE);
        if (packetSize != DAP_HANDLE_SIZE)
        {
            if (packetSize > 0)
            {
                os_printf("Wrong data out packet size:%d!\r\n", (int)packetSize);
            }
            break;
        }

        uint8_t *header = deferred_reads[deferred_read_head];
        deferred_read_head = (deferred_read_head + 1) % DAP_DEFERRED_READ_MAX;
        --deferred_read_count;

#if (USE_WINUSB == 1)
        send_stage2_submit_deferred(header, item->buf, item->length);
#else
        send_stage2_submit_deferred(header, item->buf, DAP_HANDLE_SIZE);
#endif

        if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
        {
            --dap_respond;
            xSemaphoreGive(data_response_mux);
        }

        vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
        serviced = 1;
    }

    return serviced;
}

void handle_dap_unlink()
{
    // `USBIP_CMD_UNLINK` means calling `usb_unlink_urb()` or `usb_kill_urb()`.
    // Note that execution of an URB is inherently an asynchronous operation, and there may be
    // synchronization problems in the following solutions.

    // One of the reasons this happens is that the host wants to abort the URB transfer operation
    // as soon as possible. USBIP network fluctuations will also cause this error, but I don't know
    // whether this is the main reason.

    // Unlink may be applied to zero length URB of "DIR_IN", or a URB containing data.
    // In the case of unlink, for the new "DIR_IN" request, it may always return an older response,
    // which will lead to panic. This code is a compromise for eliminating the lagging response
    // caused by UNLINK. It will clean up the buffers that may have data for return to the host.
    // In general, we assume that there is at most one piece of data that has not yet been returned.
    if (dap_respond > 0)
    {
        DapPacket_t *item;
        size_t packetSize = 0;
        item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataOUT_handle, &packetSize,
                                                     pdMS_TO_TICKS(10), DAP_HANDLE_SIZE);
        if (packetSize == DAP_HANDLE_SIZE)
        {
            if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
            {
                --dap_respond;
                xSemaphoreGive(data_response_mux);
            }

            vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
        }
    }

    // A parked read belongs to the URB the host just cancelled; drop the whole
    // queue so stale reads cannot steal the response of a later command.
    deferred_read_head = 0;
    deferred_read_count = 0;
}