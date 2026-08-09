#include "tls_client_hello.h"

#include <ctype.h>
#include <stdbool.h>
#ifndef TLS_CLIENT_HELLO_PARSER_ONLY
#include <stdlib.h>
#endif
#include <string.h>

#ifndef TLS_CLIENT_HELLO_PARSER_ONLY
#include "error.h"
#endif

#define TLS_RECORD_HEADER_SIZE 5U
#define TLS_CONTENT_TYPE_HANDSHAKE 22U
#define TLS_HANDSHAKE_TYPE_CLIENT_HELLO 1U
#define TLS_EXTENSION_SERVER_NAME 0U
#define TLS_SERVER_NAME_TYPE_HOST_NAME 0U

static uint16_t tls_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t tls_read_u24(const uint8_t *data)
{
    return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}

static bool tls_hostname_is_valid(const uint8_t *hostname, size_t length)
{
    if (hostname == NULL || length == 0 || length > 253)
    {
        return false;
    }

    size_t label_length = 0;
    for (size_t index = 0; index < length; index++)
    {
        const uint8_t character = hostname[index];
        if (character == '.')
        {
            if (label_length == 0 || label_length > 63 || hostname[index - 1] == '-')
            {
                return false;
            }
            label_length = 0;
            continue;
        }

        if (!(isalnum((unsigned char)character) || character == '-'))
        {
            return false;
        }
        if (label_length == 0 && character == '-')
        {
            return false;
        }
        label_length++;
    }

    return label_length > 0 && label_length <= 63 && hostname[length - 1] != '-';
}

static tls_client_hello_result_t tls_parse_client_hello_extensions(
    const uint8_t *hello,
    size_t hello_size,
    char *hostname,
    size_t hostname_size)
{
    size_t offset = 4;
    if (hello_size < offset + 2 + 32 + 1)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }

    offset += 2 + 32;
    const size_t session_id_length = hello[offset++];
    if (session_id_length > 32 || offset + session_id_length + 2 > hello_size)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }
    offset += session_id_length;

    const size_t cipher_suites_length = tls_read_u16(&hello[offset]);
    offset += 2;
    if (cipher_suites_length == 0 || (cipher_suites_length & 1U) != 0 ||
        offset + cipher_suites_length + 1 > hello_size)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }
    offset += cipher_suites_length;

    const size_t compression_methods_length = hello[offset++];
    if (compression_methods_length == 0 || offset + compression_methods_length > hello_size)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }
    offset += compression_methods_length;

    if (offset == hello_size)
    {
        hostname[0] = '\0';
        return TLS_CLIENT_HELLO_NO_SNI;
    }
    if (offset + 2 > hello_size)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }

    const size_t extensions_length = tls_read_u16(&hello[offset]);
    offset += 2;
    if (offset + extensions_length != hello_size)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }

    const size_t extensions_end = offset + extensions_length;
    bool sni_seen = false;
    while (offset < extensions_end)
    {
        if (offset + 4 > extensions_end)
        {
            return TLS_CLIENT_HELLO_MALFORMED;
        }
        const uint16_t extension_type = tls_read_u16(&hello[offset]);
        const size_t extension_length = tls_read_u16(&hello[offset + 2]);
        offset += 4;
        if (offset + extension_length > extensions_end)
        {
            return TLS_CLIENT_HELLO_MALFORMED;
        }

        if (extension_type == TLS_EXTENSION_SERVER_NAME)
        {
            if (sni_seen || extension_length < 5)
            {
                return TLS_CLIENT_HELLO_MALFORMED;
            }
            sni_seen = true;

            const size_t name_list_length = tls_read_u16(&hello[offset]);
            if (name_list_length + 2 != extension_length)
            {
                return TLS_CLIENT_HELLO_MALFORMED;
            }

            size_t name_offset = offset + 2;
            const size_t name_end = name_offset + name_list_length;
            size_t name_count = 0;
            while (name_offset < name_end)
            {
                if (name_offset + 3 > name_end)
                {
                    return TLS_CLIENT_HELLO_MALFORMED;
                }
                const uint8_t name_type = hello[name_offset++];
                const size_t name_length = tls_read_u16(&hello[name_offset]);
                name_offset += 2;
                if (name_length == 0 || name_offset + name_length > name_end)
                {
                    return TLS_CLIENT_HELLO_MALFORMED;
                }
                name_count++;
                if (name_count != 1 || name_type != TLS_SERVER_NAME_TYPE_HOST_NAME ||
                    !tls_hostname_is_valid(&hello[name_offset], name_length) ||
                    name_length + 1 > hostname_size)
                {
                    return TLS_CLIENT_HELLO_MALFORMED;
                }
                memcpy(hostname, &hello[name_offset], name_length);
                hostname[name_length] = '\0';
                name_offset += name_length;
            }
            if (name_count != 1)
            {
                return TLS_CLIENT_HELLO_MALFORMED;
            }
        }

        offset += extension_length;
    }

    if (!sni_seen)
    {
        hostname[0] = '\0';
        return TLS_CLIENT_HELLO_NO_SNI;
    }
    return TLS_CLIENT_HELLO_SNI;
}

