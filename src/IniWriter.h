#pragma once

#include "EngineSettings.h"

#include <string>
#include <vector>

namespace vf::ini {

// Persists changed engine settings to the user's INIs:
//  - a key that already exists in Fallout4Prefs.ini is updated there (Prefs loads last and would
//    otherwise override the value),
//  - everything else is merged into Fallout4Custom.ini (section created when missing).
// Returns the number of settings written; on failure returns -1 and fills `err`.
int WriteChanged(const std::vector<settings::Entry*>& changed, std::string& err);

}
