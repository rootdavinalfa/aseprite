// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_MCP_MCP_PROTOCOL_H_INCLUDED
#define APP_MCP_MCP_PROTOCOL_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

#include "json11.hpp"

#include <string>

namespace app {
namespace mcp {
namespace protocol {

enum class Era { Modern, Legacy };

extern const char kModernVersion[];
extern const char* const kLegacyVersions[];
extern const char kDefaultLegacyVersion[];

// Era is detected per request: modern requests carry the protocol
// version in params._meta, anything else falls back to legacy.
Era detectEra(const json11::Json& request);

bool isNotification(const json11::Json& request);

// Returns a null Json for notifications (no reply is sent).
json11::Json handle(const json11::Json& request, Era era);

// Compact one-line serialization for transport framing. json11's
// dump() inserts a space after ':' and ',', which MCP lines don't need.
std::string dumpCompact(const json11::Json& value);

json11::Json makeError(const json11::Json& id, int code, const std::string& message,
                       const json11::Json& data = json11::Json());

} // namespace protocol
} // namespace mcp
} // namespace app

#endif
