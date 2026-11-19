#ifndef HPL_RENDER_INTERPOLATION_H
#define HPL_RENDER_INTERPOLATION_H

#include <vector>

#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"

namespace hpl
{

class iEntity3D;
class cCamera;

//-----------------------------------------------------------------------

/**
 * Render interpolation ("fixed timestep, interpolated presentation").
 *
 * The game simulates at a fixed 60 Hz. Rendering happens as fast as the
 * machine allows. To make every rendered frame show a smooth state, the
 * renderer presents the world at:
 *
 *      renderState = lerp(stateAfterTick[N-1], stateAfterTick[N], alpha)
 *
 * where alpha = accumulator / fixedDelta (how far we are into the next tick).
 *
 * How it works (no per-frame scene walks, no signature changes in the renderer):
 *
 *  1) Tracking is copy-on-write. Every time an entity's transform changes
 *     (iEntity3D::SetTransformUpdated) it is "touched". The first touch in a
 *     tick records the world matrix it had before the change (= state after
 *     the previous tick). Things that never move never cost anything.
 *
 *  2) After every fixed update, EndTick() resolves the world matrix of every
 *     touched entity (= state after this tick) and keeps the ones that moved
 *     in a small "active" list.
 *
 *  3) cScene::Render calls BeginRender()/EndRender() around all rendering.
 *     BeginRender writes the interpolated pose straight into each active
 *     entity (world matrix, bounding volume, light colour, bone matrices...),
 *     so EVERY consumer (culling, shadows, light volumes, particles attached to
 *     props, beams, 3D GUI...) sees one consistent interpolated world.
 *     EndRender puts the exact simulation state back, so game logic never
 *     sees an interpolated value.
 *
 *  4) Cameras use a delta scheme: only the change made *inside* the last tick
 *     is interpolated. Changes made between ticks (per-frame mouse look) are
 *     shown immediately, which is what makes the camera feel latency free.
 *
 *  5) Things that are not entities (particles, ropes, GUI values, shader time)
 *     use GetAlpha()/GetTick() directly, or the cInterpolatedFloat helpers.
 *
 * Teleports are detected by distance (see SetSnapDistance) and can also be
 * forced with SnapAll() / iEntity3D::SnapRenderInterpolation() /
 * cCamera::SnapRenderInterpolation().
 */
class cRenderInterpolation
{
    friend class iEntity3D;
    friend class cCamera;
public:
    ////////////////////////////////////
    // Engine hooks (called by cEngine / cScene)

    /**
     * Called right before the fixed update messages of a tick.
     * abNeededForFrame: false if more than one tick follows this one before the next frame is
     * drawn. Only the last two ticks of a frame are interpolated between, so the (costly)
     * snapshot work of such a tick can be skipped.
     */
    static void BeginTick(bool abNeededForFrame = true);
    /** Called right after the fixed update messages of a tick. */
    static void EndTick();
    /** Called once per frame after all ticks are done, alpha = accumulator / fixed delta. */
    static void SetFrameAlpha(double adAlpha);
    /** Called by cScene::Render before anything is drawn. Nested calls are ignored. */
    static void BeginRender();
    /** Called by cScene::Render after everything is drawn. */
    static void EndRender();

    ////////////////////////////////////
    // Settings
    static void SetEnabled(bool abX);
    static bool IsEnabled()
    {
        return mbEnabled;
    }

    /** Moves larger than this (in meters, per tick) are treated as teleports and not interpolated. */
    static void SetSnapDistance(float afX)
    {
        mfSnapDistance = afX;
    }
    static float GetSnapDistance()
    {
        return mfSnapDistance;
    }

    static void SetFixedDelta(double adX)
    {
        mdFixedDelta = adX;
    }
    static double GetFixedDelta()
    {
        return mdFixedDelta;
    }

    ////////////////////////////////////
    // State

    /** Interpolation factor for the current frame, in [0, 1]. Always 1 when disabled. */
    static float GetAlpha()
    {
        return mfAlpha;
    }
    /** Index of the most recently started fixed tick (starts at 0, first tick is 1). */
    static int GetTick()
    {
        return mlTick;
    }
    /** True while the fixed update (PreUpdate/Update/PostUpdate) is running. */
    static bool IsInsideTick()
    {
        return mbInsideTick;
    }
    /** True between BeginRender and EndRender. */
    static bool IsRendering()
    {
        return mlRenderDepth > 0;
    }

    /**
     * Simulated game time (seconds, affected by the speed multiplier) at the
     * point the interpolated frame represents. Use this for shader/texture
     * animation so it is smooth at any frame rate and follows fast forward.
     */
    static double GetRenderGameTime();
    /** Simulated game time after the last finished tick. */
    static double GetGameTime()
    {
        return mdGameTime;
    }

    /**
     * Returns true if a value stamped with alTick should be interpolated this frame.
     */
    static bool ShouldInterpolate(int alTick)
    {
        return mbEnabled && alTick == mlTick && mbInsideTick == false;
    }

    /**
     * Throws away all interpolation history (next tick starts fresh and the
     * current frame renders the plain simulation state). Use after map loads,
     * save loads, cut scene jumps, teleports and the like.
     */
    static void SnapAll();

