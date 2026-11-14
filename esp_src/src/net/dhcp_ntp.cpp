#include "dhcp_ntp.h"

#include <esp_sntp.h>

void dhcp_ntp_init(void)
{
    esp_sntp_servermode_dhcp(true);
}


bool dhcp_ntp_server(IPAddress *address)
{
    const ip_addr_t *offered;
    ip_addr_t copy;

    // esp_sntp's own call (it takes the TCPIP core lock itself where it needs
    // it: not taken here, as the lock does not nest).
    offered = esp_sntp_getserver(0);
    if (offered != NULL)
    {
        copy = *offered;
    }
    if (offered == NULL || ip_addr_isany(&copy))
    {
        return false;
    }
    address->from_ip_addr_t(&copy);
    return true;
}
