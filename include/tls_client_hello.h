#ifndef TLS_CLIENT_HELLO_H
#define TLS_CLIENT_HELLO_H

#include <stddef.h>
#include <stdint.h>

#ifndef TLS_CLIENT_HELLO_PARSER_ONLY
#include "compiler_port.h"
#include "core/net.h"
#include "core/socket.h"
#endif

#define TLS_CLIENT_HELLO_MAX_PEEK_SIZE 32768U

typedef enum
{
    TLS_CLIENT_HELLO_INCOMPLETE = 0,
    TLS_CLIENT_HELLO_NO_SNI,
    TLS_CLIENT_HELLO_SNI,
    TLS_CLIENT_HELLO_MALFORMED,
    TLS_CLIENT_HELLO_TOO_LARGE,
} tls_client_hello_result_t;

/**
 * Parse TLS records containing one ClientHello without consuming socket data.
 *
 * The scratch buffer is used to reassemble a ClientHello split across TLS
 * records. When more input is required, required_size contains the minimum
 * raw TLS byte count needed for the next parse attempt.
 */
tls_client_hello_result_t tls_client_hello_parse_sni(
    const uint8_t *records,
    size_t records_size,
    uint8_t *scratch,
    size_t scratch_size,
    char *hostname,
    size_t hostname_size,
    size_t *required_size);

/** Peek and parse a ClientHello using CycloneTCP's public socket API. */
#ifndef TLS_CLIENT_HELLO_PARSER_ONLY
error_t tls_client_hello_peek_sni(
    Socket *socket,
    char *hostname,
    size_t hostname_size,
    bool_t *sni_present,
    tls_client_hello_result_t *parse_result);
#endif

#endif
