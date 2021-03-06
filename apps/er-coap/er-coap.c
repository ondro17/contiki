/*
 * Copyright (c) 2013, Institute for Pervasive Computing, ETH Zurich
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 */

/**
 * \file
 *      An implementation of the Constrained Application Protocol (RFC).
 * \author
 *      Matthias Kovatsch <kovatsch@inf.ethz.ch>
 */

/**
* edited by Ondrej
*/

#include <string.h>
#include <stdio.h>
#include "contiki.h"
#include "sys/cc.h"
#include "contiki-net.h"
#include "lib/memb.h"

#include "er-coap.h"
#include "er-coap-transactions.h"
#include "heterogeneous-desider.h"

#define DEBUG 0
#if DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#define PRINT6ADDR(addr) PRINTF("[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]", ((uint8_t *)addr)[0], ((uint8_t *)addr)[1], ((uint8_t *)addr)[2], ((uint8_t *)addr)[3], ((uint8_t *)addr)[4], ((uint8_t *)addr)[5], ((uint8_t *)addr)[6], ((uint8_t *)addr)[7], ((uint8_t *)addr)[8], ((uint8_t *)addr)[9], ((uint8_t *)addr)[10], ((uint8_t *)addr)[11], ((uint8_t *)addr)[12], ((uint8_t *)addr)[13], ((uint8_t *)addr)[14], ((uint8_t *)addr)[15])
#define PRINTLLADDR(lladdr) PRINTF("[%02x:%02x:%02x:%02x:%02x:%02x]", (lladdr)->addr[0], (lladdr)->addr[1], (lladdr)->addr[2], (lladdr)->addr[3], (lladdr)->addr[4], (lladdr)->addr[5])
#else
#define PRINTF(...)
#define PRINT6ADDR(addr)
#define PRINTLLADDR(addr)
#endif


/*---------------------------------------------------------------------------*/
/*- Variables ---------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
static struct uip_udp_conn *udp_conn = NULL;
static uint16_t current_mid = 0;

coap_status_t erbium_status_code = NO_ERROR;
char *coap_error_message = "";

/*OPTION_METRIC: Used for storing added profiles*/
LIST(connection_list);
/*OPTION_METRIC: Declaration of memory*/
MEMB(connections_memb, struct connection_profiles, MAX_COUNT);