tls_client_hello_result_t tls_client_hello_parse_sni(
    const uint8_t *records,
    size_t records_size,
    uint8_t *scratch,
    size_t scratch_size,
    char *hostname,
    size_t hostname_size,
    size_t *required_size)
{
    if (records == NULL || scratch == NULL || hostname == NULL || hostname_size == 0 ||
        required_size == NULL || scratch_size < 4)
    {
        return TLS_CLIENT_HELLO_MALFORMED;
    }

    hostname[0] = '\0';
    *required_size = TLS_RECORD_HEADER_SIZE;
    size_t record_offset = 0;
    size_t handshake_size = 0;
    size_t expected_handshake_size = 0;

    while (expected_handshake_size == 0 || handshake_size < expected_handshake_size)
    {
        if (record_offset + TLS_RECORD_HEADER_SIZE > records_size)
        {
            *required_size = record_offset + TLS_RECORD_HEADER_SIZE;
            return *required_size > TLS_CLIENT_HELLO_MAX_PEEK_SIZE
                       ? TLS_CLIENT_HELLO_TOO_LARGE
                       : TLS_CLIENT_HELLO_INCOMPLETE;
        }
        if (records[record_offset] != TLS_CONTENT_TYPE_HANDSHAKE || records[record_offset + 1] != 3)
        {
            return TLS_CLIENT_HELLO_MALFORMED;
        }

        const size_t record_payload_size = tls_read_u16(&records[record_offset + 3]);
        if (record_payload_size == 0)
        {
            return TLS_CLIENT_HELLO_MALFORMED;
        }
        const size_t record_end = record_offset + TLS_RECORD_HEADER_SIZE + record_payload_size;
        if (record_end > TLS_CLIENT_HELLO_MAX_PEEK_SIZE)
        {
            return TLS_CLIENT_HELLO_TOO_LARGE;
        }
        if (record_end > records_size)
        {
            *required_size = record_end;
            return TLS_CLIENT_HELLO_INCOMPLETE;
        }
        if (handshake_size + record_payload_size > scratch_size)
        {
            return TLS_CLIENT_HELLO_TOO_LARGE;
        }

        memcpy(&scratch[handshake_size], &records[record_offset + TLS_RECORD_HEADER_SIZE], record_payload_size);
        handshake_size += record_payload_size;
        record_offset = record_end;

        if (handshake_size >= 4 && expected_handshake_size == 0)
        {
            if (scratch[0] != TLS_HANDSHAKE_TYPE_CLIENT_HELLO)
            {
                return TLS_CLIENT_HELLO_MALFORMED;
            }
            expected_handshake_size = 4U + tls_read_u24(&scratch[1]);
            if (expected_handshake_size > scratch_size || expected_handshake_size > TLS_CLIENT_HELLO_MAX_PEEK_SIZE)
            {
                return TLS_CLIENT_HELLO_TOO_LARGE;
            }
        }
    }

    return tls_parse_client_hello_extensions(scratch, expected_handshake_size, hostname, hostname_size);
}

#ifndef TLS_CLIENT_HELLO_PARSER_ONLY
error_t tls_client_hello_peek_sni(
    Socket *socket,
    char *hostname,
    size_t hostname_size,
    bool_t *sni_present,
    tls_client_hello_result_t *parse_result)
{
    if (socket == NULL || hostname == NULL || hostname_size == 0 || sni_present == NULL || parse_result == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint8_t *records = malloc(TLS_CLIENT_HELLO_MAX_PEEK_SIZE);
    uint8_t *scratch = malloc(TLS_CLIENT_HELLO_MAX_PEEK_SIZE);
    if (records == NULL || scratch == NULL)
    {
        free(records);
        free(scratch);
        return ERROR_OUT_OF_MEMORY;
    }

    size_t received = 0;
    error_t error = socketReceive(socket, records, TLS_CLIENT_HELLO_MAX_PEEK_SIZE, &received, SOCKET_FLAG_PEEK);
    size_t required_size = TLS_RECORD_HEADER_SIZE;
    if (!error)
    {
        *parse_result = tls_client_hello_parse_sni(records, received, scratch,
                                                   TLS_CLIENT_HELLO_MAX_PEEK_SIZE,
                                                   hostname, hostname_size, &required_size);
    }

    while (!error && *parse_result == TLS_CLIENT_HELLO_INCOMPLETE)
    {
        if (required_size > TLS_CLIENT_HELLO_MAX_PEEK_SIZE)
        {
            *parse_result = TLS_CLIENT_HELLO_TOO_LARGE;
            break;
        }
        error = socketReceive(socket, records, required_size, &received,
                              SOCKET_FLAG_PEEK | SOCKET_FLAG_WAIT_ALL);
        if (!error)
        {
            *parse_result = tls_client_hello_parse_sni(records, received, scratch,
                                                       TLS_CLIENT_HELLO_MAX_PEEK_SIZE,
                                                       hostname, hostname_size, &required_size);
        }
    }

    if (!error)
    {
        if (*parse_result == TLS_CLIENT_HELLO_SNI)
        {
            *sni_present = TRUE;
        }
        else if (*parse_result == TLS_CLIENT_HELLO_NO_SNI)
        {
            *sni_present = FALSE;
        }
        else
        {
            error = *parse_result == TLS_CLIENT_HELLO_TOO_LARGE
                        ? ERROR_BUFFER_OVERFLOW
                        : ERROR_INVALID_SYNTAX;
        }
    }

    free(records);
    free(scratch);
    return error;
}
#endif
