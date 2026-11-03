/*
 * The PIC firmware's version, sent to the ESP in STAT-SYS in the same form as
 * the ESP's own (STAT-NET esp_version), and printed at start-up.
 */
#ifndef VERSION_H
#define VERSION_H

#include <stdint.h>

#define PIC_VERSION_MAJOR 0
#define PIC_VERSION_MINOR 3
#define PIC_VERSION_PATCH 13

#define PIC_VERSION_NUMBER                                                  \
    ((uint32_t)PIC_VERSION_MAJOR << 16 | (uint32_t)PIC_VERSION_MINOR << 8 | \
     (uint32_t)PIC_VERSION_PATCH)

#endif // VERSION_H