/*---------------------------------------------------------------------------*/
/*- Local helper functions --------------------------------------------------*/
/*---------------------------------------------------------------------------*/
static uint16_t
coap_log_2(uint16_t value)
{
  uint16_t result = 0;

  do {
    value = value >> 1;
    result++;
  } while(value);

  return result ? result - 1 : result;
}
/*---------------------------------------------------------------------------*/
static uint32_t
coap_parse_int_option(uint8_t *bytes, size_t length)
{
  uint32_t var = 0;
  int i = 0;

  while(i < length) {
    var <<= 8;
    var |= bytes[i++];
  }
  return var;
}
/*---------------------------------------------------------------------------*/
static uint8_t
coap_option_nibble(unsigned int value)
{
  if(value < 13) {
    return value;
  } else if(value <= 0xFF + 13) {
    return 13;
  } else {
    return 14;
  }
}
/*---------------------------------------------------------------------------*/
static size_t
coap_set_option_header(unsigned int delta, size_t length, uint8_t *buffer)
{
  size_t written = 0;

  buffer[0] = coap_option_nibble(delta) << 4 | coap_option_nibble(length);

  /* avoids code duplication without function overhead */
  unsigned int *x = &delta;

  do {
    if(*x > 268) {
      buffer[++written] = (*x - 269) >> 8;
      buffer[++written] = (*x - 269);
    } else if(*x > 12) {
      buffer[++written] = (*x - 13);
    }
  } while(x != &length && (x = &length));

  PRINTF("WRITTEN %u B opt header\n", 1 + written);

  return ++written;
}
/*---------------------------------------------------------------------------*/
static size_t
coap_serialize_int_option(unsigned int number, unsigned int current_number,
                          uint8_t *buffer, uint32_t value)
{
  size_t i = 0;

  if(0xFF000000 & value) {
    ++i;
  }
  if(0xFFFF0000 & value) {
    ++i;
  }
  if(0xFFFFFF00 & value) {
    ++i;
  }
  if(0xFFFFFFFF & value) {
    ++i;
  }
  PRINTF("OPTION %u (delta %u, len %u)\n", number, number - current_number,
         i);

  i = coap_set_option_header(number - current_number, i, buffer);

  if(0xFF000000 & value) {
    buffer[i++] = (uint8_t)(value >> 24);
  }
  if(0xFFFF0000 & value) {
    buffer[i++] = (uint8_t)(value >> 16);
  }
  if(0xFFFFFF00 & value) {
    buffer[i++] = (uint8_t)(value >> 8);
  }
  if(0xFFFFFFFF & value) {
    buffer[i++] = (uint8_t)(value);
  }
  return i;
}
/*---------------------------------------------------------------------------*/
static size_t
coap_serialize_array_option(unsigned int number, unsigned int current_number,
                            uint8_t *buffer, uint8_t *array, size_t length,
                            char split_char)
{
  size_t i = 0;

  PRINTF("ARRAY type %u, len %u, full [%.*s]\n", number, length, length,
         array);

  if(split_char != '\0') {
    int j;
    uint8_t *part_start = array;
    uint8_t *part_end = NULL;
    size_t temp_length;

    for(j = 0; j <= length + 1; ++j) {
      PRINTF("STEP %u/%u (%c)\n", j, length, array[j]);
      if(array[j] == split_char || j == length) {
        part_end = array + j;
        temp_length = part_end - part_start;

        i += coap_set_option_header(number - current_number, temp_length,
                                    &buffer[i]);
        memcpy(&buffer[i], part_start, temp_length);
        i += temp_length;

        PRINTF("OPTION type %u, delta %u, len %u, part [%.*s]\n", number,
               number - current_number, i, temp_length, part_start);

        ++j;                    /* skip the splitter */
        current_number = number;
        part_start = array + j;
      }
    }                           /* for */
  } else {
    i += coap_set_option_header(number - current_number, length, &buffer[i]);
    memcpy(&buffer[i], array, length);
    i += length;

    PRINTF("OPTION type %u, delta %u, len %u\n", number,
           number - current_number, length);
  }

  return i;
}
/*---------------------------------------------------------------------------*/
static void
coap_merge_multi_option(char **dst, size_t *dst_len, uint8_t *option,
                        size_t option_len, char separator)
{
  /* merge multiple options */
  if(*dst_len > 0) {
    /* dst already contains an option: concatenate */
    (*dst)[*dst_len] = separator;
    *dst_len += 1;

    /* memmove handles 2-byte option headers */
    memmove((*dst) + (*dst_len), option, option_len);

    *dst_len += option_len;
  } else {
    /* dst is empty: set to option */
    *dst = (char *)option;
    *dst_len = option_len;
  }
}
/*---------------------------------------------------------------------------*/
static int
coap_get_variable(const char *buffer, size_t length, const char *name,
                  const char **output)
{
  const char *start = NULL;
  const char *end = NULL;
  const char *value_end = NULL;
  size_t name_len = 0;

  /*initialize the output buffer first */
  *output = 0;

  name_len = strlen(name);
  end = buffer + length;

  for(start = buffer; start + name_len < end; ++start) {
    if((start == buffer || start[-1] == '&') && start[name_len] == '='
       && strncmp(name, start, name_len) == 0) {

      /* Point start to variable value */
      start += name_len + 1;

      /* Point end to the end of the value */
      value_end = (const char *)memchr(start, '&', end - start);
      if(value_end == NULL) {
        value_end = end;
      }
      *output = start;

      return value_end - start;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/*- Internal API ------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
void
coap_init_connection(uint16_t port)
{
  /* new connection with remote host */
  udp_conn = udp_new(NULL, 0, NULL);
  udp_bind(udp_conn, port);
  PRINTF("Listening on port %u\n", uip_ntohs(udp_conn->lport));

  /* initialize transaction ID */
  current_mid = random_rand();
}
/*---------------------------------------------------------------------------*/
uint16_t
coap_get_mid()
{
  return ++current_mid;
}
/*---------------------------------------------------------------------------*/
void
coap_init_message(void *packet, coap_message_type_t type, uint8_t code,
                  uint16_t mid)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  /* Important thing */
  memset(coap_pkt, 0, sizeof(coap_packet_t));

  coap_pkt->type = type;
  coap_pkt->code = code;
  coap_pkt->mid = mid;
}
/*---------------------------------------------------------------------------*/
size_t
coap_serialize_message(void *packet, uint8_t *buffer)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;
  uint8_t *option;
  unsigned int current_number = 0;

  /* Initialize */
  coap_pkt->buffer = buffer;
  coap_pkt->version = 1;

  PRINTF("-Serializing MID %u to %p, ", coap_pkt->mid, coap_pkt->buffer);

  /* set header fields */
  coap_pkt->buffer[0] = 0x00;
  coap_pkt->buffer[0] |= COAP_HEADER_VERSION_MASK
    & (coap_pkt->version) << COAP_HEADER_VERSION_POSITION;
  coap_pkt->buffer[0] |= COAP_HEADER_TYPE_MASK
    & (coap_pkt->type) << COAP_HEADER_TYPE_POSITION;
  coap_pkt->buffer[0] |= COAP_HEADER_TOKEN_LEN_MASK
    & (coap_pkt->token_len) << COAP_HEADER_TOKEN_LEN_POSITION;
  coap_pkt->buffer[1] = coap_pkt->code;
  coap_pkt->buffer[2] = (uint8_t)((coap_pkt->mid) >> 8);
  coap_pkt->buffer[3] = (uint8_t)(coap_pkt->mid);

  /* empty packet, dont need to do more stuff */
  if(!coap_pkt->code) {
    PRINTF("-Done serializing empty message at %p-\n", option);
    return 4;
  }

  /* set Token */
  PRINTF("Token (len %u)", coap_pkt->token_len);
  option = coap_pkt->buffer + COAP_HEADER_LEN;
  for(current_number = 0; current_number < coap_pkt->token_len;
      ++current_number) {
    PRINTF(" %02X", coap_pkt->token[current_number]);
    *option = coap_pkt->token[current_number];
    ++option;
  }
  PRINTF("-\n");

  /* Serialize options */
  current_number = 0;
  PRINTF("-Serializing options at %p-\n", option);

  /* The options must be serialized in the order of their number */
  COAP_SERIALIZE_BYTE_OPTION(COAP_OPTION_IF_MATCH, if_match, "If-Match");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_URI_HOST, uri_host, '\0',
                               "Uri-Host");
  COAP_SERIALIZE_BYTE_OPTION(COAP_OPTION_ETAG, etag, "ETag");
  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_IF_NONE_MATCH,
                            content_format -
                            coap_pkt->
                            content_format /* hack to get a zero field */,
                            "If-None-Match");

  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_OBSERVE, observe, "Observe");
  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_URI_PORT, uri_port, "Uri-Port");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_LOCATION_PATH, location_path, '/',
                               "Location-Path");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_URI_PATH, uri_path, '/',
                               "Uri-Path");
  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_CONTENT_FORMAT, content_format,
                            "Content-Format");
  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_MAX_AGE, max_age, "Max-Age");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_URI_QUERY, uri_query, '&',
                               "Uri-Query");
  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_ACCEPT, accept, "Accept");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_LOCATION_QUERY, location_query,
                               '&', "Location-Query");
  COAP_SERIALIZE_BLOCK_OPTION(COAP_OPTION_BLOCK2, block2, "Block2");
  COAP_SERIALIZE_BLOCK_OPTION(COAP_OPTION_BLOCK1, block1, "Block1");
  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_SIZE2, size2, "Size2");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_PROXY_URI, proxy_uri, '\0',
                               "Proxy-Uri");
  COAP_SERIALIZE_STRING_OPTION(COAP_OPTION_PROXY_SCHEME, proxy_scheme, '\0',
                               "Proxy-Scheme");

