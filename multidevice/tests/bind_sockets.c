/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../lib/compat.h"
#include "../../lib/netutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr,"failed at line %d: %s\n",__LINE__,#value); exit(1); } } while (0)
static void check_socket(int control, int udp, const char *expected) {
    unsigned short port = 0;
    int fd = control ? netutils_init_control_socket(&port,0) : netutils_init_socket(&port,0,udp);
    CHECK(fd >= 0 && port > 0);
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    CHECK(getsockname(fd,(struct sockaddr *)&local,&length)==0);
    char address[INET_ADDRSTRLEN];
    CHECK(inet_ntop(AF_INET,&local.sin_addr,address,sizeof(address)));
    CHECK(!strcmp(address,expected));
    CLOSESOCKET(fd);
}
int main(int argc, char **argv) {
    const char *address = argc > 1 ? argv[1] : "127.0.0.1";
    CHECK(netutils_init()==0);
    CHECK(netutils_set_bind_address(address,0)==0);
    CHECK(netutils_ipv4_only());
    check_socket(0,0,address); check_socket(0,1,address); check_socket(1,0,address);
    unsigned short port=0;
    CHECK(netutils_init_socket(&port,1,0)==-1);
    CHECK(netutils_set_bind_address(address,1)==0);
    check_socket(0,0,address); check_socket(0,1,address); check_socket(1,0,"127.0.0.1");
    CHECK(netutils_set_bind_address("",0)==0);
    CHECK(!netutils_ipv4_only());
    check_socket(0,0,"0.0.0.0"); check_socket(1,0,"0.0.0.0");
    netutils_cleanup();
    puts("PASS selected TCP/UDP media address, private loopback control, IPv4-only, wildcard default");
    return 0;
}
