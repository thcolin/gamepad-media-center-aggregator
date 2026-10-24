// Force-included into libutp on Vita: vitasdk's <netinet/in.h> lacks
// IN6_IS_ADDR_V4MAPPED, which utp_packedsockaddr.cpp needs to compile.

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#ifndef IN6_IS_ADDR_V4MAPPED
#define IN6_IS_ADDR_V4MAPPED(a)                    \
    ((((const unsigned int*)(a))[0] == 0) &&       \
        (((const unsigned int*)(a))[1] == 0) &&    \
        (((const unsigned int*)(a))[2] == htonl(0xffff)))
#endif
