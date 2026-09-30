#include "bactro/MaterialProbe.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <android/log.h>
#include <cstdio>
#include <cstdarg>

#define MP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::material {
namespace {

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    MP_LOGI("%s", buf);
}

} // namespace

void onSignaturesReady() {
    using bactro::memory::SignatureId;
    using bactro::memory::resolve;

    const auto hide = resolve(SignatureId::HideGlowOutlineQuery);
    const auto hand = resolve(SignatureId::ItemInHandShaderSetup);
    const auto mbin = resolve(SignatureId::MaterialBinPathBuilder);
    const auto miss = resolve(SignatureId::MaterialMissingError);
    const auto group = resolve(SignatureId::RenderMaterialGroupCommon);

    logLine("MaterialRE: HideGlowOutlineQuery %s @%p", hide ? "OK" : "MISS", reinterpret_cast<void*>(hide));
    logLine("MaterialRE: ItemInHandShaderSetup %s @%p", hand ? "OK" : "MISS", reinterpret_cast<void*>(hand));
    logLine("MaterialRE: MaterialBinPathBuilder %s @%p", mbin ? "OK" : "MISS", reinterpret_cast<void*>(mbin));
    logLine("MaterialRE: MaterialMissingError %s @%p", miss ? "OK" : "MISS", reinterpret_cast<void*>(miss));
    logLine("MaterialRE: RenderMaterialGroupCommon %s @%p", group ? "OK" : "MISS", reinterpret_cast<void*>(group));
    logLine("MaterialRE: uniform names in binary: ITEM_IN_HAND_EDGE_* ENTITY_EDGE_* HideGlowOutline");
    logLine("MaterialRE: phase1 resolve-only (no hooks) — next: MaterialUniformOverrides bind");
}

} // namespace bactro::material
