#ifndef JEM_LINK_H
#define JEM_LINK_H

#include "transport.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Jemstar link layer (DLE-framed binary protocol).
 *
 * Frame layout (on the wire):
 *   10 01 | ADDR | FUNC SEL | 10 02 | <data, DLE-stuffed> | 10 03 | CRClo CRChi
 *
 *   10 01 = DLE+SOH (start of frame)
 *   10 02 = DLE+STX (start of data)
 *   10 03 = DLE+ETX (final block)      10 17 = DLE+ETB (more blocks follow)
 *   ADDR  = 0xFE master->meter, 0x01 meter->master (meter echoes FUNC SEL)
 *
 * DLE stuffing: a 0x10 data byte is doubled (10 10). Only 0x10 is escaped.
 *
 * CRC = CRC-16/ARC (poly 0x8005, reflected in/out, init 0x0000), computed over
 * the DLE-destuffed bytes from ADDR through the terminator (0x03/0x17)
 * inclusive, and appended little-endian (raw, not stuffed).
 * (Algorithm and coverage were reverse-engineered and verified against the
 * JemRead capture LP_Read.txt.)
 */

/* DLE control characters */
#define JEM_DLE  0x10
#define JEM_SOH  0x01
#define JEM_STX  0x02
#define JEM_ETX  0x03
#define JEM_ETB  0x17
#define JEM_ACK  0x06
#define JEM_NAK  0x15

/* Address bytes observed in the JemRead capture */
#define JEM_ADDR_MASTER  0xFE   /* master -> meter requests   */
#define JEM_ADDR_METER   0x01   /* meter  -> master responses */

#define JEM_DEFAULT_TIMEOUT_MS  2000
#define JEM_MAX_FRAME           4096   /* single wire frame incl. framing/CRC */

typedef struct {
    transport_t *tp;
    uint8_t      master_addr;   /* address byte we transmit (default 0xFE) */
    uint8_t      meter_addr;    /* address byte we expect back (default 0x01) */
    int          timeout_ms;    /* per-byte read timeout */
} jem_link_t;

void jem_link_init(jem_link_t *lnk, transport_t *tp,
                   uint8_t master_addr, uint8_t meter_addr);

/* CRC-16/ARC over 'len' bytes. */
uint16_t jem_crc16(const uint8_t *data, size_t len);

/*
 * Build a complete wire frame into 'out'.
 * Returns the wire length, or -1 on error (buffer too small / data too big).
 * Pure function (no I/O) so it can be unit-tested.
 */
int jem_build_frame(uint8_t addr, uint8_t func, uint8_t sel,
                    const uint8_t *data, size_t data_len,
                    uint8_t *out, size_t out_max);

/*
 * Parse a complete wire frame (from leading DLE SOH through the 2 CRC bytes).
 * Destuffs and verifies the CRC. On success returns the data-field length,
 * sets *out_func, *out_sel, *is_final (1 if ETX, 0 if ETB), and copies the
 * destuffed data field into out_data (clamped to out_max).
 * Returns -1 on malformed frame or CRC mismatch. Pure function.
 */
int jem_parse_frame(const uint8_t *raw, size_t raw_len,
                    uint8_t *out_func, uint8_t *out_sel,
                    uint8_t *out_data, size_t out_max, int *is_final);

/* Send a framed command. Returns 0 on success, -1 on error. */
int jem_send_command(jem_link_t *lnk, uint8_t func, uint8_t sel,
                     const uint8_t *data, size_t data_len);

/* Send a single ACK byte (0x06) to solicit the next block. */
int jem_send_ack(jem_link_t *lnk);

/* Read a single raw byte; returns 0..255, or -1 on timeout/error. */
int jem_read_byte(jem_link_t *lnk);

/*
 * Read one framed response from the wire (sync, collect, parse, CRC-check).
 * See jem_parse_frame for the returned values. Returns -1 on error.
 */
int jem_read_frame(jem_link_t *lnk, uint8_t *out_func, uint8_t *out_sel,
                   uint8_t *out_data, size_t out_max, int *is_final);

/*
 * Send a command, then read a multi-block response, ACKing each ETB block
 * until the final ETX block. Concatenates the data fields into out_data.
 * Returns the total payload length, or -1 on error.
 */
int jem_read_multiblock(jem_link_t *lnk, uint8_t func, uint8_t sel,
                        const uint8_t *req, size_t req_len,
                        uint8_t *out_data, size_t out_max);

/*
 * Built-in self-test of CRC / frame build / frame parse against known frames
 * from LP_Read.txt. Returns 0 if all pass, non-zero otherwise. Prints results.
 */
int jem_link_selftest(void);

#endif /* JEM_LINK_H */
