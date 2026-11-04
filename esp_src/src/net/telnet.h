/*
 * The telnet console: the same commands as the serial console, and a copy
 * of the console output while a client is connected. Commands that change
 * anything need "telnet login" first on each connection (telnet_auth.h).
 *
 * The copy of the console output never blocks: it is buffered (8 KB) and
 * sent from telnet_service() as the connection takes it; what overflows the
 * buffer is dropped, counted, and noted in the stream when there is room. A plain write to a stalled connection (a Wi-Fi
 * hiccup, a client that stopped reading) waits up to 10 s in the network
 * stack and holds up loop() meanwhile; after three such failures the telnet
 * library dropped the client.
 */
#ifndef TELNET_H
#define TELNET_H

#include <Arduino.h>
#include <stdint.h>

void telnet_init(Stream *output);
void telnet_service(void);

void telnet_start(Stream *output);
void telnet_stop(Stream *output);
void telnet_enable(Stream *output);
void telnet_disable(Stream *output);
void telnet_set_port(Stream *output, uint16_t port);

bool telnet_is_running(void);
// This connection has logged in: cleared at every connection and disconnection.
bool telnet_authorised(void);
void telnet_set_authorised(bool on);
// The connected client's stream, or NULL. Blocking: for command replies.
Stream *telnet_client(void);
// Console output to the client, if any, without blocking (see above).
void telnet_write_text(const uint8_t *text, size_t length);
void telnet_print(Stream *output, bool verbose);

#endif // TELNET_H
