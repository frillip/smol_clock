#ifndef VERSION_H
#define VERSION_H

// The update server compares ESP_VERSION; STAT-NET carries the numeric form.
#define ESP_VERSION_MAJOR 0
#define ESP_VERSION_MINOR 3
#define ESP_VERSION_PATCH 8

#define VERSION_STRINGIFY_(x) #x
#define VERSION_STRINGIFY(x)  VERSION_STRINGIFY_(x)
#define ESP_VERSION                      \
    VERSION_STRINGIFY(ESP_VERSION_MAJOR) \
    "." VERSION_STRINGIFY(ESP_VERSION_MINOR) "." VERSION_STRINGIFY(ESP_VERSION_PATCH)
#define ESP_VERSION_NUMBER                                                  \
    ((uint32_t)ESP_VERSION_MAJOR << 16 | (uint32_t)ESP_VERSION_MINOR << 8 | \
     (uint32_t)ESP_VERSION_PATCH)

#define ESP_USER_AGENT_BASE   "SMOL CLOCK V2"
#define ESP_USER_AGENT_STRING ESP_USER_AGENT_BASE "/" ESP_VERSION

#endif // VERSION_H
