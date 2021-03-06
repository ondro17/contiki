/*
 * Copyright (c) 2017, Tomas Hermanek.
 * All rights reserved.
 */

#define DEBUG 0
#include "net/ip/uip-debug.h"

#include "serial-connection.h"
#include "heterogeneous-desider.h"

#include "contiki.h"
#include "dev/serial-line.h"
#include "net/ip/simple-udp.h"
#include "net/ip/uip.h"
#include "lib/memb.h"
#include "lib/list.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>


static uint8_t payload[70];       // ToDo refactor buff size -> size must be identical with uip_buff - headers
uint16_t sport, dport;
static short payload_len;
static uip_ipaddr_t sender_ip, receiver_ip;
struct simple_udp_connection *c = NULL;

int forwarding_semafor = 0;

void clear_buff() {
    payload_len = 0;
    sport = 0;
    dport = 0;
    payload[0] = 0;
}

/**
 * Converts character to real hexadecimal value
 *
 * @param c
 * @return
 */
static uint8_t unit_from_char(char c)
{
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= '0' && c <= '9') return c - '0';
    return 0;
}

/**
 * Fills payload buffer with real value converted from hexadecimal string
 *
 * @param hex_string
 */
void hex_string_to_value(char *hex_string)
{
    uint8_t *result;
    uint8_t *p, len, i;

    len = strlen(hex_string) / 2;

    for(i=0, p = (uint8_t *) hex_string; i<len; i++) {
        payload[i] = (unit_from_char(*p) << 4) | unit_from_char(*(p+1));
        p += 2;
    }
    payload_len = len;
    return result;
}

/**
 * Parses IPv6 string address and returns uip_ipaddr_t structure, this function is depreciated because, it was created
 * for parsing IPv6 address in extended notation and lately was extended to support compressed address too. It should be
 * refactored for better performance
 *
 * @param ip_string
 * @return
 */
