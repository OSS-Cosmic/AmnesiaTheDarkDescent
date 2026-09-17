#ifndef HPL_RENDERER_MASK_H
#define HPL_RENDERER_MASK_H

namespace hpl {

    // Renderer backends an object belongs to. Objects whose mask excludes the
    // running backend are skipped at load, so a Standard/ray-traced pair that
    // shares one name is only ever loaded once. A missing mask means both.
    static constexpr unsigned kRendererMaskStandard = 1u;
    static constexpr unsigned kRendererMaskRayTraced = 2u;
    static constexpr unsigned kRendererMaskAll = kRendererMaskStandard | kRendererMaskRayTraced;

    // Discards unknown bits.
    constexpr unsigned SanitizeRendererMask(unsigned alMask) { return alMask & kRendererMaskAll; }

    constexpr bool IsRendererMaskEnabled(unsigned alMask, unsigned alBackendBit)
    {
        return (alMask & alBackendBit) != 0u;
    }

    // A billboard connected to a light is the Standard backend's fake glow; the
    // ray-traced backend lights the source for real, so the sprite must not draw
    // there. Applied when a light adopts a billboard, undone when it lets go.
    constexpr unsigned MaskWithoutRayTraced(unsigned alMask) { return alMask & ~kRendererMaskRayTraced; }
}

#endif // HPL_RENDERER_MASK_H
