#ifndef HPL_RENDERER_MASK_H
#define HPL_RENDERER_MASK_H

namespace hpl {

    // Renderer backends an object belongs to. Objects whose mask excludes the
    // running backend are skipped at load, so a Standard/Overdrive pair that
    // shares one name is only ever loaded once. A missing mask means both.
    static constexpr unsigned kRendererMaskStandard = 1u;
    static constexpr unsigned kRendererMaskOverdrive = 2u;
    static constexpr unsigned kRendererMaskAll = kRendererMaskStandard | kRendererMaskOverdrive;

    // Discards unknown bits.
    constexpr unsigned SanitizeRendererMask(unsigned alMask) { return alMask & kRendererMaskAll; }

    constexpr bool IsRendererMaskEnabled(unsigned alMask, unsigned alBackendBit)
    {
        return (alMask & alBackendBit) != 0u;
    }
}

#endif // HPL_RENDERER_MASK_H
