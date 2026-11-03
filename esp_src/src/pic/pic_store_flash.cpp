/*
 * The picfw partition for pic_store.c: the ESP-IDF partition API. Without
 * the partition (an image on the old table), the store stays absent and
 * every PIC update is refused, saying why.
 */
#include "pic_store_flash.h"

#include <esp_partition.h>

#include "pic_store.h"

#define PICFW_LABEL "picfw"

static const esp_partition_t *partition;

static bool read_flash(uint32_t offset, void *data, uint32_t length)
{
    return esp_partition_read(partition, offset, data, length) == ESP_OK;
}


static bool write_flash(uint32_t offset, const void *data, uint32_t length)
{
    return esp_partition_write(partition, offset, data, length) == ESP_OK;
}


static bool erase_flash(uint32_t offset, uint32_t length)
{
    return esp_partition_erase_range(partition, offset, length) == ESP_OK;
}


static const PicStoreFlash flash = {read_flash, write_flash, erase_flash};

bool pic_store_flash_init(void)
{
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                         PICFW_LABEL);
    if (partition == NULL || partition->size < PIC_STORE_BYTES)
    {
        partition = NULL;
        pic_store_init(NULL);
        return false;
    }
    pic_store_init(&flash);
    return true;
}