/*OPTION_METRIC: Serialize metric option*/
 COAP_SERIALIZE_BYTE_OPTION(COAP_OPTION_METRIC, metric, "Metrics");

  COAP_SERIALIZE_INT_OPTION(COAP_OPTION_SIZE1, size1, "Size1");
 


  PRINTF("-Done serializing at %p----\n", option);

  /* Pack payload */
  if((option - coap_pkt->buffer) <= COAP_MAX_HEADER_SIZE) {
    /* Payload marker */
    if(coap_pkt->payload_len) {
      *option = 0xFF;
      ++option;
    }
    memmove(option, coap_pkt->payload, coap_pkt->payload_len);
  } else {
    /* an error occurred: caller must check for !=0 */
    coap_pkt->buffer = NULL;
    coap_error_message = "Serialized header exceeds COAP_MAX_HEADER_SIZE";
    return 0;
  }

  PRINTF("-Done %u B (header len %u, payload len %u)-\n",
         coap_pkt->payload_len + option - buffer, option - buffer,
         coap_pkt->payload_len);

  PRINTF("Dump [0x%02X %02X %02X %02X  %02X %02X %02X %02X]\n",
         coap_pkt->buffer[0],
         coap_pkt->buffer[1],
         coap_pkt->buffer[2],
         coap_pkt->buffer[3],
         coap_pkt->buffer[4],
         coap_pkt->buffer[5], coap_pkt->buffer[6], coap_pkt->buffer[7]
         );

  return (option - buffer) + coap_pkt->payload_len; /* packet length */
}
/*---------------------------------------------------------------------------*/

