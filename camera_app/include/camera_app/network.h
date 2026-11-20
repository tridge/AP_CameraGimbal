#ifndef CAMERA_APP_NETWORK_H
#define CAMERA_APP_NETWORK_H
#include "camera_app/config.h"
/* Apply at startup. An explicit primary owns the interface's global IPv4
 * addresses; a blank primary preserves them. State tracks the optional address
 * and gateway across app restarts so clearing either removes what we added. */
int ca_network_configure(const struct ca_network_config *config, const char *state_path);
#endif
