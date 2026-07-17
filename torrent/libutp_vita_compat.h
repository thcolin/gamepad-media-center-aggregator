/*
    GMCA — libutp portability shim for the PS Vita toolchain (vitasdk newlib).

    vitasdk's <netinet/in.h> declares `struct in6_addr` but omits the POSIX
    IN6_IS_ADDR_V4MAPPED macro that libutp's utp_packedsockaddr.cpp uses in
    PackedSockAddr::get_family(). We supply the canonical glibc/BSD definition so
    the VENDORED libutp source compiles unmodified. This is build glue, not a change
    to the vendored tree — it is force-included (-include) only on PLATFORM_PSV.

    The engine is IPv4-only (peers are always AF_INET, see socket.hpp), so this
    v4-mapped-IPv6 test is never actually taken at runtime; it only has to compile.
*/

#pragma once

#include <arpa/inet.h>   // htonl
#include <netinet/in.h>  // struct in6_addr

#ifndef IN6_IS_ADDR_V4MAPPED
#define IN6_IS_ADDR_V4MAPPED(a)                    \
    ((((const unsigned int*)(a))[0] == 0) &&       \
        (((const unsigned int*)(a))[1] == 0) &&    \
        (((const unsigned int*)(a))[2] == htonl(0xffff)))
#endif
