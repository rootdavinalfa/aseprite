// Aseprite
// Copyright (C) 2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/optional_alert.h"

#ifdef ENABLE_SCRIPTING
  #include "app/script/security.h"
#endif

#include "app/i18n/strings.h"
#include "ui/alert.h"
#include "ui/button.h"

namespace app {

// static
int OptionalAlert::show(Option<bool>& option, const int optionWhenDisabled, const std::string& msg)
{
#ifdef ENABLE_SCRIPTING
  // Server-driven script execution (MCP tool calls): there is no user
  // able to answer the alert; behave as if the warning option were
  // disabled and use the non-interactive default.
  if (script::hasFullAccess())
    return optionWhenDisabled;
#endif

  if (!option())
    return optionWhenDisabled;

  ui::AlertPtr alert(ui::Alert::create(msg));
  ui::CheckBox* cb = alert->addCheckBox(Strings::general_dont_show());
  const int ret = alert->show();
  if (ret == optionWhenDisabled)
    option(!cb->isSelected());
  return ret;
}

} // namespace app
