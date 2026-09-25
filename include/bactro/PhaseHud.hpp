#pragma once

namespace bactro::phasehud {

void registerModule();
void onSignaturesReady();
void onFrame();  // called from MotionBlur with valid GLES context
void shutdown();

} // namespace bactro::phasehud
