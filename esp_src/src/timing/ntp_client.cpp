#include "ntp_client.h"

#include <Arduino.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <string.h>

#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY    5
#define ARRIVALS         4 // replies queued by the callback, a burst's worth

// A reply as the callback took it: T4 and its first NTP_PACKET_BYTES.
typedef struct
{
    int64_t t4;
    uint16_t length;
    uint8_t bytes[NTP_PACKET_BYTES];
} Arrival;

static TaskHandle_t task;
static QueueHandle_t arrivals;
// busy, result and result_ready are shared by loop() and the client task:
// each read and written under lock, so neither depends on the two tasks
// sharing a core or on the order of plain stores.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static NtpServerAddress server_address;
static bool busy;
static bool result_ready;
static NtpPollResult result;
static NtpClientStats stats;

static uint64_t random_cookie(void)
{
    return (uint64_t)esp_random() << 32 | esp_random();
}


/*
 * A reply arriving, in lwIP's own task: T4 taken first thing, before the
 * hand-over to the client task, so it does not include waking that task
 * (the socket's mailbox and a context switch: tens of us, and with them
 * much of the ESP's own asymmetry, about 100 us on the test LAN).
 */
static void on_reply(void *argument, struct udp_pcb *pcb, struct pbuf *packet, const ip_addr_t *from,
                     u16_t port)
{
    Arrival arrival;

    (void)argument;
    (void)pcb;
    (void)from;
    (void)port;
    arrival.t4 = esp_timer_get_time();
    arrival.length = pbuf_copy_partial(packet, arrival.bytes, sizeof(arrival.bytes), 0);
    pbuf_free(packet);
    xQueueSend(arrivals, &arrival, 0); // full: dropped, as a socket would when full
}


// One request and its reply. True with a sample; poll->last_result and
// poll->kiss say why not.
static bool exchange(struct udp_pcb *pcb, const ip_addr_t *server, EspClockSample *sample,
                     NtpPollResult *poll)
{
    uint8_t request[NTP_PACKET_BYTES];
    uint64_t cookie = random_cookie();
    struct pbuf *packet;
    NtpReply reply;
    Arrival arrival;
    err_t sent;
    int64_t t1;
    int64_t t4;
    int64_t deadline;

    ntp_packet_request(request, cookie);
    LOCK_TCPIP_CORE();
    packet = pbuf_alloc(PBUF_TRANSPORT, sizeof(request), PBUF_RAM);
    if (packet == NULL)
    {
        UNLOCK_TCPIP_CORE();
        return false;
    }
    memcpy(packet->payload, request, sizeof(request));
    sent = udp_sendto(pcb, packet, server, NTP_PORT);
    // T1 once lwIP has handed the request to the Wi-Fi driver: not its own
    // processing, only the driver's queue and the air.
    t1 = esp_timer_get_time();
    pbuf_free(packet);
    UNLOCK_TCPIP_CORE();
    if (sent != ERR_OK)
    {
        return false;
    }
    deadline = t1 + NTP_CLIENT_TIMEOUT_MS * 1000LL;
    for (;;)
    {
        int64_t wait_us = deadline - esp_timer_get_time();

        if (wait_us < 0 || xQueueReceive(arrivals, &arrival, pdMS_TO_TICKS(wait_us / 1000 + 1)) != pdTRUE)
        {
            stats.timeouts++;
            return false;
        }
        t4 = arrival.t4;
        NtpReplyResult outcome = ntp_packet_parse(arrival.bytes, arrival.length, cookie, &reply);
        poll->last_result = outcome;
        if (outcome == NTP_REPLY_OK)
        {
            break;
        }
        stats.rejected[outcome]++;
        if (outcome == NTP_REPLY_KISS)
        {
            memcpy(poll->kiss, reply.kiss, sizeof(poll->kiss));
            return false;
        }
        // A late reply to an earlier request: keep listening until the deadline.
        if (outcome != NTP_REPLY_WRONG_ORIGIN || t4 >= deadline)
        {
            return false;
        }
    }
    stats.exchanges++;
    // offset = ((T2 - T1) + (T3 - T4)) / 2, delay = (T4 - T1) - (T3 - T2).
    sample->local_us = t1 + (t4 - t1) / 2;
    sample->offset_us = ((reply.receive_utc_us - t1) + (reply.transmit_utc_us - t4)) / 2;
    sample->delay_us = (t4 - t1) - (reply.transmit_utc_us - reply.receive_utc_us);
    if (sample->delay_us < 0)
    {
        sample->delay_us = 0;
    }
    sample->root_delay_us = reply.root_delay_us;
    sample->root_dispersion_us = reply.root_dispersion_us;
    sample->stratum = reply.stratum;
    return true;
}


