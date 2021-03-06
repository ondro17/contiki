/*
 * Copyright (c) 2017, Tomas Hermanek.
 * All rights reserved.
 */


#ifndef CONTIKI_SERIAL_CONNECTION_H
#define CONTIKI_SERIAL_CONNECTION_H

#ifdef COAP_HETEROGENEOUS
#include "er-coap.h"
#include "er-coap-engine.h"
#endif

#define MAX_SIMULTANEOUS_QUESTIONS 3

extern int forwarding_semafor;

static char const* PRINT_START_SYMBOL = "<-\n";
static char const* PRINT_END_SYMBOL  = "->\n";

#include "heterogeneous-desider.h"

typedef struct question_struct {
    struct question_struct *next;
    struct flow_struct *flow;
    short question_id;
} question_struct;

#endif //CONTIKI_SERIAL_CONNECTION_H

PROCESS_NAME(serial_connection);