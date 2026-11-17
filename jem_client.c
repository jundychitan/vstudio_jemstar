#include "jem_client.h"

#include <stdio.h>
#include <string.h>

void jem_client_init(jem_client_t *c, transport_t *tp,
                     uint8_t master_addr, uint8_t meter_addr)
{
    memset(c, 0, sizeof(*c));
    jem_link_init(&c->link, tp, master_addr, meter_addr);
}

/* 06 01 - identity.
 * Observed data: 09 01 01 0c "04.04.06\0" "0.U\0" <trailing binary>.
 * We extract the first two printable-ASCII tokens as firmware and model. */
int jem_identify(jem_client_t *c)
{
    uint8_t data[256];
    int n = jem_read_multiblock(&c->link, 0x06, 0x01, NULL, 0, data, sizeof(data));
    if (n < 0) return -1;

    char  *dst[2] = { c->firmware, c->model };
    size_t dsz[2] = { sizeof(c->firmware), sizeof(c->model) };
    int    ti = 0;
    size_t i = 0;

    while (i < (size_t)n && ti < 2) {
        while (i < (size_t)n && !(data[i] >= 0x20 && data[i] < 0x7f)) i++;  /* skip */
        size_t start = i;
        while (i < (size_t)n &&  (data[i] >= 0x20 && data[i] < 0x7f)) i++;  /* token */
        size_t len = i - start;
        if (len >= 2) {                                 /* ignore stray single chars */
            size_t cpy = (len < dsz[ti] - 1) ? len : dsz[ti] - 1;
            memcpy(dst[ti], data + start, cpy);
            dst[ti][cpy] = '\0';
            ti++;
        }
    }
    return 0;
}

/* 06 03 - meter/LP configuration words (four little-endian uint32). */
int jem_read_meter_config(jem_client_t *c)
{
    uint8_t data[64];
    int n = jem_read_multiblock(&c->link, 0x06, 0x03, NULL, 0, data, sizeof(data));
    if (n < 0) return -1;

    c->cfg_count = n / 4;
    if (c->cfg_count > 4) c->cfg_count = 4;
    for (int i = 0; i < c->cfg_count; i++) {
        c->cfg[i] = (uint32_t)data[i * 4]
                  | ((uint32_t)data[i * 4 + 1] << 8)
                  | ((uint32_t)data[i * 4 + 2] << 16)
                  | ((uint32_t)data[i * 4 + 3] << 24);
    }
    return 0;
}

/* 50 01 - logon; meter replies with a bare ACK (0x06).
 * Data:  no username  -> <password>            (raw, e.g. "000000")
 *        with username -> <username>\0<password>\0  */
int jem_logon(jem_client_t *c, const char *username, const char *password)
{
    const char *pw = password ? password : "000000";
    uint8_t data[128];
    size_t  n = 0;

    if (username && username[0]) {
        size_t ul = strlen(username), pl = strlen(pw);
        if (ul + pl + 2 > sizeof(data)) return -1;
        memcpy(data + n, username, ul); n += ul;
        data[n++] = 0x00;
        memcpy(data + n, pw, pl);       n += pl;
        data[n++] = 0x00;
    } else {
        size_t pl = strlen(pw);
        if (pl > sizeof(data)) return -1;
        memcpy(data + n, pw, pl);       n += pl;
    }

    if (jem_send_command(&c->link, 0x50, 0x01, data, n) < 0)
        return -1;

    int b = jem_read_byte(&c->link);
    if (b != JEM_ACK) {
        fprintf(stderr, "jem: logon not acknowledged (got 0x%02x)\n", b & 0xFF);
        return -1;
    }
    return 0;
}

/* 44 01 - read the load-profile image (multi-block, self-terminating). */
int jem_read_loadprofile(jem_client_t *c, uint8_t profile,
                         uint8_t *buf, size_t max)
{
    uint8_t req[1] = { profile };
    return jem_read_multiblock(&c->link, 0x44, 0x01, req, 1, buf, max);
}

