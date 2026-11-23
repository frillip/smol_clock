#include "print_queue.h"

#include <string.h>

typedef struct
{
    PrintStep step;
    uint16_t arg;
    bool verbose;
} PrintItem;

static PrintItem items[PRINT_QUEUE_LENGTH]; // items[0] runs next
static uint8_t count;
// While a step runs: where the steps it queues go (after those it queued
// before them). Otherwise -1: at the end.
static int8_t insert_at = -1;
static PrintQueueStats stats;

bool print_queue_add(PrintStep step, uint16_t arg, bool verbose)
{
    uint8_t at;

    if (count == PRINT_QUEUE_LENGTH || step == 0)
    {
        stats.dropped++;
        return false;
    }
    at = insert_at >= 0 ? (uint8_t)insert_at++ : count;
    memmove(&items[at + 1], &items[at], (size_t)(count - at) * sizeof(items[0]));
    items[at].step = step;
    items[at].arg = arg;
    items[at].verbose = verbose;
    count++;
    if (count > stats.most)
    {
        stats.most = count;
    }
    return true;
}


uint8_t print_queue_free(void)
{
    return (uint8_t)(PRINT_QUEUE_LENGTH - count);
}


bool print_queue_busy(void)
{
    return count != 0U;
}


bool print_queue_service(uint32_t now_ms, bool room)
{
    PrintItem item;

    if (count == 0U || !room || insert_at >= 0)
    {
        return false; // nothing, no room, or called from a step
    }
    item = items[0];
    count--;
    memmove(&items[0], &items[1], (size_t)count * sizeof(items[0]));
    insert_at = 0;
    stats.last = item.step;
    stats.last_arg = item.arg;
    item.step(now_ms, item.arg, item.verbose);
    insert_at = -1;
    stats.steps++;
    return true;
}


const PrintQueueStats *print_queue_stats(void)
{
    return &stats;
}


void print_queue_reset(void)
{
    count = 0;
    insert_at = -1;
    memset(&stats, 0, sizeof(stats));
}
