// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UXPLAY_MULTIDEVICE_WORKER_H
#define UXPLAY_MULTIDEVICE_WORKER_H

#include "../lib/raop.h"

namespace multidevice {
// Internal subprocess protocol. No environment setting changes normal mode.
bool worker_enabled();
bool worker_init();
bool worker_configure(raop_t *raop);
void worker_event(const char *type, const char *value = "",
                  const char *name = "", const char *model = "");
void worker_ready(unsigned short port);
void worker_video(bool active);
} // namespace multidevice
#endif