/* 50 02 - logoff; meter replies with a bare ACK (0x06). */
int jem_logoff(jem_client_t *c)
{
    if (jem_send_command(&c->link, 0x50, 0x02, NULL, 0) < 0)
        return -1;

    int b = jem_read_byte(&c->link);
    if (b != JEM_ACK) {
        fprintf(stderr, "jem: logoff not acknowledged (got 0x%02x)\n", b & 0xFF);
        return -1;
    }
    return 0;
}

/* 56 1b - meter-identification block; extract the first run of >= 6 digits. */
int jem_read_meter_id(jem_client_t *c, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    uint8_t buf[1024];
    int n = jem_read_multiblock(&c->link, 0x56, 0x1b, NULL, 0, buf, sizeof(buf));
    if (n < 0) return -1;

    for (int i = 0; i < n; ) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            int s = i;
            while (i < n && buf[i] >= '0' && buf[i] <= '9') i++;
            int len = i - s;
            if (len >= 6) {
                if ((size_t)len > out_sz - 1) len = (int)out_sz - 1;
                memcpy(out, buf + s, len);
                out[len] = '\0';
                return 0;
            }
        } else {
            i++;
        }
    }
    return -1;   /* no meter-id-like digit run found */
}

/* 52 01 - instantaneous register block (15 x int32 LE). */
int jem_read_instantaneous(jem_client_t *c, int32_t *out, int max)
{
    uint8_t buf[256];
    int n = jem_read_multiblock(&c->link, 0x52, 0x01, NULL, 0, buf, sizeof(buf));
    if (n < 0) return -1;

    int cnt = n / 4;
    if (cnt > max) cnt = max;
    for (int i = 0; i < cnt; i++) {
        out[i] = (int32_t)((uint32_t)buf[i*4]
                         | ((uint32_t)buf[i*4+1] << 8)
                         | ((uint32_t)buf[i*4+2] << 16)
                         | ((uint32_t)buf[i*4+3] << 24));
    }
    return cnt;
}

/* 52 12 - Normal Registers. Each register record stores its value as a
 * little-endian IEEE float64 located 22 bytes before its ASCII label. */
#define JEM_REG_VALUE_OFFSET  22

int jem_read_registers(jem_client_t *c, uint8_t *buf, size_t max)
{
    uint8_t req = 0xFF;                 /* 0xFF = read all normal registers */
    return jem_read_multiblock(&c->link, 0x52, 0x12, &req, 1, buf, max);
}

int jem_register_extract(const uint8_t *buf, int len, const char *label, double *out)
{
    if (!buf || !label || !out) return -1;

    size_t ll = strlen(label);
    for (int i = 0; i + (int)ll <= len; i++) {
        if (memcmp(buf + i, label, ll) == 0) {
            int vpos = i - JEM_REG_VALUE_OFFSET;
            if (vpos < 0 || vpos + 8 > len) return -1;
            double d;
            memcpy(&d, buf + vpos, 8);   /* LE float64 (host is little-endian) */
            *out = d;
            return 0;
        }
    }
    return -1;   /* label not found */
}

/* Date/time registers store a uint32 (LE) Unix timestamp 14 bytes before the
 * label (a different layout from the float64 value registers). */
#define JEM_REG_TIME_OFFSET  14

int jem_register_extract_time(const uint8_t *buf, int len, const char *label, uint32_t *out)
{
    if (!buf || !label || !out) return -1;

    size_t ll = strlen(label);
    for (int i = 0; i + (int)ll <= len; i++) {
        if (memcmp(buf + i, label, ll) == 0) {
            int vpos = i - JEM_REG_TIME_OFFSET;
            if (vpos < 0 || vpos + 4 > len) return -1;
            *out = (uint32_t)buf[vpos]
                 | ((uint32_t)buf[vpos+1] << 8)
                 | ((uint32_t)buf[vpos+2] << 16)
                 | ((uint32_t)buf[vpos+3] << 24);
            return 0;
        }
    }
    return -1;
}
