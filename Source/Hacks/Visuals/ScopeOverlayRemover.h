#pragma once

#include <algorithm>
#include <cstdint>

#include <MemorySearch/PatternFinder.h>
#include <MemorySearch/BytePatternLiteral.h>
#include <Platform/Macros/IsPlatform.h>
#include <Utils/ReturnAddress.h>

class ScopeOverlayRemover {
public:
    bool enabled;

    ScopeOverlayRemover(ReturnAddress scopeDust, ReturnAddress scopeArc, float* vignette)
        : scopeDust{ scopeDust }, scopeArc{ scopeArc }, vignette{ vignette }
    {
    }

    void setDrawColorHook(ReturnAddress hookReturnAddress, int& alpha) const noexcept
    {
        if (enabled && (hookReturnAddress == scopeDust || hookReturnAddress == scopeArc))
            alpha = 0;
    }

    // «Без зума»: прячем весь оверлей скоупа, включая чёрные линии, которые при движении
    // расплываются от разброса. Отдельных паттернов на линии нет, поэтому берём все вызовы
    // DrawSetColor из окрестности CHudScope::Paint (между ними лежат пыль и дуги)
    void hideWholeScope(ReturnAddress hookReturnAddress, int& alpha) const noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(hookReturnAddress.get());
        const auto dust = reinterpret_cast<std::uintptr_t>(scopeDust.get());
        const auto arc = reinterpret_cast<std::uintptr_t>(scopeArc.get());
        if (!dust)
            return;
        // границы CHudScope::Paint в билде 10-2026: [dust - 0x500, dust + 0xA00) — там 4 вызова
        // DrawSetColor: пыль, линии-«тень» (размытие по разбросу), дуги и ещё один слой размытия
        (void)arc;
        const auto begin = dust - 0x500;
        const auto end = dust + 0xA00;
        if (address >= begin && address < end)
            alpha = 0;
    }

    void updateColorCorrectionWeightsHook(bool force = false) const noexcept
    {
        if ((enabled || force) && vignette)
            *vignette = 0.0f;
    }

    template <typename Configurator>
    void configure(Configurator& configurator)
    {
        configurator("Enabled", enabled).def(false);
    }

private:
    ReturnAddress scopeDust;
    ReturnAddress scopeArc;
    float* vignette;
};
