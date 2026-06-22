#pragma once

namespace vf::intro {

// Blanks the startup logo movie (sIntroMovie -> GameIntro_V3_B.bk2) and the new-game
// intro sequence (sIntroSequence) in the live settings so neither plays. The animated
// main-menu background (MainMenuLoop.bk2) and the S.P.E.C.I.A.L. videos are untouched.
// Must run after settings::ResolveAll(). Returns the number of settings blanked.
int Apply();

}