    ////////////////////////////////////
    // Helpers
    static float Lerp(float afA, float afB, float afT)
    {
        return afA + (afB - afA) * afT;
    }
    static cVector3f Lerp(const cVector3f &avA, const cVector3f &avB, float afT)
    {
        return avA + (avB - avA) * afT;
    }
    static cVector2f Lerp(const cVector2f &avA, const cVector2f &avB, float afT)
    {
        return avA + (avB - avA) * afT;
    }
    static cColor Lerp(const cColor &aA, const cColor &aB, float afT);
    /** Interpolates an angle (radians) along the shortest arc. */
    static float LerpAngle(float afA, float afB, float afT);
    /**
     * Interpolates a transform: lerped translation, slerped rotation and lerped scale.
     * Handles scaled and mirrored (negative determinant) matrices.
     */
    static cMatrixf InterpolateMatrix(const cMatrixf &a_mtxA, const cMatrixf &a_mtxB, float afT);
    static bool MatrixEqual(const cMatrixf &a_mtxA, const cMatrixf &a_mtxB);

    ////////////////////////////////////
    // Debug
    static int GetActiveEntityCount()
    {
        return (int)mvActive.size();
    }

    /**
     * Marks an entity as changed this tick. Called automatically by
     * iEntity3D::SetTransformUpdated. Call it yourself when an entity's
     * interpolated non-transform state changes (light colour, bones, fades...).
     */
    static void TouchEntity(iEntity3D *apEntity);

private:
    ////////////////////////////////////
    // Called by iEntity3D
    static void RemoveEntity(iEntity3D *apEntity);

    ////////////////////////////////////
    // Called by cCamera
    static void AddCamera(cCamera *apCamera);
    static void RemoveCamera(cCamera *apCamera);

    static void ClearActive();
    static void ForgetTouched();

    class cTouchedEntry
    {
    public:
        iEntity3D *mpEntity;
        cMatrixf m_mtxPrev;
        bool mbPrevValid;
        bool mbTouchedOutsideTick;
    };

    class cActiveEntry
    {
    public:
        iEntity3D *mpEntity;
        cMatrixf m_mtxPrev;     //world, or camera space (view * world) for view models
        cMatrixf m_mtxCurr;
        cMatrixf m_mtxWorldCurr;//exact world transform, restored after rendering
        cCamera *mpCamera;      //set for view model entities
        bool mbMoved;
        bool mbApplied;
    };

    static std::vector<cTouchedEntry> mvTouched;
    static std::vector<cTouchedEntry> mvProcessing;
    static std::vector<cActiveEntry> mvActive;
    static std::vector<cCamera *> mvCameras;

    static bool mbEnabled;
    static bool mbInsideTick;
    static bool mbTickNeededForFrame;
    static bool mbSnapAllPending;
    static bool mbApplyingState;
    static int mlViewModelEntityCount;
    static int mlTick;
    static int mlRenderDepth;
    static float mfAlpha;
    static float mfRawAlpha;
    static float mfSnapDistance;
    static double mdFixedDelta;
    static double mdGameTime;
};

//-----------------------------------------------------------------------

/**
 * A float that is written by game logic once (or more) per fixed tick and
 * read by drawing code every frame. Get() returns the smoothly interpolated
 * value when called while drawing and the exact value otherwise.
 *
 * Writes outside a tick (from OnDraw, loading, etc) snap the value.
 */
class cInterpolatedFloat
{
public:
    cInterpolatedFloat() : mfPrev(0), mfCurr(0), mlTick(-1) {}
    explicit cInterpolatedFloat(float afX) : mfPrev(afX), mfCurr(afX), mlTick(-1) {}

    void Set(float afX)
    {
        if(cRenderInterpolation::IsInsideTick())
        {
            if(mlTick != cRenderInterpolation::GetTick())
            {
                mfPrev = mfCurr;
                mlTick = cRenderInterpolation::GetTick();
            }
        }
        else
        {
            mfPrev = afX;
            mlTick = -1;
        }
        mfCurr = afX;
    }
    /** Sets value with no interpolation from the old value. */
    void Snap(float afX)
    {
        mfPrev = afX;
        mfCurr = afX;
        mlTick = -1;
    }

    /** The exact simulation value. */
    float Value() const
    {
        return mfCurr;
    }
    /** The value to draw this frame. */
    float Get() const
    {
        if(cRenderInterpolation::ShouldInterpolate(mlTick))
        {
            return cRenderInterpolation::Lerp(mfPrev, mfCurr, cRenderInterpolation::GetAlpha());
        }
        return mfCurr;
    }

    cInterpolatedFloat &operator=(float afX)
    {
        Set(afX);
        return *this;
    }
    cInterpolatedFloat &operator+=(float afX)
    {
        Set(mfCurr + afX);
        return *this;
    }
    cInterpolatedFloat &operator-=(float afX)
    {
        Set(mfCurr - afX);
        return *this;
    }
    operator float() const
    {
        return mfCurr;
    }

private:
    float mfPrev;
    float mfCurr;
    int mlTick;
};

//-----------------------------------------------------------------------

};
#endif // HPL_RENDER_INTERPOLATION_H
