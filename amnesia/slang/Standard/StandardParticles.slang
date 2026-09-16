// Shared GPU particle math: RNG, spawn defaults, one integration step, and the
// life-relative size/colour curves.
//
// This is the single source of truth for behaviour that used to live in
// cParticleEmitter_UserData::SetParticleDefaults and ::UpdateMotion
// (HPL2/core/sources/scene/ParticleEmitter_UserData.cpp:658 and :878). The sim
// shader calls particleSpawn/particleIntegrate; the expand shader calls
// particleEvalSize/particleEvalColor, because neither size nor colour is
// stored per particle -- both are pure functions of (startLife, life, seed,
// emitter params).
//
// The host RNG is libc rand() seeded globally, so the original per-particle
// stream is not reproducible and nothing depends on it. What must match is the
// DISTRIBUTION (same RandRect ranges, same draw order) and the integration
// math. particleRand mirrors cMath::RandRectf: min + u*(max-min), u in [0,1).

import SceneTypes;

// PCG-style integer hash. Stateless and trivially re-implementable on the host
// and in Python, which is what the numeric oracle needs.
public uint particleHash(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28) + 4u)) ^ v) * 277803737u;
    return (w >> 22) ^ w;
}

// Sequential draw. Mirrors the *order* in which SetParticleDefaults consumes
// rand(), so the host oracle can follow the same sequence.
public struct ParticleRng
{
    public uint state;
};

public ParticleRng particleRngInit(uint seed)
{
    ParticleRng rng;
    rng.state = particleHash(seed);
    return rng;
}

public float particleRand01(inout ParticleRng rng)
{
    rng.state = particleHash(rng.state);
    return float(rng.state) * (1.0f / 4294967296.0f);
}

// cMath::RandRectf
public float particleRandRange(inout ParticleRng rng, float lo, float hi)
{
    return lo + particleRand01(rng) * (hi - lo);
}

// cMath::RandRectVector2f / 3f / RandRectColor draw component-wise in order.
public float2 particleRandRange2(inout ParticleRng rng, float2 lo, float2 hi)
{
    float x = particleRandRange(rng, lo.x, hi.x);
    float y = particleRandRange(rng, lo.y, hi.y);
    return float2(x, y);
}

public float3 particleRandRange3(inout ParticleRng rng, float3 lo, float3 hi)
{
    float x = particleRandRange(rng, lo.x, hi.x);
    float y = particleRandRange(rng, lo.y, hi.y);
    float z = particleRandRange(rng, lo.z, hi.z);
    return float3(x, y, z);
}

public float4 particleRandRange4(inout ParticleRng rng, float4 lo, float4 hi)
{
    float x = particleRandRange(rng, lo.x, hi.x);
    float y = particleRandRange(rng, lo.y, hi.y);
    float z = particleRandRange(rng, lo.z, hi.z);
    float w = particleRandRange(rng, lo.w, hi.w);
    return float4(x, y, z, w);
}

// cMath::RandRectl(lo, hi) == (rand() % (hi - lo + 1)) + lo, inclusive.
public uint particleRandRangeU(inout ParticleRng rng, uint lo, uint hi)
{
    if (hi <= lo)
        return lo;
    rng.state = particleHash(rng.state);
    return lo + (rng.state % (hi - lo + 1u));
}

// cMath::MatrixRotateX/Y/Z (Math.cpp:2643-2669), composed in
// eEulerRotationOrder_XYZ, which pre-multiplies: R = Rz * Ry * Rx.
public float3 particleRotateXYZ(float3 rot, float3 v)
{
    float sx = sin(rot.x), cx = cos(rot.x);
    float sy = sin(rot.y), cy = cos(rot.y);
    float sz = sin(rot.z), cz = cos(rot.z);

    // Rx
    float3 a = float3(v.x, cx * v.y - sx * v.z, sx * v.y + cx * v.z);
    // Ry
    float3 b = float3(cy * a.x + sy * a.z, a.y, -sy * a.x + cy * a.z);
    // Rz
    return float3(cz * b.x - sz * b.y, sz * b.x + cz * b.y, b.z);
}