/*
 * A poll on lwIP's raw UDP API (under the TCPIP core lock, which this core
 * builds with), not a socket, for the earlier T4 (on_reply) and T1
 * (exchange). Its requests carry DSCP EF (NTP_CLIENT_DSCP): Wi-Fi with WMM
 * queues them ahead of bulk traffic, where the driver and the access point
 * honour it, so less queueing, and less of it on one side only.
 */
static void poll_server(const NtpServerAddress *address, NtpPollResult *poll)
{
    IPAddress shown(address->ipv6 ? IPv6 : IPv4, address->bytes, address->zone);
    ip_addr_t server;
    struct udp_pcb *pcb;
    int index;

    memset(poll, 0, sizeof(*poll));
    // As IPAddress writes it, as everywhere else.
    snprintf(poll->address, sizeof(poll->address), "%s", shown.toString().c_str());
    shown.to_ip_addr_t(&server);
    xQueueReset(arrivals); // anything late from the last poll
    LOCK_TCPIP_CORE();
    pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb != NULL)
    {
        pcb->tos = NTP_CLIENT_DSCP << 2;
        udp_recv(pcb, on_reply, NULL);
    }
    UNLOCK_TCPIP_CORE();
    if (pcb == NULL)
    {
        return;
    }
    for (index = 0; index < NTP_CLIENT_BURST; index++)
    {
        EspClockSample sample;

        if (index != 0)
        {
            vTaskDelay(pdMS_TO_TICKS(NTP_CLIENT_SPACING_MS));
        }
        if (exchange(pcb, &server, &sample, poll))
        {
            if (!poll->ok || sample.delay_us < poll->sample.delay_us)
            {
                poll->sample = sample;
            }
            poll->ok = true;
            poll->replies++;
        }
        else if (poll->kiss[0] != '\0')
        {
            break; // told to go away: no more requests this poll
        }
    }
    LOCK_TCPIP_CORE();
    udp_remove(pcb); // no callback after this
    UNLOCK_TCPIP_CORE();
}


static void client_task(void *argument)
{
    (void)argument;
    for (;;)
    {
        NtpPollResult poll;

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        poll_server(&server_address, &poll);
        stats.polls++;
        if (poll.ok)
        {
            stats.good_polls++;
        }
        portENTER_CRITICAL(&lock);
        result = poll;
        result_ready = true;
        busy = false; // with the result, never before it
        portEXIT_CRITICAL(&lock);
    }
}


void ntp_client_init(void)
{
    arrivals = xQueueCreate(ARRIVALS, sizeof(Arrival));
    xTaskCreatePinnedToCore(client_task, "ntp", TASK_STACK_BYTES, NULL, TASK_PRIORITY, &task,
                            ARDUINO_RUNNING_CORE);
}


bool ntp_client_poll(const NtpServerAddress *address)
{
    if (task == NULL)
    {
        return false;
    }
    portENTER_CRITICAL(&lock);
    if (busy)
    {
        portEXIT_CRITICAL(&lock);
        return false;
    }
    busy = true;
    // Read by the task only after the notification, and only while busy.
    server_address = *address;
    portEXIT_CRITICAL(&lock);
    xTaskNotifyGive(task);
    return true;
}


bool ntp_client_busy(void)
{
    bool now;

    portENTER_CRITICAL(&lock);
    now = busy;
    portEXIT_CRITICAL(&lock);
    return now;
}


bool ntp_client_take(NtpPollResult *destination)
{
    bool ready;

    portENTER_CRITICAL(&lock);
    ready = result_ready;
    if (ready)
    {
        *destination = result;
        result_ready = false;
    }
    portEXIT_CRITICAL(&lock);
    return ready;
}


const NtpClientStats *ntp_client_stats(void)
{
    return &stats;
}
