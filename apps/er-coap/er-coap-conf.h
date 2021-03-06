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
 *      Collection of default configuration values.
 * \author
 *      Matthias Kovatsch <kovatsch@inf.ethz.ch>
 */

#ifndef ER_COAP_CONF_H_
#define ER_COAP_CONF_H_

/* Features that can be disabled to achieve smaller memory footprint */
#define COAP_LINK_FORMAT_FILTERING     0
#define COAP_PROXY_OPTION_PROCESSING   0

/* Listening port for the CoAP REST Engine */
#ifndef COAP_SERVER_PORT
#define COAP_SERVER_PORT               COAP_DEFAULT_PORT
#endif

/*-----------------------------------OPTION_METRIC: Constans needed by option metric starts here------------------------------------------*/
#define COAP_OPTION_METRIC_EQUAL_PROFILE     	128 //Value between 1-255, divisible by 2
#define COAP_OPTION_METRIC_HIGHER_PROFILE   	192 //Value between 1-255, divisible by 2
#define COAP_OPTION_METRIC_LOWER_PROFILE   	64  //Value between 1-255, divisible by 2
#define COAP_OPTION_METRIC_DECREASE_VALUE  	-16 //Divisible by 2, n*COAP_OPTION_METRIC_DECREASE_VALUE+starting metric should be 0 or 256
#define COAP_OPTION_METRIC_INCREASE_VALUE   	16  //Divisible by 2, n*COAP_OPTION_METRIC_INCREASE_VALUE+starting metric should be 0 or 256
/*-----------------------------------OPTION_METRIC: Constans needed by option metric ends here------------------------------------------*/

/* The number of concurrent messages that can be stored for retransmission in the transaction layer. */
#ifndef COAP_MAX_OPEN_TRANSACTIONS
#define COAP_MAX_OPEN_TRANSACTIONS     4
#endif /* COAP_MAX_OPEN_TRANSACTIONS */

/* Maximum number of failed request attempts before action */
#ifndef COAP_MAX_ATTEMPTS
#define COAP_MAX_ATTEMPTS              4
#endif /* COAP_MAX_ATTEMPTS */

/* Conservative size limit, as not all options have to be set at the same time. Check when Proxy-Uri option is used */
#ifndef COAP_MAX_HEADER_SIZE    /*     Hdr                  CoF  If-Match         Obs Blo strings   */
#define COAP_MAX_HEADER_SIZE           (4 + COAP_TOKEN_LEN + 3 + 1 + COAP_ETAG_LEN + 4 + 4 + 30)  /* 65 */
#endif /* COAP_MAX_HEADER_SIZE */

/* Number of observer slots (each takes abot xxx bytes) */
#ifndef COAP_MAX_OBSERVERS
#define COAP_MAX_OBSERVERS    COAP_MAX_OPEN_TRANSACTIONS - 1
#endif /* COAP_MAX_OBSERVERS */

/* Interval in notifies in which NON notifies are changed to CON notifies to check client. */
#define COAP_OBSERVE_REFRESH_INTERVAL  20

#endif /* ER_COAP_CONF_H_ */
