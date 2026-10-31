/*
MBEDTLS_CONFIG_PS5.H

The PS5's additions to Mbed TLS's configuration (port/third_party/mbedtls,
which is not edited): tools/ps5_build.py gives this file to the library and
to posix_dtls.c as MBEDTLS_USER_CONFIG_FILE, which build_info.h includes
after mbedtls_config.h.

Mbed TLS's own entropy source reads /dev/urandom or calls getrandom, which a
title has neither of; its random numbers come from the console's
arc4random_buf instead, through the hardware source (mbedtls_hardware_poll,
port/ps5/host/host_net_ps5.c).
*/

#ifndef PS5_MBEDTLS_CONFIG_H
#define PS5_MBEDTLS_CONFIG_H

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#endif
