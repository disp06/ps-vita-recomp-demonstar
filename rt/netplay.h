#pragma once
#include "host.h"
#include "games.h"

enum { NP_NONE = 0, NP_HOST = 1, NP_CLIENT = 2 };
extern int g_np_role;

/* host layer: transport to the other console (non-blocking receive) */
int host_net_send(const void *buf, int len);
int host_net_recv(void *buf, int max);
int host_net_cancelled(void);                    /* user backed out of the lobby */
void host_net_status(const char *msg, int ms);   /* overlay text (NULL clears) */
void host_net_end(const char *why);              /* session over: show why, clean up */

int np_host(int game, launch_opts *opt, char *err, int errlen);
int np_join(launch_opts *opt, char *err, int errlen);
void np_init_state(void);
void rt_poll_input(host_input_t *in);
