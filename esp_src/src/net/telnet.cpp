#include "telnet.h"

#include <ESPTelnetStream.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <stdlib.h>
#include <string.h>

#include "../console/commands.h"
#include "../console/console.h"
#include "../settings/user_prefs.h"
#include "telnet_auth.h"
#include "wifi_link.h"

/*
 * The library's stream, without its flush(). ESPTelnet calls flush() on
 * every connect and disconnect (emptyClientStream()), and on the ESP32 that
 * waits for the client's write buffer to read steady and above zero for
 * 10 ms, up to the keep-alive interval (1 s). NetworkClient does not report
 * that buffer (availableForWrite() is 0), so every connection held loop()
 * up for the whole second, plus 50 ms: an ESP PPS edge run on and a late
 * TIME-NTP for each (seen once a minute with tools/clock_monitor.py). Our
 * output never needs it: replies and console text go through the queues
 * below, written with MSG_DONTWAIT.
 */
class TelnetLink : public ESPTelnetStream
{
  public:
    void flush() override
    {
    }
};

static TelnetLink telnet;
static bool running;
static ConsoleLine line;
// Console output waiting for the socket, sent from telnet_service().
#define TX_BUFFER_BYTES 8192U
static uint8_t tx_buffer[TX_BUFFER_BYTES];
static uint32_t tx_head; // next byte to send
static uint32_t tx_count;
static uint32_t pending_dropped; // bytes dropped since the last note
static uint32_t dropped_bytes;   // ... in all
static uint32_t connections;
static bool authorised; // this connection has logged in

static void tx_flush(void);
static void reply_discard(void);
static size_t reply_write(const uint8_t *text, size_t length);
static void reply_drain(uint32_t timeout_ms);
static uint32_t disconnections;

/*
 * A command's reply: written to the socket as far as it takes it now, the
 * rest queued (reply_*) and sent from telnet_service(), so a long reply
 * (ntp gnss -v is about 25 KB) never holds up loop(). Written straight to
 * the telnet stream, it did: seconds over Wi-Fi, with the TIME-NTP reports,
 * the PIC link and the LED stopped meanwhile.
 */
class ReplyStream : public Stream
{
  public:
    size_t write(uint8_t byte) override
    {
        return reply_write(&byte, 1);
    }
    size_t write(const uint8_t *buffer, size_t size) override
    {
        return reply_write(buffer, size);
    }
    int available(void) override
    {
        return 0;
    }
    int read(void) override
    {
        return -1;
    }
    int peek(void) override
    {
        return -1;
    }
    // Before a restart: what is queued, waiting up to a second for it.
    void flush(void) override
    {
        reply_drain(1000);
    }
};

static ReplyStream reply_stream;
// The queued part of the reply, at reply[reply_head]; from the heap, grown
// as needed up to REPLY_MAX_BYTES, freed once sent.
#define REPLY_MAX_BYTES 49152U
static uint8_t *reply;
static uint32_t reply_size;
static uint32_t reply_head;
static uint32_t reply_count;

static void on_connect(String ip)
{
    Serial.printf("\nTelnet: %s connected\n", ip.c_str());
    telnet.printf("\nSmol Clock, %s\n(type 'quit' or 'exit' to disconnect; "
                  "'telnet login <password>' to change anything)\n> ",
                  WiFi.getHostname());
    authorised = false;
    console_line_clear(&line);
    tx_head = 0;
    tx_count = 0;
    reply_discard();
    pending_dropped = 0;
    connections++;
}

static void on_disconnect(String ip)
{
    Serial.printf("Telnet: %s disconnected\n", ip.c_str());
    authorised = false;
    disconnections++;
}

static void on_reconnect(String ip)
{
    Serial.printf("\nTelnet: %s reconnected\n", ip.c_str());
}

static void on_attempt(String ip)
{
    Serial.printf("Telnet: %s tried to connect\n", ip.c_str());
}

void telnet_init(Stream *output)
{
    telnet_auth_init();
    telnet.onConnect(on_connect);
    telnet.onConnectionAttempt(on_attempt);
    telnet.onReconnect(on_reconnect);
    telnet.onDisconnect(on_disconnect);
    if (user_prefs.fields.telnet.flags.enabled)
    {
        telnet_start(output);
    }
    else
    {
        output->printf("Telnet disabled, enable with \"telnet on\"\n");
    }
}

