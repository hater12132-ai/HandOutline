#pragma once
namespace bactro::material {
void onSignaturesReady();
// Debug / native material tools (default off). Output goes to bactro_status.txt.
void setAssetProbe(bool on);       // log material file opens (AAssetManager_open)
void setMaterialDump(bool on);     // save original Actor/Entity/ItemInHand material.bin to bactro_materials/dump
void setMaterialOverride(bool on); // serve bactro_materials/override/<Name>.material.bin instead of the APK file
void setPathProbe(bool on);        // MaterialBinPathBuilder logger (never fired in testing; kept for reference)
} // namespace bactro::material
