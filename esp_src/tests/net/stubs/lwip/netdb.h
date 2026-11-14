#ifndef LWIP_NETDB_H_STUB
#define LWIP_NETDB_H_STUB
// A resolver from a table; addresses kept as text in the sockaddr (see the
// IPAddress stub).
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
int lwip_getaddrinfo(const char *name, const char *service, const struct addrinfo *hints,
                     struct addrinfo **result);
void lwip_freeaddrinfo(struct addrinfo *info);
#endif