void
coap_send_message(uip_ipaddr_t *addr, uint16_t port, uint8_t *data,
                  uint16_t length)
{
  /* configure connection to reply to client */
  uip_ipaddr_copy(&udp_conn->ripaddr, addr);
  udp_conn->rport = port;

   /*OPTION_METRIC: Changed function for sending*/
   heterogeneous_udp_sendto(udp_conn, data, length, addr, port);
  //uip_udp_packet_send(udp_conn, data, length);

  PRINTF("-sent UDP datagram (%u)-\n", length);

  /* restore server socket to allow data from any node */
  memset(&udp_conn->ripaddr, 0, sizeof(udp_conn->ripaddr));
  udp_conn->rport = 0;
}
/*---------------------------------------------------------------------------*/
coap_status_t
coap_parse_message(void *packet, uint8_t *data, uint16_t data_len)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  /* initialize packet */
  memset(coap_pkt, 0, sizeof(coap_packet_t));

  /* pointer to packet bytes */
  coap_pkt->buffer = data;

  /* parse header fields */
  coap_pkt->version = (COAP_HEADER_VERSION_MASK & coap_pkt->buffer[0])
    >> COAP_HEADER_VERSION_POSITION;
  coap_pkt->type = (COAP_HEADER_TYPE_MASK & coap_pkt->buffer[0])
    >> COAP_HEADER_TYPE_POSITION;
  coap_pkt->token_len =
    MIN(COAP_TOKEN_LEN,
        (COAP_HEADER_TOKEN_LEN_MASK & coap_pkt->
         buffer[0]) >> COAP_HEADER_TOKEN_LEN_POSITION);
  coap_pkt->code = coap_pkt->buffer[1];
  coap_pkt->mid = coap_pkt->buffer[2] << 8 | coap_pkt->buffer[3];

  if(coap_pkt->version != 1) {
    coap_error_message = "CoAP version must be 1";
    return BAD_REQUEST_4_00;
  }
	

  uint8_t *current_option = data + COAP_HEADER_LEN;

  memcpy(coap_pkt->token, current_option, coap_pkt->token_len);
  PRINTF("Token (len %u) [0x%02X%02X%02X%02X%02X%02X%02X%02X]\n",
         coap_pkt->token_len, coap_pkt->token[0], coap_pkt->token[1],
         coap_pkt->token[2], coap_pkt->token[3], coap_pkt->token[4],
         coap_pkt->token[5], coap_pkt->token[6], coap_pkt->token[7]
         );                     /*FIXME always prints 8 bytes */

  /* parse options */
  memset(coap_pkt->options, 0, sizeof(coap_pkt->options));
  current_option += coap_pkt->token_len;

  unsigned int option_number = 0;
  unsigned int option_delta = 0;
  size_t option_length = 0;

  while(current_option < data + data_len) {
    /* payload marker 0xFF, currently only checking for 0xF* because rest is reserved */
    if((current_option[0] & 0xF0) == 0xF0) {
      coap_pkt->payload = ++current_option;
      coap_pkt->payload_len = data_len - (coap_pkt->payload - data);

      /* also for receiving, the Erbium upper bound is REST_MAX_CHUNK_SIZE */
      if(coap_pkt->payload_len > REST_MAX_CHUNK_SIZE) {
        coap_pkt->payload_len = REST_MAX_CHUNK_SIZE;
        /* null-terminate payload */
      }
      coap_pkt->payload[coap_pkt->payload_len] = '\0';

      break;
    }

    option_delta = current_option[0] >> 4;
    option_length = current_option[0] & 0x0F;
    ++current_option;

    /* avoids code duplication without function overhead */
    unsigned int *x = &option_delta;

    do {
      if(*x == 13) {
        *x += current_option[0];
        ++current_option;
      } else if(*x == 14) {
        *x += 255;
        *x += current_option[0] << 8;
        ++current_option;
        *x += current_option[0];
        ++current_option;
      }
    } while(x != &option_length && (x = &option_length));

    option_number += option_delta;

    PRINTF("OPTION %u (delta %u, len %u): ", option_number, option_delta,
           option_length);

    SET_OPTION(coap_pkt, option_number);

    switch(option_number) {
    case COAP_OPTION_CONTENT_FORMAT:
      coap_pkt->content_format = coap_parse_int_option(current_option,
                                                       option_length);
      PRINTF("Content-Format [%u]\n", coap_pkt->content_format);
      break;
    case COAP_OPTION_MAX_AGE:
      coap_pkt->max_age = coap_parse_int_option(current_option,
                                                option_length);
      PRINTF("Max-Age [%lu]\n", coap_pkt->max_age);
      break;
    case COAP_OPTION_ETAG:
      coap_pkt->etag_len = MIN(COAP_ETAG_LEN, option_length);
      memcpy(coap_pkt->etag, current_option, coap_pkt->etag_len);
      PRINTF("ETag %u [0x%02X%02X%02X%02X%02X%02X%02X%02X]\n",
             coap_pkt->etag_len, coap_pkt->etag[0], coap_pkt->etag[1],
             coap_pkt->etag[2], coap_pkt->etag[3], coap_pkt->etag[4],
             coap_pkt->etag[5], coap_pkt->etag[6], coap_pkt->etag[7]
             );                 /*FIXME always prints 8 bytes */
      break;
    case COAP_OPTION_ACCEPT:
      coap_pkt->accept = coap_parse_int_option(current_option, option_length);
      PRINTF("Accept [%u]\n", coap_pkt->accept);
      break;
    case COAP_OPTION_IF_MATCH:
      /* TODO support multiple ETags */
      coap_pkt->if_match_len = MIN(COAP_ETAG_LEN, option_length);
      memcpy(coap_pkt->if_match, current_option, coap_pkt->if_match_len);
      PRINTF("If-Match %u [0x%02X%02X%02X%02X%02X%02X%02X%02X]\n",
             coap_pkt->if_match_len, coap_pkt->if_match[0],
             coap_pkt->if_match[1], coap_pkt->if_match[2],
             coap_pkt->if_match[3], coap_pkt->if_match[4],
             coap_pkt->if_match[5], coap_pkt->if_match[6],
             coap_pkt->if_match[7]
             ); /* FIXME always prints 8 bytes */
      break;
    case COAP_OPTION_IF_NONE_MATCH:
      coap_pkt->if_none_match = 1;
      PRINTF("If-None-Match\n");
      break;

    case COAP_OPTION_PROXY_URI:
#if COAP_PROXY_OPTION_PROCESSING
      coap_pkt->proxy_uri = (char *)current_option;
      coap_pkt->proxy_uri_len = option_length;
#endif
      PRINTF("Proxy-Uri NOT IMPLEMENTED [%.*s]\n", coap_pkt->proxy_uri_len,
             coap_pkt->proxy_uri);
      coap_error_message = "This is a constrained server (Contiki)";
      return PROXYING_NOT_SUPPORTED_5_05;
      break;
    case COAP_OPTION_PROXY_SCHEME:
#if COAP_PROXY_OPTION_PROCESSING
      coap_pkt->proxy_scheme = (char *)current_option;
      coap_pkt->proxy_scheme_len = option_length;
#endif
      PRINTF("Proxy-Scheme NOT IMPLEMENTED [%.*s]\n",
             coap_pkt->proxy_scheme_len, coap_pkt->proxy_scheme);
      coap_error_message = "This is a constrained server (Contiki)";
      return PROXYING_NOT_SUPPORTED_5_05;
      break;

    case COAP_OPTION_URI_HOST:
      coap_pkt->uri_host = (char *)current_option;
      coap_pkt->uri_host_len = option_length;
      PRINTF("Uri-Host [%.*s]\n", coap_pkt->uri_host_len, coap_pkt->uri_host);
      break;
    case COAP_OPTION_URI_PORT:
      coap_pkt->uri_port = coap_parse_int_option(current_option,
                                                 option_length);
      PRINTF("Uri-Port [%u]\n", coap_pkt->uri_port);
      break;
    case COAP_OPTION_URI_PATH:
      /* coap_merge_multi_option() operates in-place on the IPBUF, but final packet field should be const string -> cast to string */
      coap_merge_multi_option((char **)&(coap_pkt->uri_path),
                              &(coap_pkt->uri_path_len), current_option,
                              option_length, '/');
      PRINTF("Uri-Path [%.*s]\n", coap_pkt->uri_path_len, coap_pkt->uri_path);
      break;
    case COAP_OPTION_URI_QUERY:
      /* coap_merge_multi_option() operates in-place on the IPBUF, but final packet field should be const string -> cast to string */
      coap_merge_multi_option((char **)&(coap_pkt->uri_query),
                              &(coap_pkt->uri_query_len), current_option,
                              option_length, '&');
      PRINTF("Uri-Query [%.*s]\n", coap_pkt->uri_query_len,
             coap_pkt->uri_query);
      break;

    case COAP_OPTION_LOCATION_PATH:
      /* coap_merge_multi_option() operates in-place on the IPBUF, but final packet field should be const string -> cast to string */
      coap_merge_multi_option((char **)&(coap_pkt->location_path),
                              &(coap_pkt->location_path_len), current_option,
                              option_length, '/');
      PRINTF("Location-Path [%.*s]\n", coap_pkt->location_path_len,
             coap_pkt->location_path);
      break;
    case COAP_OPTION_LOCATION_QUERY:
      /* coap_merge_multi_option() operates in-place on the IPBUF, but final packet field should be const string -> cast to string */
      coap_merge_multi_option((char **)&(coap_pkt->location_query),
                              &(coap_pkt->location_query_len), current_option,
                              option_length, '&');
      PRINTF("Location-Query [%.*s]\n", coap_pkt->location_query_len,
             coap_pkt->location_query);
      break;

    case COAP_OPTION_OBSERVE:
      coap_pkt->observe = coap_parse_int_option(current_option,
                                                option_length);
      PRINTF("Observe [%lu]\n", coap_pkt->observe);
      break;
    case COAP_OPTION_BLOCK2:
      coap_pkt->block2_num = coap_parse_int_option(current_option,
                                                   option_length);
      coap_pkt->block2_more = (coap_pkt->block2_num & 0x08) >> 3;
      coap_pkt->block2_size = 16 << (coap_pkt->block2_num & 0x07);
      coap_pkt->block2_offset = (coap_pkt->block2_num & ~0x0000000F)
        << (coap_pkt->block2_num & 0x07);
      coap_pkt->block2_num >>= 4;
      PRINTF("Block2 [%lu%s (%u B/blk)]\n", coap_pkt->block2_num,
             coap_pkt->block2_more ? "+" : "", coap_pkt->block2_size);
      break;
    case COAP_OPTION_BLOCK1:
      coap_pkt->block1_num = coap_parse_int_option(current_option,
                                                   option_length);
      coap_pkt->block1_more = (coap_pkt->block1_num & 0x08) >> 3;
      coap_pkt->block1_size = 16 << (coap_pkt->block1_num & 0x07);
      coap_pkt->block1_offset = (coap_pkt->block1_num & ~0x0000000F)
        << (coap_pkt->block1_num & 0x07);
      coap_pkt->block1_num >>= 4;
      PRINTF("Block1 [%lu%s (%u B/blk)]\n", coap_pkt->block1_num,
             coap_pkt->block1_more ? "+" : "", coap_pkt->block1_size);
      break;
    case COAP_OPTION_SIZE2:
      coap_pkt->size2 = coap_parse_int_option(current_option, option_length);
      PRINTF("Size2 [%lu]\n", coap_pkt->size2);
      break;
    case COAP_OPTION_SIZE1:
      coap_pkt->size1 = coap_parse_int_option(current_option, option_length);
      PRINTF("Size1 [%lu]\n", coap_pkt->size1);
      break;
/*OPTION_METRIC: Deserialization of option metric*/
    case COAP_OPTION_METRIC:
      coap_pkt->metric_len = MIN(COAP_METRIC_LEN, option_length);
      memcpy(coap_pkt->metric, current_option, coap_pkt->metric_len);
      break;
    default:
      printf("unknown option number (%u)\n", option_number);
      /* check if critical (odd) */
      if(option_number & 1) {
        coap_error_message = "Unsupported critical option";
        return BAD_OPTION_4_02;
      }
    }

    current_option += option_length;
  }                             /* for */
  PRINTF("-Done parsing-------\n");

  return NO_ERROR;
}
/*---------------------------------------------------------------------------*/
/*- REST Engine API ---------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
int
coap_get_query_variable(void *packet, const char *name, const char **output)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(IS_OPTION(coap_pkt, COAP_OPTION_URI_QUERY)) {
    return coap_get_variable(coap_pkt->uri_query, coap_pkt->uri_query_len,
                             name, output);
  }
  return 0;
}
int
coap_get_post_variable(void *packet, const char *name, const char **output)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(coap_pkt->payload_len) {
    return coap_get_variable((const char *)coap_pkt->payload,
                             coap_pkt->payload_len, name, output);
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
int
coap_set_status_code(void *packet, unsigned int code)
{
  if(code <= 0xFF) {
    ((coap_packet_t *)packet)->code = (uint8_t)code;
    return 1;
  } else {
    return 0;
  }
}
/*---------------------------------------------------------------------------*/
int
coap_set_token(void *packet, const uint8_t *token, size_t token_len)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->token_len = MIN(COAP_TOKEN_LEN, token_len);
  memcpy(coap_pkt->token, token, coap_pkt->token_len);

  return coap_pkt->token_len;
}
/*---------------------------------------------------------------------------*/
/*- CoAP REST Implementation API --------------------------------------------*/
/*---------------------------------------------------------------------------*/
int
coap_get_header_content_format(void *packet, unsigned int *format)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_CONTENT_FORMAT)) {
    return 0;
  }
  *format = coap_pkt->content_format;
  return 1;
}
int
coap_set_header_content_format(void *packet, unsigned int format)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->content_format = (coap_content_format_t)format;
  SET_OPTION(coap_pkt, COAP_OPTION_CONTENT_FORMAT);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_accept(void *packet, unsigned int *accept)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_ACCEPT)) {
    return 0;
  }
  *accept = coap_pkt->accept;
  return 1;
}
int
coap_set_header_accept(void *packet, unsigned int accept)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->accept = (coap_content_format_t)accept;
  SET_OPTION(coap_pkt, COAP_OPTION_ACCEPT);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_max_age(void *packet, uint32_t *age)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_MAX_AGE)) {
    *age = COAP_DEFAULT_MAX_AGE;
  } else {
    *age = coap_pkt->max_age;
  } return 1;
}
int
coap_set_header_max_age(void *packet, uint32_t age)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->max_age = age;
  SET_OPTION(coap_pkt, COAP_OPTION_MAX_AGE);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_etag(void *packet, const uint8_t **etag)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_ETAG)) {
    return 0;
  }
  *etag = coap_pkt->etag;
  return coap_pkt->etag_len;
}
int
coap_set_header_etag(void *packet, const uint8_t *etag, size_t etag_len)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->etag_len = MIN(COAP_ETAG_LEN, etag_len);
  memcpy(coap_pkt->etag, etag, coap_pkt->etag_len);

  SET_OPTION(coap_pkt, COAP_OPTION_ETAG);
  return coap_pkt->etag_len;
}
/*---------------------------------------------------------------------------*/
/*FIXME support multiple ETags */
int
coap_get_header_if_match(void *packet, const uint8_t **etag)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_IF_MATCH)) {
    return 0;
  }
  *etag = coap_pkt->if_match;
  return coap_pkt->if_match_len;
}
int
coap_set_header_if_match(void *packet, const uint8_t *etag, size_t etag_len)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->if_match_len = MIN(COAP_ETAG_LEN, etag_len);
  memcpy(coap_pkt->if_match, etag, coap_pkt->if_match_len);

  SET_OPTION(coap_pkt, COAP_OPTION_IF_MATCH);
  return coap_pkt->if_match_len;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_if_none_match(void *packet)
{
  return IS_OPTION((coap_packet_t *)packet,
                   COAP_OPTION_IF_NONE_MATCH) ? 1 : 0;
}
int
coap_set_header_if_none_match(void *packet)
{
  SET_OPTION((coap_packet_t *)packet, COAP_OPTION_IF_NONE_MATCH);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_proxy_uri(void *packet, const char **uri)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_PROXY_URI)) {
    return 0;
  }
  *uri = coap_pkt->proxy_uri;
  return coap_pkt->proxy_uri_len;
}
int
coap_set_header_proxy_uri(void *packet, const char *uri)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  /*TODO Provide alternative that sets Proxy-Scheme and Uri-* options and provide er-coap-conf define */

  coap_pkt->proxy_uri = uri;
  coap_pkt->proxy_uri_len = strlen(uri);

  SET_OPTION(coap_pkt, COAP_OPTION_PROXY_URI);
  return coap_pkt->proxy_uri_len;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_uri_host(void *packet, const char **host)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_URI_HOST)) {
    return 0;
  }
  *host = coap_pkt->uri_host;
  return coap_pkt->uri_host_len;
}
int
coap_set_header_uri_host(void *packet, const char *host)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->uri_host = host;
  coap_pkt->uri_host_len = strlen(host);

  SET_OPTION(coap_pkt, COAP_OPTION_URI_HOST);
  return coap_pkt->uri_host_len;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_uri_path(void *packet, const char **path)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_URI_PATH)) {
    return 0;
  }
  *path = coap_pkt->uri_path;
  return coap_pkt->uri_path_len;
}
int
coap_set_header_uri_path(void *packet, const char *path)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;
  while(path[0] == '/')
    ++path;
  coap_pkt->uri_path = path;
  coap_pkt->uri_path_len = strlen(path);

  SET_OPTION(coap_pkt, COAP_OPTION_URI_PATH);
  return coap_pkt->uri_path_len;
}
/*---------------------------OPTION_METRIC: Implementation starts here-------------------------------------*/