// -----------------------------------------------------------------------------
// The randomised half of a particle. Everything here is re-derived from `seed`
// every time it is needed rather than being stored in GpuParticle, which is what
// keeps that record at 64 B. Draw order matches SetParticleDefaults exactly.
// -----------------------------------------------------------------------------
public struct ParticleDefaults
{
    public float4 startColor;
    public float2 startSize;
    public uint   subDivNum;
    public float  bounceAmount;
    public uint   bounceCount;
    public float3 localPos;      // offset from the emitter origin
    public float3 vel;           // pre-direction-rotation
    public float  maxSpeed;
    public float  speedMul;
    public float3 acc;
    public float  spinVel;       // kGpuPartSpinConstant
    public float  spinFactor;    // kGpuPartSpinMovement
    public float  startSpin;
    public float3 revVel;
};

public ParticleDefaults particleDefaults(GpuEmitterParams p, uint seed)
{
    ParticleRng rng = particleRngInit(seed);
    ParticleDefaults d;

    // 1. start colour
    d.startColor = particleRandRange4(rng, p.minStartColor, p.maxStartColor);

    // 2. start size -- a single draw when only the x range is authored, which
    //    is what the .ps files almost always carry (SetParticleDefaults:668).
    if (p.minStartSize.y == 0.0f && p.maxStartSize.y == 0.0f)
    {
        float s = particleRandRange(rng, p.minStartSize.x, p.maxStartSize.x);
        d.startSize = float2(s, s);
    }
    else
    {
        d.startSize = particleRandRange2(rng, p.minStartSize, p.maxStartSize);
    }

    // 3. sub-division cell
    d.subDivNum = 0u;
    if (p.subDivCount > 1u)
    {
        if (p.subDivType != kGpuSubDivAnimation)
            d.subDivNum = particleRandRangeU(rng, 0u, p.subDivCount - 1u);
    }

    // 4./5. collision
    d.bounceAmount = particleRandRange(rng, p.minBounceAmount, p.maxBounceAmount);
    d.bounceCount  = particleRandRangeU(rng, p.minCollisionMax, p.maxCollisionMax);

    // 6. start position, relative to the emitter origin
    if (p.startPosType == kGpuStartTypeSphere)
    {
        float2 rot = particleRandRange2(rng, p.minStartAngles, p.maxStartAngles);
        float  r   = particleRandRange(rng, p.minStartRadius, p.maxStartRadius);
        d.localPos = particleRotateXYZ(float3(rot.x, rot.y, 0.0f), float3(0.0f, r, 0.0f));
    }
    else
    {
        d.localPos = particleRandRange3(rng, p.minStartPos, p.maxStartPos);
    }

    // 7. start velocity
    if (p.startVelType == kGpuStartTypeSphere)
    {
        float2 rot = particleRandRange2(rng, p.minStartVelAngles, p.maxStartVelAngles);
        float  s   = particleRandRange(rng, p.minStartVelSpeed, p.maxStartVelSpeed);
        d.vel = particleRotateXYZ(float3(rot.x, rot.y, 0.0f), float3(0.0f, s, 0.0f));
    }
    else
    {
        d.vel = particleRandRange3(rng, p.minStartVel, p.maxStartVel);
    }

    // 8./9./10.
    d.maxSpeed = particleRandRange(rng, p.minVelMaximum, p.maxVelMaximum);
    d.speedMul = particleRandRange(rng, p.minSpeedMultiply, p.maxSpeedMultiply);
    d.acc      = particleRandRange3(rng, p.minStartAcc, p.maxStartAcc);

    // 11. spin velocity -- Constant stores it directly, Movement stores the
    //     factor and recomputes spinVel from |vel| every step.
    float spinDraw = particleRandRange(rng, p.minSpinRange, p.maxSpinRange);
    d.spinVel    = (p.partSpinType == kGpuPartSpinMovement) ? 0.0f : spinDraw;
    d.spinFactor = (p.partSpinType == kGpuPartSpinMovement) ? spinDraw : 0.0f;

    // 12. initial spin phase
    d.startSpin = particleRandRange(rng, 0.0f, 6.28318530718f);

    // 13. revolution velocity.
    //
    // startLife is deliberately NOT drawn here. The CPU shadow scheduler needs
    // the same value to key its expiry heap, and making a libc RNG and a GPU
    // hash agree bit-for-bit is not a contract worth signing. The host draws it
    // with cMath::RandRectf(minLifeSpan, maxLifeSpan) and ships it in the spawn
    // command instead, so exactly one float crosses the boundary explicitly.
    d.revVel = particleRandRange3(rng, p.minRevVel, p.maxRevVel);

    return d;
}