void telnet_service(void)
{
    if (!running)
    {
        return;
    }
    telnet.loop();
    if (!telnet.isConnected())
    {
        tx_count = 0;
        reply_discard();
        return;
    }
    tx_flush();
    if (reply_count != 0)
    {
        return; // the next command once this reply is out
    }
    if (console_read_line(&line, &telnet, false))
    {
        if (strcmp(line.text, "quit") == 0 || strcmp(line.text, "exit") == 0)
        {
            telnet.disconnectClient();
        }
        else
        {
            tx_flush(); // what came before, before the reply
            commands_execute(&reply_stream, line.text);
            reply_stream.print("> ");
        }
        console_line_clear(&line);
    }
}

// Its addresses and port: 192.0.2.5:23, [2001:db8::5]:23; or just the port
// without a network yet.
static void print_listening(Stream *output)
{
    if (wifi_link_is_up())
    {
        wifi_link_print_addresses(output, user_prefs.fields.telnet.port);
        output->printf("\n");
    }
    else
    {
        output->printf("port %u once the network is up\n", user_prefs.fields.telnet.port);
    }
}


void telnet_start(Stream *output)
{
    if (running)
    {
        output->printf("Telnet already running on ");
        print_listening(output);
        return;
    }
    // On :: : IPv6 and IPv4 (as IPv4-mapped) clients alike.
    telnet.begin(user_prefs.fields.telnet.port, false);
    running = true;
    output->printf("Telnet running, connect on ");
    print_listening(output);
}

void telnet_stop(Stream *output)
{
    if (!running)
    {
        output->printf("Telnet is not running\n");
        return;
    }
    output->printf("Telnet stopping; the current session stays open\n");
    if (user_prefs.fields.telnet.flags.enabled)
    {
        output->printf("It starts again at reset: turn that off with \"telnet off\"\n");
    }
    telnet.stop(false);
    running = false;
}

void telnet_enable(Stream *output)
{
    user_prefs.fields.telnet.flags.enabled = 1;
    output->printf("Telnet enabled\n");
    if (!running)
    {
        telnet_start(output);
    }
}

void telnet_disable(Stream *output)
{
    user_prefs.fields.telnet.flags.enabled = 0;
    output->printf("Telnet disabled%s\n",
                   running ? " but still running: stop it with \"telnet stop\"" : "");
}

void telnet_set_port(Stream *output, uint16_t port)
{
    if (port == user_prefs.fields.telnet.port)
    {
        return;
    }
    user_prefs.fields.telnet.port = port;
    if (running)
    {
        telnet_stop(output);
        telnet_start(output);
    }
}

bool telnet_authorised(void)
{
    return authorised;
}


void telnet_set_authorised(bool on)
{
    authorised = on;
}


bool telnet_is_running(void)
{
    return running;
}

Stream *telnet_client(void)
{
    return running && telnet.isConnected() ? &reply_stream : NULL;
}

// Into the buffer; what does not fit is dropped and counted.
static void buffer_text(const uint8_t *text, size_t length)
{
    uint32_t room = TX_BUFFER_BYTES - tx_count;
    uint32_t tail;
    size_t index;

    if (length > room)
    {
        pending_dropped += (uint32_t)(length - room);
        dropped_bytes += (uint32_t)(length - room);
        length = room;
    }
    tail = (tx_head + tx_count) % TX_BUFFER_BYTES;
    for (index = 0; index < length; index++)
    {
        tx_buffer[tail] = text[index];
        tail = (tail + 1) % TX_BUFFER_BYTES;
    }
    tx_count += (uint32_t)length;
}

void telnet_write_text(const uint8_t *text, size_t length)
{
    if (!running || !telnet.isConnected())
    {
        return;
    }
    if (reply_count != 0)
    {
        reply_write(text, length); // behind the reply, in order
        return;
    }
    if (pending_dropped != 0 && TX_BUFFER_BYTES - tx_count >= 48U + length)
    {
        char note[48];
        int n = snprintf(note, sizeof(note), "\r\n[telnet: %lu bytes dropped]\r\n",
                         (unsigned long)pending_dropped);

        pending_dropped = 0;
        buffer_text((const uint8_t *)note, (size_t)n);
    }
    buffer_text(text, length);
}

/*
 * As much of the buffer as the socket takes now, without waiting. One send
 * per pass of loop() gathers the many small writes of a burst (the PIC's
 * start-up text arrives a few bytes at a time) into full segments: lwIP
 * limits the segments queued as well as the bytes, so a send per piece ran
 * out of segments with the window still open.
 */
