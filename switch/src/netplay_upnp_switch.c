/* SPDX-License-Identifier: GPL-3.0-or-later */
/* netplay_upnp.c without miniupnpc: the Switch build asks no router for a
   port forward. A host can still be joined when its port is reachable. */
#include "netplay.h"

#include <stdio.h>

bool recomp_netplay_open_router_port(uint16_t port, char *address, size_t size)
{
    if (address != NULL && size != 0u) address[0] = '\0';
    fprintf(stderr, "recomp netplay: no UPnP on the Switch; forward UDP port %u by hand if needed\n",
        (unsigned)port);
    return false;
}

void recomp_netplay_close_router_port(void)
{
}