public uint particlePack(bool alive, uint bounceCount, uint subDivNum)
{
    uint packed = (subDivNum & kGpuParticleSubDivMask)
                | ((bounceCount << kGpuParticleBounceShift) & kGpuParticleBounceMask);
    return alive ? (packed | kGpuParticleAliveBit) : packed;
}

public bool particleAlive(GpuParticle part)  { return (part.packed & kGpuParticleAliveBit) != 0u; }
public uint particleSubDiv(GpuParticle part) { return (part.packed & kGpuParticleSubDivMask) >> kGpuParticleSubDivShift; }
public uint particleBounce(GpuParticle part) { return (part.packed & kGpuParticleBounceMask) >> kGpuParticleBounceShift; }

// -----------------------------------------------------------------------------
// Spawn. Mirrors SetParticleDefaults' placement rules: Local emitters spawn
// around the origin of their own space, World emitters around the emitter node's
// world translation, and UsesDirection rotates the start velocity into world
// space (World coord system only -- ParticleEmitter_UserData.cpp:765).
// -----------------------------------------------------------------------------
public GpuParticle particleSpawn(GpuEmitterParams p, GpuEmitterInstance inst,
                                 uint slot, uint seed, float startLife)
{
    ParticleDefaults d = particleDefaults(p, seed);

    bool localSpace = (p.flags & kGpuEmitterFlagCoordSystemLocal) != 0u;
    float3 origin = localSpace ? float3(0.0f, 0.0f, 0.0f) : inst.worldPos;

    float3 vel = d.vel;
    if ((p.flags & kGpuEmitterFlagUsesDirection) != 0u && !localSpace)
    {
        // Rotation part of the emitter's world matrix.
        vel = mul((float3x3)inst.worldMat, vel);
    }

    GpuParticle part;
    part.pos         = origin + d.localPos;
    part.lastPos     = part.pos;
    part.vel         = vel;
    part.life        = startLife;
    part.startLife   = startLife;
    part.spin        = d.startSpin;
    part.seed        = seed;
    part.emitterSlot = slot;
    part.pad0        = 0u;
    part.packed      = particlePack(true, d.bounceCount, d.subDivNum);
    return part;
}

