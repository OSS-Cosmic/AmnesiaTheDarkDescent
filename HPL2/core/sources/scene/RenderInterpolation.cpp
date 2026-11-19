#include "scene/RenderInterpolation.h"

#include "scene/Entity3D.h"
#include "scene/Camera.h"
#include "math/Math.h"

#include <cmath>
#include <cstring>

namespace hpl
{

//////////////////////////////////////////////////////////////////////////
// STATIC DATA
//////////////////////////////////////////////////////////////////////////

std::vector<cRenderInterpolation::cTouchedEntry> cRenderInterpolation::mvTouched;
std::vector<cRenderInterpolation::cTouchedEntry> cRenderInterpolation::mvProcessing;
std::vector<cRenderInterpolation::cActiveEntry> cRenderInterpolation::mvActive;
std::vector<cCamera *> cRenderInterpolation::mvCameras;

bool cRenderInterpolation::mbEnabled = true;
bool cRenderInterpolation::mbInsideTick = false;
bool cRenderInterpolation::mbTickNeededForFrame = true;
bool cRenderInterpolation::mbSnapAllPending = false;
bool cRenderInterpolation::mbApplyingState = false;
int cRenderInterpolation::mlViewModelEntityCount = 0;
int cRenderInterpolation::mlTick = 0;
int cRenderInterpolation::mlRenderDepth = 0;
float cRenderInterpolation::mfAlpha = 1.0f;
float cRenderInterpolation::mfRawAlpha = 0.0f;
float cRenderInterpolation::mfSnapDistance = 2.0f;
double cRenderInterpolation::mdFixedDelta = 1.0 / 60.0;
double cRenderInterpolation::mdGameTime = 0.0;

//////////////////////////////////////////////////////////////////////////
// ENGINE HOOKS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cRenderInterpolation::BeginTick(bool abNeededForFrame)
{
    ++mlTick;
    mbInsideTick = true;
    mbTickNeededForFrame = abNeededForFrame;

    //Remember where the cameras were before this tick changed them.
    for(size_t i = 0; i < mvCameras.size(); ++i)
    {
        mvCameras[i]->InterpBeginTick();
    }
}

//-----------------------------------------------------------------------

void cRenderInterpolation::EndTick()
{
    mbInsideTick = false;
    mdGameTime += mdFixedDelta;

    const bool bSnapAll = mbSnapAllPending;
    mbSnapAllPending = false;

    //////////////////////////////
    // Cameras
    for(size_t i = 0; i < mvCameras.size(); ++i)
    {
        mvCameras[i]->InterpEndTick(mlTick, bSnapAll, mfSnapDistance);
    }

    //////////////////////////////
    // The previous active set is no longer valid
    ClearActive();

    //////////////////////////////
    // Interpolation turned off: nothing will be interpolated, so skip resolving and
    // snapshotting (this resolves all bones of every touched mesh, every tick, which is a
    // lot of CPU time on old machines where interpolation is most likely to be turned off).
    // Mark them to snap, so the history starts fresh if interpolation is turned on again.
    if(mbEnabled == false)
    {
        ForgetTouched();
        return;
    }

    //////////////////////////////
    // At least two more ticks run before the next frame is drawn (low frame rate), so this
    // snapshot would be overwritten before it is ever used. Just forget the touched list.
    // Anything changed is left with a dirty transform, so when it is touched again in the
    // next tick it starts with fresh history (snap) and interpolates normally between the
    // last two ticks. A pending SnapAll is kept for the next tick that is processed.
    if(mbTickNeededForFrame == false)
    {
        if(bSnapAll)
        {
            mbSnapAllPending = true;
        }
        for(size_t i = 0; i < mvTouched.size(); ++i)
        {
            if(mvTouched[i].mpEntity)
            {
                mvTouched[i].mpEntity->mlInterpTouchIdx = -1;
            }
        }
        mvTouched.clear();
        return;
    }

    //////////////////////////////
    // Snapshot everything touched during this tick.
    // The list is swapped out first, so anything touched by the hooks below simply
    // ends up in the (fresh) list for the next tick.
    mvProcessing.swap(mvTouched);
    mvTouched.clear();

    const float fSnapDistSqr = mfSnapDistance * mfSnapDistance;

    for(size_t i = 0; i < mvProcessing.size(); ++i)
    {
        iEntity3D *pEntity = mvProcessing[i].mpEntity;
        if(pEntity == NULL)
        {
            continue;    //destroyed during the tick
        }
        pEntity->mlInterpTouchIdx = -1;

        if(pEntity->IsRenderInterpolated() == false)
        {
            pEntity->mbInterpSnap = false;
            continue;
        }

        const bool bPrevValid = mvProcessing[i].mbPrevValid;
        const bool bOutsideTick = mvProcessing[i].mbTouchedOutsideTick;
        const cMatrixf mtxPrev = mvProcessing[i].m_mtxPrev;

        //Make sure the world transform (and things like bones) is up to date.
        pEntity->OnRenderInterpResolve();
        const cMatrixf mtxCurr = pEntity->m_mtxWorldTransform;

        bool bSnap = bSnapAll || pEntity->mbInterpSnap || bPrevValid == false || bOutsideTick;
        pEntity->mbInterpSnap = false;

        bool bMoved = false;
        if(bSnap == false && MatrixEqual(mtxPrev, mtxCurr) == false)
        {
            //Teleport check
            cVector3f vDelta = mtxCurr.GetTranslation() - mtxPrev.GetTranslation();
            if(vDelta.SqrLength() > fSnapDistSqr)
            {
                bSnap = true;
            }
            else
            {
                bMoved = true;
            }
        }

        bool bExtra = pEntity->OnRenderInterpSnapshot(bSnap);

        ////////////////////////////
        // View model (e.g. hands): interpolate relative to the camera, so it is drawn glued to
        // the camera as it is rendered, including mouse look done after the tick.
        cCamera *pViewCamera = mlViewModelEntityCount > 0 ? pEntity->FindRenderInterpCamera() : NULL;
        if(pViewCamera)
        {
            cActiveEntry entry;
            entry.mpEntity = pEntity;
            entry.mpCamera = pViewCamera;
            entry.m_mtxWorldCurr = mtxCurr;
            entry.m_mtxCurr = cMath::MatrixMul(pViewCamera->GetInterpTickEndView(), mtxCurr);

            bool bPrevOk = bSnap == false && pViewCamera->GetInterpPrevTickEndViewValid(mlTick);
            entry.m_mtxPrev = bPrevOk ? cMath::MatrixMul(pViewCamera->GetInterpPrevTickEndView(), mtxPrev) : entry.m_mtxCurr;

            entry.mbMoved = true;   //Always placed, the camera may turn between ticks
            entry.mbApplied = false;

            pEntity->mlInterpActiveIdx = (int)mvActive.size();
            mvActive.push_back(entry);
            continue;
        }

        if(bMoved || bExtra)
        {
            cActiveEntry entry;
            entry.mpEntity = pEntity;
            entry.mpCamera = NULL;
            entry.m_mtxPrev = mtxPrev;
            entry.m_mtxCurr = mtxCurr;
            entry.m_mtxWorldCurr = mtxCurr;
            entry.mbMoved = bMoved;
            entry.mbApplied = false;

            pEntity->mlInterpActiveIdx = (int)mvActive.size();
            mvActive.push_back(entry);
        }
    }

    mvProcessing.clear();
}

//-----------------------------------------------------------------------

void cRenderInterpolation::SetFrameAlpha(double adAlpha)
{
    if(adAlpha < 0.0)
    {
        adAlpha = 0.0;
    }
    if(adAlpha > 1.0)
    {
        adAlpha = 1.0;
    }

    mfRawAlpha = (float)adAlpha;
    mfAlpha = mbEnabled ? mfRawAlpha : 1.0f;
}

//-----------------------------------------------------------------------

void cRenderInterpolation::BeginRender()
{
    ++mlRenderDepth;
    if(mlRenderDepth > 1)
    {
        return;    //Nested render (e.g. a render call from inside rendering), already applied.
    }

    //////////////////////////////
    // Rendering from inside a tick (e.g. a menu background snapshot): show the exact state.
    if(mbInsideTick)
    {
        //Just make sure nothing is computed lazily later from a half updated parent.
        for(size_t i = 0; i < mvTouched.size(); ++i)
        {
            iEntity3D *pEntity = mvTouched[i].mpEntity;
            if(pEntity)
            {
                pEntity->OnRenderInterpResolve();
            }
        }
        return;
    }

    //////////////////////////////
    // Interpolation off: the exact state is shown anyway, nothing to resolve or snapshot.
    if(mbEnabled == false)
    {
        ForgetTouched();
        return;
    }

    //////////////////////////////
    // Everything touched since the last tick was changed outside of a tick (in OnDraw,
    // when created, by a teleport...). Show those exactly as they are, and make their
    // current state the starting point for the next tick, so it interpolates from here.
    // Resolving also makes sure no cached world matrix is computed from an interpolated
    // parent while rendering.
    for(size_t i = 0; i < mvTouched.size(); ++i)
    {
        iEntity3D *pEntity = mvTouched[i].mpEntity;
        if(pEntity == NULL)
        {
            continue;
        }

        pEntity->mlInterpTouchIdx = -1;
        pEntity->OnRenderInterpResolve();

        if(pEntity->IsRenderInterpolated())
        {
            pEntity->OnRenderInterpSnapshot(true);
        }
        pEntity->mbInterpSnap = false;

        //Do not interpolate it this frame
        int lActiveIdx = pEntity->mlInterpActiveIdx;
        if(lActiveIdx >= 0 && lActiveIdx < (int)mvActive.size() && mvActive[lActiveIdx].mpEntity == pEntity)
        {
            mvActive[lActiveIdx].mpEntity = NULL;
        }
        pEntity->mlInterpActiveIdx = -1;
    }
    mvTouched.clear();

    if(mbEnabled == false)
    {
        return;
    }

    const float fT = mfAlpha;

    //////////////////////////////
    // Cameras
    for(size_t i = 0; i < mvCameras.size(); ++i)
    {
        mvCameras[i]->InterpApply(fT, mlTick);
    }

    //////////////////////////////
    // Entities

    //Callbacks run while writing render state (e.g. beams flag themselves as updated),
    //that must not count as a game change.
    mbApplyingState = true;

    for(size_t i = 0; i < mvActive.size(); ++i)
    {
        cActiveEntry &entry = mvActive[i];
        iEntity3D *pEntity = entry.mpEntity;
        if(pEntity == NULL)
        {
            continue;
        }

        //Changed again after the snapshot (outside a tick), show the exact state.
        if(pEntity->mlInterpTouchIdx >= 0 || pEntity->mbTransformUpdated)
        {
            continue;
        }

        if(entry.mpCamera)
        {
            //Camera space -> world, using the camera as it is being rendered
            cMatrixf mtxCamWorld = cMath::MatrixInverse(entry.mpCamera->GetViewMatrix());
            cMatrixf mtxRel = InterpolateMatrix(entry.m_mtxPrev, entry.m_mtxCurr, fT);

            cVector3f vCamPos = mtxCamWorld.GetTranslation();
            mtxCamWorld.SetTranslation(0);
            mtxCamWorld = cMath::MatrixMul(entry.mpCamera->GetViewModelOffset(), mtxCamWorld);
            mtxCamWorld.SetTranslation(vCamPos);

            pEntity->SetRenderWorldMatrix(cMath::MatrixMul(mtxCamWorld, mtxRel));
        }
        else if(entry.mbMoved && fT < 1.0f)
        {
            pEntity->SetRenderWorldMatrix(InterpolateMatrix(entry.m_mtxPrev, entry.m_mtxCurr, fT));
        }
        pEntity->OnRenderInterpApply(fT);

        entry.mbApplied = true;
    }

    mbApplyingState = false;
}

//-----------------------------------------------------------------------

void cRenderInterpolation::EndRender()
{
    --mlRenderDepth;
    if(mlRenderDepth > 0)
    {
        return;
    }
    if(mlRenderDepth < 0)
    {
        mlRenderDepth = 0;
    }

    //////////////////////////////
    // Cameras
    for(size_t i = 0; i < mvCameras.size(); ++i)
    {
        mvCameras[i]->InterpRestore();
    }

    //////////////////////////////
    // Entities, restore in reverse order
    mbApplyingState = true;
    for(int i = (int)mvActive.size() - 1; i >= 0; --i)
    {
        cActiveEntry &entry = mvActive[i];
        if(entry.mbApplied == false)
        {
            continue;
        }
        entry.mbApplied = false;

        iEntity3D *pEntity = entry.mpEntity;
        if(pEntity == NULL)
        {
            continue;
        }

        if(entry.mbMoved)
        {
            //If something changed the transform while rendering, it will be recalculated from the
            //local transform anyway, so just leave it dirty.
            if(pEntity->mbTransformUpdated == false)
            {
                pEntity->SetRenderWorldMatrix(entry.m_mtxWorldCurr);
            }
        }
        pEntity->OnRenderInterpRestore();
    }
    mbApplyingState = false;
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// SETTINGS / STATE
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cRenderInterpolation::SetEnabled(bool abX)
{
    if(mbEnabled == abX)
    {
        return;
    }

    mbEnabled = abX;
    if(mbEnabled == false)
    {
        mfAlpha = 1.0f;
    }
    SnapAll();
}

//-----------------------------------------------------------------------

double cRenderInterpolation::GetRenderGameTime()
{
    if(mbInsideTick)
    {
        return mdGameTime;
    }

    //The interpolated frame shows the world between the last two ticks.
    if(mbEnabled)
    {
        return mdGameTime - (1.0 - (double)mfAlpha) * mdFixedDelta;
    }

    //Not interpolating: nothing else lags, so run time-only effects ahead to the real present.
    return mdGameTime + (double)mfRawAlpha * mdFixedDelta;
}

//-----------------------------------------------------------------------

void cRenderInterpolation::SnapAll()
{
    mbSnapAllPending = true;

    //Stop interpolating stale history right away (unless we are in the middle of
    //rendering, then EndRender needs the list to restore things).
    if(mlRenderDepth == 0)
    {
        ClearActive();
    }

    for(size_t i = 0; i < mvCameras.size(); ++i)
    {
        mvCameras[i]->SnapRenderInterpolation();
    }
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// REGISTRATION
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cRenderInterpolation::TouchEntity(iEntity3D *apEntity)
{
    if(apEntity->mlInterpTouchIdx >= 0)
    {
        return;    //Already touched since the last snapshot
    }
    if(mbApplyingState)
    {
        return;    //Side effect of writing render state, not a real change
    }

    cTouchedEntry entry;
    entry.mpEntity = apEntity;
    entry.mbTouchedOutsideTick = mbInsideTick == false;
    //A clean world matrix is the state the entity had after the previous tick.
    entry.mbPrevValid = apEntity->mbTransformUpdated == false;
    if(entry.mbPrevValid)
    {
        entry.m_mtxPrev = apEntity->m_mtxWorldTransform;
    }

    apEntity->mlInterpTouchIdx = (int)mvTouched.size();
    mvTouched.push_back(entry);
}

//-----------------------------------------------------------------------

void cRenderInterpolation::RemoveEntity(iEntity3D *apEntity)
{
    int lTouchIdx = apEntity->mlInterpTouchIdx;
    if(lTouchIdx >= 0 && lTouchIdx < (int)mvTouched.size() && mvTouched[lTouchIdx].mpEntity == apEntity)
    {
        mvTouched[lTouchIdx].mpEntity = NULL;
    }
    apEntity->mlInterpTouchIdx = -1;

    //In case it is destroyed by a snapshot hook while the touched list is being processed.
    for(size_t i = 0; i < mvProcessing.size(); ++i)
    {
        if(mvProcessing[i].mpEntity == apEntity)
        {
            mvProcessing[i].mpEntity = NULL;
        }
    }

    int lActiveIdx = apEntity->mlInterpActiveIdx;
    if(lActiveIdx >= 0 && lActiveIdx < (int)mvActive.size() && mvActive[lActiveIdx].mpEntity == apEntity)
    {
        mvActive[lActiveIdx].mpEntity = NULL;
    }
    apEntity->mlInterpActiveIdx = -1;
}

//-----------------------------------------------------------------------

void cRenderInterpolation::AddCamera(cCamera *apCamera)
{
    mvCameras.push_back(apCamera);
}

void cRenderInterpolation::RemoveCamera(cCamera *apCamera)
{
    for(size_t i = 0; i < mvCameras.size(); ++i)
    {
        if(mvCameras[i] == apCamera)
        {
            mvCameras.erase(mvCameras.begin() + i);
            return;
        }
    }
}

//-----------------------------------------------------------------------

void cRenderInterpolation::ForgetTouched()
{
    for(size_t i = 0; i < mvTouched.size(); ++i)
    {
        iEntity3D *pEntity = mvTouched[i].mpEntity;
        if(pEntity == NULL)
        {
            continue;
        }
        pEntity->mlInterpTouchIdx = -1;
        pEntity->mbInterpSnap = true;
    }
    mvTouched.clear();
}

//-----------------------------------------------------------------------

void cRenderInterpolation::ClearActive()
{
    for(size_t i = 0; i < mvActive.size(); ++i)
    {
        if(mvActive[i].mpEntity)
        {
            mvActive[i].mpEntity->mlInterpActiveIdx = -1;
        }
    }
    mvActive.clear();
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// HELPERS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cColor cRenderInterpolation::Lerp(const cColor &aA, const cColor &aB, float afT)
{
    return cColor(aA.r + (aB.r - aA.r) * afT,
                  aA.g + (aB.g - aA.g) * afT,
                  aA.b + (aB.b - aA.b) * afT,
                  aA.a + (aB.a - aA.a) * afT);
}

//-----------------------------------------------------------------------

float cRenderInterpolation::LerpAngle(float afA, float afB, float afT)
{
    float fDiff = afB - afA;
    while(fDiff > kPif)
    {
        fDiff -= k2Pif;
    }
    while(fDiff < -kPif)
    {
        fDiff += k2Pif;
    }
    return afA + fDiff * afT;
}

//-----------------------------------------------------------------------

bool cRenderInterpolation::MatrixEqual(const cMatrixf &a_mtxA, const cMatrixf &a_mtxB)
{
    return memcmp(a_mtxA.v, a_mtxB.v, sizeof(float) * 12) == 0;
}

//-----------------------------------------------------------------------

static inline cVector3f GetColumn(const cMatrixf &a_mtx, int alCol)
{
    return cVector3f(a_mtx.m[0][alCol], a_mtx.m[1][alCol], a_mtx.m[2][alCol]);
}

static inline void SetColumn(cMatrixf &a_mtx, int alCol, const cVector3f &avVec)
{
    a_mtx.m[0][alCol] = avVec.x;
    a_mtx.m[1][alCol] = avVec.y;
    a_mtx.m[2][alCol] = avVec.z;
}

static inline float Det3x3(const cMatrixf &a_mtx)
{
    return  a_mtx.m[0][0] * (a_mtx.m[1][1] * a_mtx.m[2][2] - a_mtx.m[2][1] * a_mtx.m[1][2]) -
            a_mtx.m[0][1] * (a_mtx.m[1][0] * a_mtx.m[2][2] - a_mtx.m[2][0] * a_mtx.m[1][2]) +
            a_mtx.m[0][2] * (a_mtx.m[1][0] * a_mtx.m[2][1] - a_mtx.m[2][0] * a_mtx.m[1][1]);
}

//Splits the 3x3 part into rotation * scale (scale along the local axes = columns).
static bool DecomposeRotScale(const cMatrixf &a_mtx, cQuaternion &aqRot, cVector3f &avScale)
{
    avScale.x = GetColumn(a_mtx, 0).Length();
    avScale.y = GetColumn(a_mtx, 1).Length();
    avScale.z = GetColumn(a_mtx, 2).Length();

    if(avScale.x < 1e-8f || avScale.y < 1e-8f || avScale.z < 1e-8f)
    {
        return false;
    }

    //Mirrored transform, move the reflection into the scale so the rest is a proper rotation.
    if(Det3x3(a_mtx) < 0)
    {
        avScale.x = -avScale.x;
    }

    cVector3f vAxis[3] = {  GetColumn(a_mtx, 0) / avScale.x,
                            GetColumn(a_mtx, 1) / avScale.y,
                            GetColumn(a_mtx, 2) / avScale.z
                         };

    //Sheared (e.g. rotated child of a non-uniformly scaled parent): not rotation * scale.
    const float kfMaxSkew = 0.001f;
    if( cMath::Abs(cMath::Vector3Dot(vAxis[0], vAxis[1])) > kfMaxSkew ||
            cMath::Abs(cMath::Vector3Dot(vAxis[0], vAxis[2])) > kfMaxSkew ||
            cMath::Abs(cMath::Vector3Dot(vAxis[1], vAxis[2])) > kfMaxSkew)
    {
        return false;
    }

    cMatrixf mtxRot = cMatrixf::Identity;
    SetColumn(mtxRot, 0, vAxis[0]);
    SetColumn(mtxRot, 1, vAxis[1]);
    SetColumn(mtxRot, 2, vAxis[2]);

    aqRot.FromRotationMatrix(mtxRot);
    aqRot.Normalize();

    return true;
}

static cQuaternion QuatInterp(const cQuaternion &aqA, const cQuaternion &aqB, float afT)
{
    cQuaternion qB = aqB;
    float fCos = aqA.w * qB.w + aqA.v.x * qB.v.x + aqA.v.y * qB.v.y + aqA.v.z * qB.v.z;

    //Shortest path
    if(fCos < 0.0f)
    {
        fCos = -fCos;
        qB = qB * -1.0f;
    }

    float fCoeffA, fCoeffB;
    if(fCos > 0.9995f)
    {
        //Very close, normalized lerp is accurate and safe.
        fCoeffA = 1.0f - afT;
        fCoeffB = afT;
    }
    else
    {
        float fAngle = acos(fCos);
        float fInvSin = 1.0f / sin(fAngle);
        fCoeffA = sin((1.0f - afT) * fAngle) * fInvSin;
        fCoeffB = sin(afT * fAngle) * fInvSin;
    }

    cQuaternion qOut = aqA * fCoeffA + qB * fCoeffB;
    qOut.Normalize();
    return qOut;
}

cMatrixf cRenderInterpolation::InterpolateMatrix(const cMatrixf &a_mtxA, const cMatrixf &a_mtxB, float afT)
{
    if(afT <= 0.0f)
    {
        return a_mtxA;
    }
    if(afT >= 1.0f)
    {
        return a_mtxB;
    }

    cMatrixf mtxOut = a_mtxB;

    //Translation
    mtxOut.m[0][3] = a_mtxA.m[0][3] + (a_mtxB.m[0][3] - a_mtxA.m[0][3]) * afT;
    mtxOut.m[1][3] = a_mtxA.m[1][3] + (a_mtxB.m[1][3] - a_mtxA.m[1][3]) * afT;
    mtxOut.m[2][3] = a_mtxA.m[2][3] + (a_mtxB.m[2][3] - a_mtxA.m[2][3]) * afT;

    //Fast path: only translation changed (very common: sliding, falling, carried objects)
    if( a_mtxA.m[0][0] == a_mtxB.m[0][0] && a_mtxA.m[0][1] == a_mtxB.m[0][1] && a_mtxA.m[0][2] == a_mtxB.m[0][2] &&
            a_mtxA.m[1][0] == a_mtxB.m[1][0] && a_mtxA.m[1][1] == a_mtxB.m[1][1] && a_mtxA.m[1][2] == a_mtxB.m[1][2] &&
            a_mtxA.m[2][0] == a_mtxB.m[2][0] && a_mtxA.m[2][1] == a_mtxB.m[2][1] && a_mtxA.m[2][2] == a_mtxB.m[2][2])
    {
        return mtxOut;
    }

    cQuaternion qA, qB;
    cVector3f vScaleA, vScaleB;
    if( DecomposeRotScale(a_mtxA, qA, vScaleA) == false ||
            DecomposeRotScale(a_mtxB, qB, vScaleB) == false ||
            (vScaleA.x < 0) != (vScaleB.x < 0))
    {
        //Degenerate, sheared or mirror flip: no proper rotation to slerp, blend the 3x3 part
        //linearly instead (never pops, at worst slightly soft for very large changes).
        for(int r = 0; r < 3; ++r)
        {
            for(int c = 0; c < 3; ++c)
            {
                mtxOut.m[r][c] = a_mtxA.m[r][c] + (a_mtxB.m[r][c] - a_mtxA.m[r][c]) * afT;
            }
        }
        return mtxOut;
    }

    cQuaternion qRot = QuatInterp(qA, qB, afT);
    cVector3f vScale = vScaleA + (vScaleB - vScaleA) * afT;

    cMatrixf mtxRot = cMath::MatrixQuaternion(qRot);
    SetColumn(mtxOut, 0, GetColumn(mtxRot, 0) * vScale.x);
    SetColumn(mtxOut, 1, GetColumn(mtxRot, 1) * vScale.y);
    SetColumn(mtxOut, 2, GetColumn(mtxRot, 2) * vScale.z);

    return mtxOut;
}

//-----------------------------------------------------------------------

}