/** 
 * Function to get metric array from parsed packet
 */
int
coap_get_metrics(void *packet, const int **metric)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;
  if(!IS_OPTION(coap_pkt, COAP_OPTION_METRIC)) {
    return 0;
  }
  *metric = coap_pkt->metric;
  return coap_pkt->metric_len;
}

/** 
 * Function to set metric array to packet
 */
int 
coap_set_metrics(void *packet, const uint8_t *metric, size_t metric_len)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;
  coap_pkt->metric_len = MIN(COAP_OPTION_METRIC, metric_len);
  memcpy(coap_pkt->metric, metric, coap_pkt->metric_len);
  SET_OPTION(coap_pkt, COAP_OPTION_METRIC);
  return coap_pkt->metric_len;
}

/** 
 * Function to get metric values from packet
 */
struct k_val coap_get_k_val(uint8_t *data, uint16_t data_len)
{
  static coap_packet_t message[1];
  uint8_t buffer[REST_MAX_CHUNK_SIZE];
  memcpy(buffer, data, data_len);
  uint8_t ii;
  struct k_val values;
  uint8_t *p;
  p=&values;
  for(ii=1; ii<10; ii++){
    *p=0;
    p=p+1;
  }
  PRINTF("coap_get_val data with len: %d\n", data_len);
  uint8_t i=0;
  for (i = 0; i < data_len; i++)
  {
    unsigned char c = ((char*)data)[i] ;
    PRINTF ("%02x ", c) ;
  }
  PRINTF("\n");
  erbium_status_code = NO_ERROR;
  erbium_status_code = coap_parse_message(message, buffer, data_len);
    if(erbium_status_code == NO_ERROR) {
      if(!IS_OPTION(message, COAP_OPTION_METRIC)) {
        printf("No metric option!\n");
        return values;
      }
      return coap_metrics_deserialization(&message->metric);
  }
  else
    printf("Erbium error!!!\n");
    return values;

}