// -----------------------------------------------------------------------------
// One integration step. Field-for-field the body of UpdateMotion's per-particle
// loop (ParticleEmitter_UserData.cpp:938-1160), minus the collision block, which
// the sim shader applies separately because it needs the depth texture.
// -----------------------------------------------------------------------------
public GpuParticle particleIntegrate(GpuParticle part, GpuEmitterParams p,
                                     GpuEmitterInstance inst, float dt)
{
    ParticleDefaults d = particleDefaults(p, part.seed);

    part.lastPos = part.pos;
    part.pos += part.vel * dt;

    part.vel += d.acc * dt;

    if (p.gravityType == kGpuGravityVector)
    {
        part.vel += p.gravityAcc * dt;
    }
    else if (p.gravityType == kGpuGravityCenter)
    {
        // Heading is away from the emitter origin; gravityAcc.y is the
        // magnitude (ParticleEmitter_UserData.cpp:966).
        bool localSpace = (p.flags & kGpuEmitterFlagCoordSystemLocal) != 0u;
        float3 origin = localSpace ? float3(0.0f, 0.0f, 0.0f) : inst.worldPos;
        float3 dir = part.pos - origin;
        float len = length(dir);
        if (len > 0.0f)
            part.vel += (dir / len) * p.gravityAcc.y * dt;
    }

    if (d.maxSpeed > 0.0f)
    {
        float speed = length(part.vel);
        if (speed > d.maxSpeed)
            part.vel = (part.vel / speed) * d.maxSpeed;
    }

    if (d.speedMul != 0.0f && d.speedMul != 1.0f)
        part.vel *= pow(d.speedMul, dt);

    if ((p.flags & kGpuEmitterFlagUsePartSpin) != 0u)
    {
        float spinVel = (p.partSpinType == kGpuPartSpinMovement)
                      ? length(part.vel) * d.spinFactor
                      : d.spinVel;
        part.spin += spinVel * dt;
        if (part.spin >= 6.28318530718f)       part.spin -= 6.28318530718f;
        else if (part.spin <= -6.28318530718f) part.spin += 6.28318530718f;
    }

    if ((p.flags & kGpuEmitterFlagUseRevolution) != 0u)
    {
        float3 rot = d.revVel * dt;
        part.pos = particleRotateXYZ(rot, part.pos);
        part.vel = particleRotateXYZ(rot, part.vel);
    }

    part.life -= dt;

    // Sub-div animation walks the atlas over the particle's life.
    if (p.subDivType == kGpuSubDivAnimation && p.subDivCount > 0u)
    {
        float lifePercent = 1.0f - (part.life / max(part.startLife, 1e-6f));
        int cell = int(lifePercent * float(p.subDivCount) - 0.0001f);
        uint sub = uint(clamp(cell, 0, int(p.subDivCount) - 1));
        part.packed = particlePack(particleAlive(part), particleBounce(part), sub);
    }

    return part;
}

// -----------------------------------------------------------------------------
// Life-relative curves. Three segments keyed off the particle's own start life,
// exactly as UpdateMotion computes them from mfLifeSize_/mfLifeColor_Middle*.
// -----------------------------------------------------------------------------
public float2 particleEvalSize(GpuParticle part, GpuEmitterParams p, ParticleDefaults d)
{
    float middleStart = part.startLife * (1.0f - p.middleRelSizeTime);
    float middleEnd   = part.startLife * (1.0f - (p.middleRelSizeTime + p.middleRelSizeLength));

    if (part.life > middleStart)
    {
        float t = (part.life - middleStart) / max(part.startLife - middleStart, 1e-6f);
        return d.startSize * p.startRelSize * t + d.startSize * p.middleRelSize * (1.0f - t);
    }
    if (part.life > middleEnd)
    {
        return d.startSize * p.middleRelSize;
    }
    float t = part.life / max(middleEnd, 1e-6f);
    return d.startSize * p.middleRelSize * t + d.startSize * p.endRelSize * (1.0f - t);
}

public float4 particleEvalColor(GpuParticle part, GpuEmitterParams p, ParticleDefaults d)
{
    float middleStart = part.startLife * (1.0f - p.middleRelColorTime);
    float middleEnd   = part.startLife * (1.0f - (p.middleRelColorTime + p.middleRelColorLength));

    float4 color;
    if (part.life > middleStart)
    {
        float t = (part.life - middleStart) / max(part.startLife - middleStart, 1e-6f);
        color = d.startColor * p.startRelColor * t + d.startColor * p.middleRelColor * (1.0f - t);
    }
    else if (part.life > middleEnd)
    {
        color = d.startColor * p.middleRelColor;
    }
    else
    {
        float t = part.life / max(middleEnd, 1e-6f);
        color = d.startColor * p.middleRelColor * t + d.startColor * p.endRelColor * (1.0f - t);
    }

    if ((p.flags & kGpuEmitterFlagMulRGBWithAlpha) != 0u)
        color = float4(color.rgb * color.a, color.a);

    return color;
}