static void tx_flush(void)
{
    int fd;

    if (tx_count == 0 && reply_count == 0)
    {
        return;
    }
    fd = telnet.getClient().fd();
    if (fd < 0)
    {
        return;
    }
    while (tx_count != 0)
    {
        uint32_t chunk =
            TX_BUFFER_BYTES - tx_head < tx_count ? TX_BUFFER_BYTES - tx_head : tx_count;
        ssize_t sent = send(fd, tx_buffer + tx_head, chunk, MSG_DONTWAIT);

        if (sent <= 0)
        {
            return; // no room now: try again next pass
        }
        tx_head = (tx_head + (uint32_t)sent) % TX_BUFFER_BYTES;
        tx_count -= (uint32_t)sent;
        if ((uint32_t)sent < chunk)
        {
            return;
        }
    }
    // The ring is out: then the reply, which came after it.
    while (reply_count != 0)
    {
        ssize_t sent = send(fd, reply + reply_head, reply_count, MSG_DONTWAIT);

        if (sent <= 0)
        {
            return;
        }
        reply_head += (uint32_t)sent;
        reply_count -= (uint32_t)sent;
    }
    reply_discard(); // all sent: the memory back
}

static void reply_discard(void)
{
    free(reply);
    reply = NULL;
    reply_size = 0;
    reply_head = 0;
    reply_count = 0;
}

// Queued behind what is there; what does not fit (REPLY_MAX_BYTES, or no
// memory) is dropped and counted, as the console copy's is.
static void reply_queue(const uint8_t *text, size_t length)
{
    uint32_t need;

    if (reply_head != 0)
    {
        memmove(reply, reply + reply_head, reply_count);
        reply_head = 0;
    }
    need = reply_count + (uint32_t)length;
    if (need > reply_size)
    {
        uint32_t size = reply_size != 0 ? reply_size : 1024U;
        uint8_t *grown;

        while (size < need && size < REPLY_MAX_BYTES)
        {
            size *= 2U;
        }
        if (size > REPLY_MAX_BYTES)
        {
            size = REPLY_MAX_BYTES;
        }
        grown = (uint8_t *)realloc(reply, size);
        if (grown != NULL)
        {
            reply = grown;
            reply_size = size;
        }
    }
    if (length > reply_size - reply_count)
    {
        pending_dropped += (uint32_t)(length - (reply_size - reply_count));
        dropped_bytes += (uint32_t)(length - (reply_size - reply_count));
        length = reply_size - reply_count;
    }
    if (length != 0)
    {
        memcpy(reply + reply_count, text, length);
        reply_count += (uint32_t)length;
    }
}

/*
 * Straight to the socket while nothing is queued ahead of it (the console
 * copy's ring, or an earlier part of the reply), without waiting; the rest
 * queued. A command's progress lines still go out as it runs.
 */
static size_t reply_write(const uint8_t *text, size_t length)
{
    size_t taken = length;

    if (!running || !telnet.isConnected())
    {
        return taken;
    }
    tx_flush();
    if (tx_count == 0 && reply_count == 0)
    {
        int fd = telnet.getClient().fd();

        while (fd >= 0 && length != 0)
        {
            ssize_t sent = send(fd, text, length, MSG_DONTWAIT);

            if (sent <= 0)
            {
                break;
            }
            text += sent;
            length -= (size_t)sent;
        }
    }
    if (length != 0)
    {
        reply_queue(text, length);
    }
    return taken;
}

// Waiting for the queue to go, up to timeout_ms: only before a restart.
static void reply_drain(uint32_t timeout_ms)
{
    uint32_t start = millis();

    while ((tx_count != 0 || reply_count != 0) && telnet.isConnected() &&
           millis() - start < timeout_ms)
    {
        tx_flush();
        delay(5);
    }
}

void telnet_print(Stream *output, bool verbose)
{
    uint16_t port = user_prefs.fields.telnet.port;

    if (running && wifi_link_is_up())
    {
        output->printf("Telnet:   RUNNING ");
        wifi_link_print_addresses(output, port);
    }
    else
    {
        output->printf("Telnet:   %s, port %u", running ? "RUNNING, no network" : "STOPPED", port);
    }
    output->printf(", start-up %s", user_prefs.fields.telnet.flags.enabled ? "ON" : "OFF");
    if (running && telnet.isConnected())
    {
        output->printf(", client %s%s", telnet.getIP().c_str(),
                       authorised ? " (logged in)" : "");
    }
    if (telnet_auth_is_default())
    {
        output->printf(", DEFAULT PASSWORD (telnet passwd)");
    }
    if (!verbose)
    {
        output->printf("\n");
        return;
    }
    output->printf(", %lu wrong passwords since the last login", (unsigned long)telnet_auth_failures());
    output->printf(", %lu connections, %lu disconnections, %lu bytes dropped\n",
                   (unsigned long)connections, (unsigned long)disconnections,
                   (unsigned long)dropped_bytes);
}