/** 
 * Function to get metric values into k_val struct from metric array
 */
struct k_val coap_metrics_deserialization(uint8_t *metrics){
  PRINTF("Coap metric deserialization\n");
  PRINTF("1st flag number: %d\n",metrics[0]);
  PRINTF("2nd flag number: %d\n",metrics[1]);
  uint8_t numbers[8] = {128, 64, 32, 16, 8, 4, 2, 1};
  uint8_t ii;
  struct k_val values;
  uint8_t *p;
  p=&values;
  for(ii=1; ii<10; ii++){
    *p=0;
    p=p+1;
  }
  uint8_t jj=2;
  p=&values;
  metrics[0]=metrics[0]-64; //verzia
  for(ii=2; ii<11; ii++){
    if(metrics[(ii / 8)]>=numbers[ii % 8]){
      metrics[(ii / 8)]=metrics[(ii / 8)]-numbers[ii % 8];
      *p=metrics[jj];  
      jj++;
    }
    p=p+1;
  }
  p=&values;
  for(ii=1; ii<10; ii++){
    PRINTF("%d\n", *p);
    p=p+1;
  }
  return values;
}

/** 
 * Function to create byte array from connection_profile struct
 */
void coap_metrics_serialization(void *packet, struct connection_profiles *c){
  uint8_t numbers[8] = {128, 64, 32, 16, 8, 4, 2, 1};
  uint8_t i=2;
  uint8_t metrics[12];
  metrics[0]=0;
  metrics[1]=0;
  metrics[0]=metrics[0]+64; //ver1
  uint8_t *p;
  p=&(c->bandwidth);
  uint8_t ii=1;
  uint8_t jj=2;
  for(ii=1; ii<10; ii++){
    if (ii==7)
      jj=0;
    if(*p!=0){
      metrics[i]=*p;
      i++;
      if (ii<7){
        metrics[0]=metrics[0]+numbers[jj];
      }else{
        metrics[1]=metrics[1]+numbers[jj];
      }    
    }   
  jj++;
  p=p+1;
  }
  PRINTF("Metrics serialized!\n");
  coap_set_metrics(packet, metrics, i);
}

/** 
 * Function to set profile (metrics) into packet
 */
void coap_set_profile(const char *resource_url,void *packet, uip_ipaddr_t *server_ipaddr){
  struct connection_profiles *c = NULL;   
  for(c = list_head(connection_list); c != NULL; c = list_item_next(c)) {
    if ((uip_ipaddr_cmp(&c->server_ipaddr, server_ipaddr)==1)&&(strcmp(c->resource_url,resource_url)==0)){
      break;
    }
  }
  if (c==NULL)
    printf("No record for this combination url:IP address!\n");
  else
    coap_metrics_serialization(packet, c);
  }

/** 
 * Function to change profile priority
 */
/*if direction==0 -> decrease if direction==1 -> increase*/
int coap_change_profile_priority(const char *resource_url, unsigned int profile, uip_ipaddr_t *server_ipaddr, uint8_t direction) {
  if (check_number_profile(profile)==1)
    return 1;
  struct connection_profiles *c = NULL;   
  if (direction==1)
    PRINTF("Increase profile %s\n",resource_url);
  else
    PRINTF("Decrease profile %s\n",resource_url);
    for(c = list_head(connection_list); c != NULL; c = list_item_next(c)) {
      if ((uip_ipaddr_cmp(&c->server_ipaddr, server_ipaddr)==1)&&(strcmp(c->resource_url,resource_url)==0)){
	break;
      }
    }
    if (c==NULL){
      printf("Non-existing record\n");
      return 1;   
    }
    else{
      if (change_profile_metric(profile, c, direction)==0)
        return 0;
      else
        return 1;
    }
}

/** 
 * Function to save profiles for connection IP:URL
 */