uip_ipaddr_t *string_ipv6_to_uip_ipaddr(char *ip_string, uip_ipaddr_t *ipv6) {
    uint16_t ip[8];
    char *actual = ip_string;

    uint8_t bool_sep = 0,left = 0, right = 0, i, len = strlen(ip_string);

    for (i = 0; i <= len; i++) {
        if (ip_string[i] == ':' && ip_string[i+1] == ':') {
            bool_sep = 1;
        } else if (ip_string[i] == ':') {
            if (bool_sep == 0)
                left++;
            else
                right++;
        }
    }
    right = 8 - right;

    for (i = left + 1; i <= right-1; i++) {
        ip[i] = 0;
    }

    char *end_str, *token = strtok_r(ip_string, ":", &end_str);
    i = 0;

    while (token != NULL) {
        uint16_t number = (uint16_t)strtol(token, NULL, 16);
        token = strtok_r(NULL, ":", &end_str);

        if (i <= left) {
            ip[i] = number;
            i++;
        }
        else {
            ip[right] = number;
            right++;
        }
    }

    uip_ip6addr(ipv6, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
    return ipv6;
}

/**
 * Parses incoming packet, which is in format <cmd>;<srcIp>;<dstIp>;<udp.sport>;<udp.dport>;<payload>
 *
 * @param data
 * @param len
 * @return
 */
uint8_t parse_incoming_packet(char *data, int len, uint16_t *sport, uint16_t *dport, struct simple_udp_connection *c,
                            char *payload, int *payload_len, uip_ipaddr_t *sender_ip, uip_ipaddr_t *receiver_ip, int i) {
    char *end_str, *token = strtok_r(data, ";", &end_str);
    uint8_t meta;

    while (token != NULL)  {
        switch (i) {
            case 0:
                meta =  strtol(token, &token, 10);
                break;
            case 1:
                string_ipv6_to_uip_ipaddr(token, sender_ip);
                break;
            case 2:
                string_ipv6_to_uip_ipaddr(token, receiver_ip);
                break;
            case 3:
                *sport = strtol(token, &token, 10);
                break;
            case 4:
                *dport = strtol(token, &token, 10);
                break;
            case 5:
                hex_string_to_value(token);
                break;
            case 6:
                break;
        }
        token = strtok_r(NULL, ";", &end_str);
        i++;
    }

    // finds existing connection in database of connections
    for(uip_udp_conn = &uip_udp_conns[0]; uip_udp_conn < &uip_udp_conns[UIP_UDP_CONNS]; ++uip_udp_conn) {
        if(uip_udp_conn->lport != 0 &&
           UIP_HTONS(*dport) == uip_udp_conn->lport &&
           (uip_udp_conn->rport == 0 || UIP_HTONS(*sport) == uip_udp_conn->rport)) {
            c=uip_udp_conn;
        }
    }
    return meta;
}

/**
 * Allows to copy first digit int part and transforms it to int
 *
 * @param data
 * @param i
 * @param start
 * @param len
 * @return
 */
uint8_t parse_int_from_string(char *data, int *i, uint8_t start, uint8_t len) {
    char string[3];
    uint8_t j;

    for (j=start; j <= len; j++) {
        if (!isdigit(data[j])) {
            if (start < j) {
                strncpy (string, data + (start*sizeof(char)), (j - start));
                string[j-start] = '\0';
                *i = j-1;
                return strtol(string, &string, 10);;
            }
            else {
                return 0;
            }
        }
    }
    return 0;
}

/**
 * Handles commands sent over serial
 * w -> register wifi technology with metrics
 * p -> deliver packet to mote
 * f -> request to forward packet using rpl
 *
 * @param data
 * @param len
 * @return
 */
int handle_commands(char *data, uint8_t len) {
    PRINTF("Handling command %c\n", data[1]);
    if (data[1] == 'w') {
        metrics_struct *rpl_metrics = add_metrics(RPL_TECHNOLOGY, DEFAULT_RPL_EN, DEFAULT_RPL_BW, DEFAULT_RPL_ETX);

        tech_struct *wifi_tech = add_technology(WIFI_TECHNOLOGY);
        uint8_t i, en = 0, bw = 0, etx = 0;

        for (i=2;i <= len; i++) {
            if (data[i] == 'e') {
                en = parse_int_from_string(data, &i, i+1, len);
            }
            else if (data[i] == 'b') {
                bw = parse_int_from_string(data, &i, i+1, len);
            }
            else if (data[i] == 'x') {
                etx = parse_int_from_string(data, &i, i+1, len);
            }
        }
        add_metrics(WIFI_TECHNOLOGY, en, bw, etx);
        wr_rate = en/rpl_metrics->energy;
        sent_wifi = 0;
        sent_rpl = 0;
//        clear_flows();
        return 1;
    } else if (data[1] == 'p') {    // DELIVERY PACKET FROM WIFI PART
//        payload_len = 0;
        clear_buff();
        parse_incoming_packet(data, len, &sport, &dport, c, &payload, &payload_len, &sender_ip, &receiver_ip, 0);
#ifdef SIMPLE_UDP_HETEROGENEOUS
        heterogenous_udp_callback(c, &sender_ip, sport, &receiver_ip, dport, payload, payload_len);
#endif
#ifdef COAP_HETEROGENEOUS
        PRINTF("Delivering packet to COAP from Wifi\n");
        coap_receive_params(payload_len, &payload, sport, sender_ip);
#endif
    } else if (data[1] == 'f') {        // Forward Packet part todo debug + add form flow
        clear_buff();
        parse_incoming_packet(data, len, &sport, &dport, c, &payload, &payload_len, &sender_ip, &receiver_ip, 0);

        uint8_t k_en, k_bw, k_etx;
        fill_keys(&payload, payload_len, &k_en, &k_bw, &k_etx);

        flow_struct *flow = find_flow(&receiver_ip, k_en, k_bw, k_etx);

        if (!flow) {
            PRINTF("F: Flow not found, creating new one\n");
            flow = add_flow(&receiver_ip, RPL_TECHNOLOGY, k_en, k_bw, k_etx);
        }

        flow->validity = FLOW_VALIDITY;
        PRINTF("Forwarding packet from WIFI using RPL: ");
        PRINT6ADDR(&receiver_ip);
        PRINTF("\n");

        uip_udp_packet_forward(sender_ip, receiver_ip, sport, dport, &payload, payload_len);
        leds_on(RPL_FORWARD_LED);
        inc_sent_rpl();
    }
    return 0;
}

/**
 * Function handles requests to print data
 * m -> prints metrics table
 * f -> prints flow table
 * s -> prints statistics
 *
 * @param data
 * @param len
 * @return
 */
int handle_prints(char *data, int len) {
    PRINTF("Handling print %c\n", data[1]);
    printf(PRINT_START_SYMBOL);
    if (data[1] == 'm') {
        print_metrics_table();
    } else if (data[1] == 'f') {
        print_flow_table();
    } else if (data[1] == 'e') {
        print_energy_counter();
    }
#ifdef HETEROGENEOUS_STATISTICS
    else if (data[1] == 's') {
        print_statistics_table();
    }
#endif
    printf(PRINT_END_SYMBOL);
    return 1;
}

/**
 * Function handles incoming requests to contiki device
 * c -> request for configuration details
 * n -> request for neighbour table (used by root)
 *
 * @param data
 * @param len
 * @return
 */
int handle_requests(char *data, int len) {
    PRINTF("Handling request %c\n", data[1]);
    if (data[1] == 'c') {
        print_src_ip();
        print_mode();
        return 1;
    }
    else if (data[1] == 'n') {
        print_neighbours();
        return 1;
    }
    else if (data[1] == 'p') {
        int question_id;
        clear_buff();

        question_id = parse_incoming_packet(data, len, &sport, &dport, c, &payload, &payload_len, &sender_ip, &receiver_ip, -1);
        uint8_t k_en, k_bw, k_etx;
        fill_keys(&payload, payload_len, &k_en, &k_bw, &k_etx);
        flow_struct *flow = find_flow(&receiver_ip, k_en, k_bw, k_etx);

        PRINTF("W: forwarding callback called for packet from: ");
        PRINT6ADDR(&sender_ip);
        PRINTF(" to: ");
        PRINT6ADDR(&receiver_ip);
        PRINTF(" data: %s\n", payload);

        if (!flow) {
            PRINTF("W: Flow not found, creating new one\n");
            uint8_t dst_technology = select_technology(k_en, k_bw, k_etx);
            flow = add_flow(&receiver_ip, dst_technology, k_en, k_bw, k_etx);
        }

        if (flow) {
            flow->validity = FLOW_VALIDITY;
            PRINTF("W: flow flags: %d\n", flow->flags);
            if (flow->technology == RPL_TECHNOLOGY) {
                printf("$p;%d;0;\n", question_id);
                forwarding_semafor = 1;
                uip_udp_packet_forward(sender_ip, receiver_ip, sport, dport, &payload, payload_len);
                leds_on(RPL_FORWARD_LED);

            } else {
                printf("$p;%d;1;\n", question_id);
                leds_on(WIFI_FORWARD_LED);
                flow->flags &= ~PND;
                flow->flags |= CNF;
            }
        }

        flow_struct *src_flow = find_flow(&sender_ip, k_en, k_bw, k_etx);         // source flow

        if (!src_flow) {
            PRINTF("W: Creating new src flow\n");
            src_flow = add_flow(&sender_ip, WIFI_TECHNOLOGY, k_en, k_bw, k_etx);
        }
        else if (src_flow->technology != WIFI_TECHNOLOGY) {
            PRINTF("W: Marking src flow as WIFI\n");
            src_flow->technology = WIFI_TECHNOLOGY;
        }
        src_flow->flags &= ~PND;
        src_flow->flags |= CNF;

    }
    return 0;
}

/**
 * Function handles incoming responses to requests
 * p -> response for "ask_for_route" request
 *
 * @param data
 * @param len
 * @return
 */
int handle_responses(char *data, int len) {
    PRINTF("Handling response %c\n", data[1]);
    if (data[1] == 'p') {
        char *end_str, *token = strtok_r(data, ";", &end_str);
        int i = 0, num[3];

        while (token != NULL) {
            int number = (int)strtol(token, NULL, 10);
            token = strtok_r(NULL, ";", &end_str);
            num[i] = number;
            i++;
        }

        flow_struct *flow = find_flow_by_id(num[1], NULL);
        if (flow) {
            if (num[2] == 1) {
                flow->flags |= CNF;         // sets confirmed flag to true
            }
            else {
                flow->flags &= ~CNF;        // sets confirmed flag to false
            }
            flow->flags &= ~PND;            // removes pending flag
        }
        return 1;
    }
    if (data[1] == 'w') {
        renew_metrics_validity(WIFI_TECHNOLOGY);
    }
    return 0;
}

/**
 * Function handles input data and multiplexes them into separate functions differs them using symbols !#?$
 *
 * @param data
 * @return
 */
int handle_input(char *data) {
    int len = strlen(data);

    if (data[0] == '!') {
        return handle_commands(data, len);
    }
    else if (data[0] == '#') {
        return handle_prints(data, len);
    }
    else if (data[0] == '?') {
        return handle_requests(data, len);
    }
    else if (data[0] == '$') {
        return handle_responses(data, len);
    }
    return 0;
}

/**
 * Process which waits for serial line input
 */

PROCESS(serial_connection, "Heterogenous serial handler");

PROCESS_THREAD(serial_connection, ev, data)
{
    PROCESS_BEGIN();
    PRINTF("Starting serial line\n");

    for(;;) {
        PROCESS_YIELD();
        if(ev == serial_line_event_message) {
            handle_input((char *)data);
        }
    }
    PROCESS_END();
}