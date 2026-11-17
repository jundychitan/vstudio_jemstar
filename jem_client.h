#ifndef JEM_CLIENT_H
#define JEM_CLIENT_H

#include "jem_link.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Jemstar session client. Implements the command sequence captured in
 * LP_Read.txt:
 *   06 01  identity      -> firmware / model strings
 *   06 03  meter config  -> 4 x uint32 (LP geometry; meaning TBD)
 *   50 01  logon         -> password, meter replies with bare ACK (0x06)
 *   44 01  read profile  -> multi-block load-profile image
 *   50 02  logoff        -> meter replies with bare ACK (0x06)
 */

typedef struct {
    jem_link_t link;
    char       firmware[32];   /* e.g. "04.04.06" */
    char       model[32];      /* e.g. "0.U"      */
    char       serial[32];     /* best-effort meter serial (may be empty) */
    uint32_t   cfg[4];         /* raw uint32 values from 06 03 */
    int        cfg_count;
} jem_client_t;

void jem_client_init(jem_client_t *c, transport_t *tp,
                     uint8_t master_addr, uint8_t meter_addr);

/* 06 01: read identity; fills firmware/model. Returns 0 / -1. */
int jem_identify(jem_client_t *c);

/* 06 03: read meter/LP config words. Returns 0 / -1. */
int jem_read_meter_config(jem_client_t *c);

/*
 * 50 01: logon. If 'username' is non-NULL and non-empty the data field is
 * "<username>\0<password>\0"; otherwise it is just the raw password
 * (default "000000" if 'password' is NULL). Returns 0 / -1.
 */
int jem_logon(jem_client_t *c, const char *username, const char *password);

/*
 * 44 01: read the load-profile image (multi-block) into buf.
 * 'profile' is the request selector byte (observed value 0x01).
 * Returns the total payload length, or -1 on error.
 */
int jem_read_loadprofile(jem_client_t *c, uint8_t profile,
                         uint8_t *buf, size_t max);

/* 50 02: logoff. Returns 0 / -1. */
int jem_logoff(jem_client_t *c);

/*
 * 56 1b: read the meter-identification block (multi-block) and extract the
 * numeric Meter ID (first run of >= 6 ASCII digits, e.g. "164934833") into
 * out (NUL-terminated). Requires an active logon. Returns 0 / -1.
 */
int jem_read_meter_id(jem_client_t *c, char *out, size_t out_sz);

/*
 * 52 01: read the instantaneous register block (15 x int32, little-endian)
 * into out[]. Returns the number of int32 values read, or -1 on error.
 */
int jem_read_instantaneous(jem_client_t *c, int32_t *out, int max);

/*
 * 52 12: read the Normal Registers block into buf (multi-block).
 * Requires an active logon. Returns the payload length, or -1 on error.
 */
int jem_read_registers(jem_client_t *c, uint8_t *buf, size_t max);

/*
 * Extract a register value by label from a block read with jem_read_registers.
 * Each register's value is the IEEE float64 (little-endian) stored 22 bytes
 * before its ASCII label (e.g. "Working kWh Del"). Returns 0 / -1.
 */
int jem_register_extract(const uint8_t *buf, int len, const char *label, double *out);

/*
 * Extract a date/time register (e.g. "Present Date") as a Unix timestamp.
 * Date registers store a uint32 (little-endian) 14 bytes before the label.
 * Returns 0 / -1.
 */
int jem_register_extract_time(const uint8_t *buf, int len, const char *label, uint32_t *out);

#endif /* JEM_CLIENT_H */
