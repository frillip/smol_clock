// Host stand-in for drivers/uart2.c, used by the GNSS host tests.
// Implements the uart2.h API over plain queues, plus hooks for the test
// harness to inject received bytes and errors and to collect sent bytes.
#ifndef HOST_UART2_H
#define HOST_UART2_H
#include <stdint.h>
#include <stdbool.h>

void host_uart2_reset(void);
uint32_t host_uart2_baud(void);
uint32_t host_uart2_init_count(void);
// Bytes arriving from the receiver.
void host_uart2_inject_rx(const uint8_t *bytes, uint16_t length);
void host_uart2_inject_error(uint16_t flags);
// Bytes the dsPIC has queued for transmission; returns count taken.
uint16_t host_uart2_take_tx(uint8_t *destination, uint16_t capacity);

#endif
