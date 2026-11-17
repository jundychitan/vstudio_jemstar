#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

/* Transport type */
typedef enum {
    TRANSPORT_SERIAL,
    TRANSPORT_TCP
} transport_type_t;

/* Serial parity */
typedef enum {
    PARITY_NONE,
    PARITY_EVEN,
    PARITY_ODD
} parity_t;

/* Serial parameters */
typedef struct {
    int  baudrate;    /* e.g. 300, 9600 */
    int  databits;    /* 7 or 8 */
    parity_t parity;  /* NONE, EVEN, ODD */
    int  stopbits;    /* 1 or 2 */
} serial_params_t;

/* Transport handle */
typedef struct {
    transport_type_t type;
    int              fd;        /* file descriptor (serial or socket) */
    int              verbose;
} transport_t;

/*
 * Open a serial port.
 * Returns 0 on success, -1 on error.
 */
int transport_open_serial(transport_t *tp, const char *device, const serial_params_t *params);

/*
 * Open a TCP connection.
 * Returns 0 on success, -1 on error.
 */
int transport_open_tcp(transport_t *tp, const char *host, int port);

/*
 * Change serial port parameters (baud, databits, parity, stopbits).
 * Only valid for TRANSPORT_SERIAL.
 * Returns 0 on success, -1 on error.
 */
int transport_set_serial_params(transport_t *tp, const serial_params_t *params);

/*
 * Write exactly 'len' bytes. Returns 0 on success, -1 on error.
 * If verbose, hex-dumps outgoing data.
 */
int transport_write(transport_t *tp, const uint8_t *buf, size_t len);

/*
 * Read up to 'max_len' bytes with a timeout in milliseconds.
 * Returns number of bytes read, 0 on timeout, -1 on error.
 */
int transport_read(transport_t *tp, uint8_t *buf, size_t max_len, int timeout_ms);

/*
 * Read bytes until 'terminator' byte is seen or timeout.
 * Returns number of bytes read (including terminator), 0 on timeout, -1 on error.
 */
int transport_read_until(transport_t *tp, uint8_t *buf, size_t max_len,
                         uint8_t terminator, int timeout_ms);

/*
 * Close the transport.
 */
void transport_close(transport_t *tp);

/*
 * Flush any pending input data.
 */
void transport_flush_input(transport_t *tp);

#endif /* TRANSPORT_H */
