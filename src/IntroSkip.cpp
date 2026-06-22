#include "IntroSkip.h"
#include "EngineSettings.h"
#include "Log.h"

#include <string>

namespace vf::intro {

int Apply()
{
    // sIntroSequence is what actually plays the startup Bethesda/Vault-Tec montage
    // (its value is "GameIntro_V3_B.bk2"); sIntroMovie is a secondary override. Blanking
    // both in live memory skips the intro while leaving the main-menu background and the
    // S.P.E.C.I.A.L. videos untouched.
    static const char* kNames[] = {
        "sIntroSequence:General",
        "sIntroMovie:General",
    };

    int blanked = 0;
    for (const char* name : kNames) {
        settings::Entry* e = settings::Find(name);
        if (!e) {
            log::Write("intro: %s not resolved — skipped", name);
            continue;
        }
        // Copy before mutating — SetStringInPlace overwrites the same buffer GetString returns.
        std::string before = settings::GetString(*e);
        if (before.empty()) {
            log::Write("intro: %s already empty", name);
            continue;
        }
        if (settings::SetStringInPlace(*e, "")) {
            log::Write("intro: blanked %s (was \"%s\")", name, before.c_str());
            ++blanked;
        } else {
            log::Write("intro: could not blank %s in place", name);
        }
    }
    return blanked;
}

}
