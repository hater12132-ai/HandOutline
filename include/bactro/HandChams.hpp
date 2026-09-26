#pragma once

namespace bactro::handchams {

void registerModule();
void onSignaturesReady();
void shutdown();
void onPostFrame(); // GLES wire overlay after frame (from MotionBlur)

} // namespace bactro::handchams
