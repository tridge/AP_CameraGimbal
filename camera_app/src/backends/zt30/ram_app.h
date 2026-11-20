#pragma once
#include "camera_app/config.h"
#include <csignal>
namespace ca_zt30 {
// RAM qualification only. Called before any main startup effects or SDK load.
bool ram_app_prepare(int argc, const volatile sig_atomic_t *stop, void (*restore_signals)());
void ram_app_configure(ca_config &settings);
bool ram_media_finished();
int ram_media_result();
bool ram_media_closed();
bool ram_media_wide();
}
