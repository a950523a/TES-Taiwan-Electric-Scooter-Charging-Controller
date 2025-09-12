#ifndef NETWORK_SERVICES_H
#define NETWORK_SERVICES_H

#include "Charger_Defs.h"

void net_init();
void net_handle_tasks(DisplayData& data);
void net_reset_wifi_credentials();

#endif