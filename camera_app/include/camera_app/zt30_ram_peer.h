#pragma once
#include <netinet/in.h>
// Fixed local Ethernet test host plus the development web service on loopback.
static inline bool ca_zt30_ram_peer(const sockaddr *p) {
    if(!p || p->sa_family!=AF_INET)return false;
    const uint32_t address=ntohl(reinterpret_cast<const sockaddr_in*>(p)->sin_addr.s_addr);
    return address==0xc0a8900fU || address==0x7f000001U;
}
