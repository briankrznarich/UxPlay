// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UXPLAY_MULTIDEVICE_WORKER_H
#define UXPLAY_MULTIDEVICE_WORKER_H

#include "../lib/raop.h"
#include <functional>
#include <string>

namespace multidevice {
// Internal subprocess protocol. No environment setting changes normal mode.
bool worker_enabled();
bool worker_init();
void worker_command_handler(std::function<void(const std::string &)> handler);
bool worker_configure(raop_t *raop);
void worker_event(const char *type, const char *value = "",
                  const char *name = "", const char *model = "");
void worker_ready(unsigned short port);
void worker_video(bool active);
} // namespace multidevice
#endif
