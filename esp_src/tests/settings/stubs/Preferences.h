#ifndef PREFERENCES_H_STUB
#define PREFERENCES_H_STUB
#include <map>
#include <string>
#include <vector>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
// NVS as a map of key to bytes.
extern std::map<std::string, std::vector<unsigned char>> fake_nvs;
class Preferences
{
  public:
    bool begin(const char *, bool)
    {
        return true;
    }
    void end(void)
    {
    }
    size_t getBytesLength(const char *key)
    {
        auto found = fake_nvs.find(key);
        return found == fake_nvs.end() ? 0 : found->second.size();
    }
    size_t getBytes(const char *key, void *data, size_t size)
    {
        memcpy(data, fake_nvs[key].data(), size);
        return size;
    }
    size_t putBytes(const char *key, const void *data, size_t size)
    {
        fake_nvs[key].assign((const unsigned char *)data, (const unsigned char *)data + size);
        return size;
    }
    bool isKey(const char *key)
    {
        return fake_nvs.find(key) != fake_nvs.end();
    }
    int32_t getInt(const char *key, int32_t fallback)
    {
        int32_t value;

        if (getBytesLength(key) != sizeof(value))
        {
            return fallback;
        }
        memcpy(&value, fake_nvs[key].data(), sizeof(value));
        return value;
    }
    size_t putInt(const char *key, int32_t value)
    {
        return putBytes(key, &value, sizeof(value));
    }
};
#endif
