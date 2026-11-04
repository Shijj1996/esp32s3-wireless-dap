#ifndef __DAP_HANDLE_H__
#define __DAP_HANDLE_H__

#include "components/USBIP/usbip_defs.h"

enum reset_handle_t
{
    NO_SIGNAL = 0,
    RESET_HANDLE = 1,
    DELETE_HANDLE = 2,
};

void handle_dap_data_request(usbip_stage2_header *header, uint32_t length);
void handle_dap_data_response(usbip_stage2_header *header);
void handle_swo_trace_response(usbip_stage2_header *header);
void handle_dap_unlink();

int fast_reply(uint8_t *buf, uint32_t length);

// A bulk-IN URB may arrive before the DAP thread produced the matching answer
// (the host submits the read right after the write). Park it instead of
// completing it with zero bytes, then answer it from the main TCP loop.
int dap_defer_read(const uint8_t *raw_header);
int dap_service_deferred_read(void);

#endif