int coap_add_profile(const char *resource_url, unsigned int profile1, unsigned int profile2, int equal, uip_ipaddr_t server_ipaddr) {
  if (check_number_profile(profile1)==1){
    printf("Wrong 1st profile\n");
    return 1;
  }
  if ((check_number_profile(profile2)==1) && (profile2!=0)){
    printf("Wrong 2nd profile\n");
    return 1;
  }
  struct connection_profiles *c = NULL;   
  for(c = list_head(connection_list); c != NULL; c = list_item_next(c)) {
    if ((uip_ipaddr_cmp(&c->server_ipaddr, &server_ipaddr)==1)&&(strcmp(c->resource_url,resource_url)==0)){
      break;
    }
  }
  if (c==NULL){
    c = memb_alloc(&connections_memb);
    if (c==NULL) {
      printf("Maximum capacity of profiles exceeded\n");
      return 1;
    }
    else{
      PRINTF("Writing new profile\n");
        c->server_ipaddr = server_ipaddr;
        c->resource_url = resource_url;
        c->profile1 = profile1;
        c->profile2 = profile2;
        pair_profile_metric(profile1, profile2, equal, c);
        list_add(connection_list, c);
      }
    }
    else{
      PRINTF("Rewriting profile entry\n");
      c->server_ipaddr = server_ipaddr;
      c->resource_url = resource_url;
      c->profile1 = profile1;
      c->profile2 = profile2;
      pair_profile_metric(profile1, profile2, equal, c);
    }
  return 0;
}

/** 
 * Function to inicialize connection_profile values and set metric of profile
 */      
void pair_profile_metric(unsigned int profile1, unsigned int profile2, int equal, struct connection_profiles *c){  
  c->bandwidth=0;
  c->rem_energy=0;
  c->etx=0;
  c->power_saving=0;
  c->quality=0;
  c->rtt=0;
  c->efx=0;
  c->link_load=0;
  c->security=0;
  if (profile2 == 0){   
    PRINTF("Setting one profile (%d) with value %d\n", profile1, COAP_OPTION_METRIC_EQUAL_PROFILE);
    set_one_profile(profile1, c, COAP_OPTION_METRIC_EQUAL_PROFILE );
  } else {
    if (equal){
      PRINTF("Setting two equal profiles (%d) and (%d) with value %d\n",profile1, profile2, COAP_OPTION_METRIC_EQUAL_PROFILE);
      set_one_profile(profile1, c, COAP_OPTION_METRIC_EQUAL_PROFILE );
      set_one_profile(profile2, c, COAP_OPTION_METRIC_EQUAL_PROFILE );
    } else {
      PRINTF("Setting ordered profiles (%d) and (%d) with values %d and %d\n",profile1, profile2, COAP_OPTION_METRIC_HIGHER_PROFILE, COAP_OPTION_METRIC_LOWER_PROFILE);
      set_one_profile(profile1, c, COAP_OPTION_METRIC_HIGHER_PROFILE );
      set_one_profile(profile2, c, COAP_OPTION_METRIC_LOWER_PROFILE );
    }
  }
}

/** 
 * Function to set correct pointers to metrics
 */
int set_pointer_to_metric(unsigned int profile, struct connection_profiles *c, uint8_t **p1, uint8_t **p2){
  switch(profile) {
    case PROFILE_LOWPOWER:
      PRINTF("Setting pointers for LOWPOWER\n");
      *p1=&(c->rem_energy);
      *p2=&(c->power_saving);
      break;
    case PROFILE_SPEED :
      PRINTF("Setting pointers for SPEED\n"); 
      *p1=&(c->bandwidth);
      *p2=&(c->link_load);
      break;
    case PROFILE_RELIABILITY:
      PRINTF("Setting pointers for RELIABILITY\n");
      *p1=&(c->etx);
      *p2=&(c->efx);
      break;
    case PROFILE_MULTIMEDIA:
      PRINTF("Setting pointers for MULTIMEDIA\n");
      *p1=&(c->rtt);
      *p2=&(c->quality);
      break;
   case PROFILE_SECURITY:
      PRINTF("Setting pointers for SECURITY\n");
      *p1=&(c->security);
      *p2=&(c->security);
      break;
   case 0:
      printf("No profile\n");
      return 1;
   default:
      printf("set_pointer_to_metric unknown profile: (%u)\n", profile);
      return 1;
    }
  return 0;
}

/** 
 * Function provides control of profile number
 */
int check_number_profile(unsigned int profile){
  switch(profile) {
    case PROFILE_LOWPOWER:
      break;
    case PROFILE_SPEED :
      break;
    case PROFILE_RELIABILITY:
      break;
    case PROFILE_MULTIMEDIA:
      break;
   case PROFILE_SECURITY:
      break;
   default:
      //printf("Unknown profile: (%u)\n", profile);
      return 1;
  }
  return 0;
}

/** 
 * Function is responsible for increasing or decreasing correct profile
 */
int change_profile_metric(unsigned int profile, struct connection_profiles *c, uint8_t direction){
  int increaser=0;
  int decreaser=0;
  if (direction==0){ //decrease
    increaser=COAP_OPTION_METRIC_DECREASE_VALUE;
    decreaser=COAP_OPTION_METRIC_INCREASE_VALUE;
  }
  else//increase
  {
    increaser=COAP_OPTION_METRIC_INCREASE_VALUE;
    decreaser=COAP_OPTION_METRIC_DECREASE_VALUE;
  }
     if ((c->profile1!=profile)&&(c->profile2!=profile)){
       printf("Wrong profile: (%u)\n", profile);
       return 1;
     } else if (c->profile1==profile){
       if (change_one_profile(c->profile1, c, increaser)!=0){
         PRINTF("First try - cannot change profile: (%d)\n",c->profile1);
         if (change_one_profile(c->profile2, c, decreaser)!=0){
            printf("Cannot change metric of both profiles\n");
            return 1;
         }
       }
     }else{
       if (change_one_profile(c->profile2, c, increaser)!=0){
         PRINTF("First try - cannot change profile: (%d)\n",c->profile2);
         if (change_one_profile(c->profile1, c, decreaser)!=0){
            printf("Cannot change metric of both profiles\n");
            return 1;
         }
       }
     } 
  return 0;
}

/** 
 * computing and setting new metric value
 */
int change_one_profile(unsigned int profile, struct connection_profiles *c, int value){ 
  uint8_t *p2, *p1;
  //if (set_pointer_to_metric(profile, c, &p1, &p2)!=0)
  if (profile==0)
    return 1;
  set_pointer_to_metric(profile, c, &p1, &p2);
  if((*p1==1) && (value>0)){
    *p1=(uint8_t)0;
  }
  if((*p1==255) && (value<0)){
    value++;
  }
  int a = (int)*p1+(int)value;
  PRINTF("Computing new_metric: %d = %d + %d\n",a,*p1,value);
  switch(a) {
    case 0:
      *p1=(uint8_t)1;
      *p2=(uint8_t)1;
      return 0;
    case 256 :
      *p1=(uint8_t)255;
      *p2=(uint8_t)255;
      return 0;
    default:
      if(a<0)
        return 1;
      else if (a>256)
        return 1;
      else{
        *p1=(uint8_t)a;
        *p2=(uint8_t)a;
        return 0;
      }
  }
}

