#pragma once
#include "camera_app/private_uart.h"
struct ca_unigcs;
struct ca_media;
struct ca_backend;
struct ca_mavlink_server;

// MT11 UniGCS private camera service. Public SIYI SDK remains independent.
int ca_unigcs_open(ca_unigcs **out, ca_media *media, unsigned tcp_port,
                   unsigned discovery_port);
void ca_unigcs_update(ca_unigcs *server, ca_backend *backend, bool manual_active,
                     ca_mavlink_server *mavlink=nullptr);
void ca_unigcs_emit(void *opaque, const ca_private_frame *frame);
void ca_unigcs_close(ca_unigcs *server);
