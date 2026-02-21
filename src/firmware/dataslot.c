/*
 * Data Slot interface for Analogue Pocket
 *
 * Implements CPU-controlled data slot operations using APF target commands.
 */

#include "dataslot.h"
#include "libc/libc.h"
#include "terminal.h"

/* Parameter buffer in SDRAM (placed at a known location) */
/* We use the end of SDRAM test region to avoid conflicts */
#define PARAM_BUFFER_ADDR   0x10F00000  /* CPU address for param struct */
#define RESP_BUFFER_ADDR    0x10F01000  /* CPU address for response struct */

/* Timeout for operations (in loop iterations) */
/* 15 seconds at 133MHz with ~10 cycles/loop = 200M iterations */
#define TIMEOUT_LOOPS       200000000

__attribute__((section(".text.boot")))
int dataslot_wait_complete(void) {
    volatile int timeout = TIMEOUT_LOOPS;
    uint32_t status;

    term_printf("wait: initial status=%x\n", DS_STATUS);

    /* First, if ACK is already high from previous command, wait for it to clear.
     * This proves the bridge received our new command and cleared old status. */
    status = DS_STATUS;
    if (status & DS_STATUS_ACK) {
        term_printf("wait: ACK high, waiting to clear\n");
        while (DS_STATUS & DS_STATUS_ACK) {
            if (--timeout <= 0) {
                term_printf("wait: timeout at clear, t=%d\n", timeout);
                return -3;
            }
        }
        term_printf("wait: ACK cleared\n");
    }

    /* Wait for this command's ack */
    timeout = TIMEOUT_LOOPS;
    while (!(DS_STATUS & DS_STATUS_ACK)) {
        if (--timeout <= 0) {
            term_printf("wait: timeout at ack, t=%d\n", timeout);
            return -1;
        }
    }
    term_printf("wait: got ACK\n");

    /* Wait for done */
    timeout = TIMEOUT_LOOPS;
    while (!(DS_STATUS & DS_STATUS_DONE)) {
        if (--timeout <= 0) {
            term_printf("wait: timeout at done, t=%d s=%x\n", timeout, DS_STATUS);
            return -2;
        }
    }
    term_printf("wait: got DONE\n");

    /* Check error code */
    uint32_t final_status = DS_STATUS;
    int err = (final_status & DS_STATUS_ERR_MASK) >> DS_STATUS_ERR_SHIFT;
    term_printf("wait: final status=%x err=%d\n", final_status, err);
    return err ? -err : 0;
}

__attribute__((section(".text.boot")))
int dataslot_open_file(const char *filename, uint32_t flags, uint32_t size) {
    /* Build parameter struct in SDRAM */
    dataslot_open_param_t *param = (dataslot_open_param_t *)PARAM_BUFFER_ADDR;

    /* Clear and fill the struct */
    memset(param, 0, sizeof(*param));
    strncpy(param->filename, filename, 255);
    param->filename[255] = '\0';
    param->flags = flags;
    param->size = size;

    /* Set up registers */
    DS_SLOT_ID = 0;  /* Slot 0 for opened files */
    DS_PARAM_ADDR = CPU_TO_BRIDGE_ADDR(PARAM_BUFFER_ADDR);
    DS_RESP_ADDR = CPU_TO_BRIDGE_ADDR(RESP_BUFFER_ADDR);

    /* Trigger openfile command */
    DS_COMMAND = DS_CMD_OPENFILE;

    /* Wait for completion */
    return dataslot_wait_complete();
}

__attribute__((section(".text.boot")))
int dataslot_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length) {
    /* Validate destination is in SDRAM */
    uint32_t dest_addr = (uint32_t)dest;
    if (dest_addr < 0x10000000 || dest_addr >= 0x14000000) {
        return -10;  /* Invalid destination address */
    }

    uint32_t bridge_addr = CPU_TO_BRIDGE_ADDR(dest_addr);

    /* Debug: print parameters */
    term_printf("DS: slot=%d off=%x br=%x len=%x\n",
                slot_id, offset, bridge_addr, length);
    term_printf("DS: status before=%x\n", DS_STATUS);

    /* Set up registers */
    DS_SLOT_ID = slot_id;
    DS_SLOT_OFFSET = offset;
    DS_BRIDGE_ADDR = bridge_addr;
    DS_LENGTH = length;

    /* Trigger read command */
    DS_COMMAND = DS_CMD_READ;

    /* Wait for completion */
    int result = dataslot_wait_complete();

    term_printf("DS: status after=%x result=%d\n", DS_STATUS, result);

    /* Debug: print first 16 bytes of destination */
    volatile uint8_t *p = (volatile uint8_t *)dest;
    term_printf("DS: data=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

    return result;
}

__attribute__((section(".text.boot")))
int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length) {
    /* Validate source is in SDRAM */
    uint32_t src_addr = (uint32_t)src;
    if (src_addr < 0x10000000 || src_addr >= 0x14000000) {
        return -10;  /* Invalid source address */
    }

    /* Set up registers */
    DS_SLOT_ID = slot_id;
    DS_SLOT_OFFSET = offset;
    DS_BRIDGE_ADDR = CPU_TO_BRIDGE_ADDR(src_addr);
    DS_LENGTH = length;

    /* Trigger write command */
    DS_COMMAND = DS_CMD_WRITE;

    /* Wait for completion */
    return dataslot_wait_complete();
}

__attribute__((section(".text.boot")))
int32_t dataslot_load(uint16_t slot_id, void *dest, uint32_t max_length) {
    /* For now, just read the requested amount */
    /* TODO: Could query slot size first if needed */
    int result = dataslot_read(slot_id, 0, dest, max_length);
    if (result < 0) return result;
    return (int32_t)max_length;
}

__attribute__((section(".text.boot")))
int dataslot_get_size(uint16_t slot_id, uint32_t *size_out) {
    /* TODO: Implement proper slot size query via APF protocol */
    /* For now, return a large fixed size based on slot ID */
    if (size_out == NULL) return -1;

    switch (slot_id) {
        case 0:  /* SM64 binary */
            *size_out = 4 * 1024 * 1024;   /* 4 MB */
            break;
        case 1:  /* PAK data */
            *size_out = 20 * 1024 * 1024;  /* 20 MB */
            break;
        default:
            *size_out = 1 * 1024 * 1024;   /* 1 MB default */
            break;
    }
    return 0;
}
