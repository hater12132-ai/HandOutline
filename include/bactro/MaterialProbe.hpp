#pragma once
namespace bactro::material {
void onSignaturesReady();
// Debug probes (logging only, default off). Output goes to bactro_status.txt.
void setAssetProbe(bool on);  // hooks AAssetManager_open (public NDK API) and logs *.material.bin requests
void setPathProbe(bool on);   // hooks MaterialBinPathBuilder (x8/sret-safe) and logs what it is asked for
} // namespace bactro::material