/** 
 * assign of values to pointers
 */
void set_one_profile(unsigned int profile, struct connection_profiles *c, int value){ 
  uint8_t *p2, *p1;
  set_pointer_to_metric(profile, c, &p1, &p2);
  *p1=value;
  *p2=value;
}

/*---------------------------OPTION_METRIC: Implementation ends here-------------------------------------*/

int
coap_get_header_uri_query(void *packet, const char **query)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_URI_QUERY)) {
    return 0;
  }
  *query = coap_pkt->uri_query;
  return coap_pkt->uri_query_len;
}
int
coap_set_header_uri_query(void *packet, const char *query)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  while(query[0] == '?')
    ++query;

  coap_pkt->uri_query = query;
  coap_pkt->uri_query_len = strlen(query);

  SET_OPTION(coap_pkt, COAP_OPTION_URI_QUERY);
  return coap_pkt->uri_query_len;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_location_path(void *packet, const char **path)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_LOCATION_PATH)) {
    return 0;
  }
  *path = coap_pkt->location_path;
  return coap_pkt->location_path_len;
}
int
coap_set_header_location_path(void *packet, const char *path)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  char *query;

  while(path[0] == '/')
    ++path;

  if((query = strchr(path, '?'))) {
    coap_set_header_location_query(packet, query + 1);
    coap_pkt->location_path_len = query - path;
  } else {
    coap_pkt->location_path_len = strlen(path);
  } coap_pkt->location_path = path;

  if(coap_pkt->location_path_len > 0) {
    SET_OPTION(coap_pkt, COAP_OPTION_LOCATION_PATH);
  }
  return coap_pkt->location_path_len;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_location_query(void *packet, const char **query)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_LOCATION_QUERY)) {
    return 0;
  }
  *query = coap_pkt->location_query;
  return coap_pkt->location_query_len;
}
int
coap_set_header_location_query(void *packet, const char *query)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  while(query[0] == '?')
    ++query;

  coap_pkt->location_query = query;
  coap_pkt->location_query_len = strlen(query);

  SET_OPTION(coap_pkt, COAP_OPTION_LOCATION_QUERY);
  return coap_pkt->location_query_len;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_observe(void *packet, uint32_t *observe)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_OBSERVE)) {
    return 0;
  }
  *observe = coap_pkt->observe;
  return 1;
}
int
coap_set_header_observe(void *packet, uint32_t observe)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->observe = observe;
  SET_OPTION(coap_pkt, COAP_OPTION_OBSERVE);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_block2(void *packet, uint32_t *num, uint8_t *more,
                       uint16_t *size, uint32_t *offset)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_BLOCK2)) {
    return 0;
  }
  /* pointers may be NULL to get only specific block parameters */
  if(num != NULL) {
    *num = coap_pkt->block2_num;
  }
  if(more != NULL) {
    *more = coap_pkt->block2_more;
  }
  if(size != NULL) {
    *size = coap_pkt->block2_size;
  }
  if(offset != NULL) {
    *offset = coap_pkt->block2_offset;
  }
  return 1;
}
int
coap_set_header_block2(void *packet, uint32_t num, uint8_t more,
                       uint16_t size)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(size < 16) {
    return 0;
  }
  if(size > 2048) {
    return 0;
  }
  if(num > 0x0FFFFF) {
    return 0;
  }
  coap_pkt->block2_num = num;
  coap_pkt->block2_more = more ? 1 : 0;
  coap_pkt->block2_size = size;

  SET_OPTION(coap_pkt, COAP_OPTION_BLOCK2);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_block1(void *packet, uint32_t *num, uint8_t *more,
                       uint16_t *size, uint32_t *offset)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_BLOCK1)) {
    return 0;
  }
  /* pointers may be NULL to get only specific block parameters */
  if(num != NULL) {
    *num = coap_pkt->block1_num;
  }
  if(more != NULL) {
    *more = coap_pkt->block1_more;
  }
  if(size != NULL) {
    *size = coap_pkt->block1_size;
  }
  if(offset != NULL) {
    *offset = coap_pkt->block1_offset;
  }
  return 1;
}
int
coap_set_header_block1(void *packet, uint32_t num, uint8_t more,
                       uint16_t size)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(size < 16) {
    return 0;
  }
  if(size > 2048) {
    return 0;
  }
  if(num > 0x0FFFFF) {
    return 0;
  }
  coap_pkt->block1_num = num;
  coap_pkt->block1_more = more;
  coap_pkt->block1_size = size;

  SET_OPTION(coap_pkt, COAP_OPTION_BLOCK1);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_size2(void *packet, uint32_t *size)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_SIZE2)) {
    return 0;
  }
  *size = coap_pkt->size2;
  return 1;
}
int
coap_set_header_size2(void *packet, uint32_t size)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->size2 = size;
  SET_OPTION(coap_pkt, COAP_OPTION_SIZE2);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_header_size1(void *packet, uint32_t *size)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(!IS_OPTION(coap_pkt, COAP_OPTION_SIZE1)) {
    return 0;
  }
  *size = coap_pkt->size1;
  return 1;
}
int
coap_set_header_size1(void *packet, uint32_t size)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->size1 = size;
  SET_OPTION(coap_pkt, COAP_OPTION_SIZE1);
  return 1;
}
/*---------------------------------------------------------------------------*/
int
coap_get_payload(void *packet, const uint8_t **payload)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  if(coap_pkt->payload) {
    *payload = coap_pkt->payload;
    return coap_pkt->payload_len;
  } else {
    *payload = NULL;
    return 0;
  }
}
int
coap_set_payload(void *packet, const void *payload, size_t length)
{
  coap_packet_t *const coap_pkt = (coap_packet_t *)packet;

  coap_pkt->payload = (uint8_t *)payload;
  coap_pkt->payload_len = MIN(REST_MAX_CHUNK_SIZE, length);

  return coap_pkt->payload_len;
}
/*---------------------------------------------------------------------------*/
