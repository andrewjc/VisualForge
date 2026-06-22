#include "FlexShim.h"
#include "Log.h"
#include "PatternScan.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <new>
#include <vector>

namespace vf::flexshim {

Tunables g_tune;

namespace {

std::atomic<int> s_calls{0};
bool s_installed = false;

// Per-function call counters so the log shows usage shape without flooding: the first few
// calls of each function are logged in full, then only the running total.
struct CallSite {
    const char* name;
    int count;
};

constexpr int kMaxTracked = 48;
CallSite s_sites[kMaxTracked] = {};
int s_siteCount = 0;

void Note(const char* name)
{
    s_calls.fetch_add(1, std::memory_order_relaxed);

    for (int i = 0; i < s_siteCount; ++i) {
        if (s_sites[i].name == name) {
            ++s_sites[i].count;
            if (s_sites[i].count <= 3)
                log::Write("flexshim: %s (call %d)", name, s_sites[i].count);
            else if (s_sites[i].count == 50)
                log::Write("flexshim: %s reached 50 calls (further calls not logged)", name);
            return;
        }
    }
    if (s_siteCount < kMaxTracked) {
        s_sites[s_siteCount++] = {name, 1};
        log::Write("flexshim: %s (first call)", name);
    }
}

// ---- solver state -----------------------------------------------------------------------
// The engine owns spawning, mesh rasterisation and rendering; all it needs from Flex is
// integrated positions. So the solver keeps the particle buffers it is handed, advances
// them, and hands them back.
//
// Buffer layouts follow the public FleX 1.0 API: particles are float4 (x, y, z, invMass),
// velocities float3. invMass == 0 means pinned.
struct Solver {
    std::vector<float> particles;   // 4 per particle
    std::vector<float> velocities;  // 3 per particle
    int count = 0;
    // All of these come from the engine's own FlexParams where the values look sane, so
    // stone, glass, metal and wood behave the way Bethesda tuned them rather than all
    // sharing one hardcoded feel. Defaults are used when a field is implausible.
    float gravity[3] = {0.0f, 0.0f, -9.8f};
    float radius = 3.5f;
    float dynamicFriction = 0.6f;  // matches kFriction below
    float staticFriction = 0.6f;
    float restitution = 0.25f;     // matches kRestitution below
    float damping = 0.0f;
    float dissipation = 0.0f;
    float maxSpeed = 0.0f; // 0 = unlimited
    bool haveParams = false;
};

// The Ext container holds the buffers the engine writes spawn data into and reads results
// from (via flexExtGetParticleData), so these are the ones that must actually be advanced.
struct Container {
    std::vector<float> particles;   // 4 per particle (x, y, z, invMass)
    std::vector<float> velocities;  // 3 per particle
    std::vector<int> phases;
    std::vector<float> normals;     // 4 per particle
    int maxParticles = 0;
    Solver* solver = nullptr;
};

// Debris pieces are rigid bodies, not loose particles: the engine rasterises a NIF into a
// particle group, then renders the mesh at the transform the solver reports back. So the
// rigid transforms are what has to move for anything to be visible.
struct Rigids {
    std::vector<float> rotations;    // 4 per rigid (quaternion x,y,z,w)
    std::vector<float> translations; // 3 per rigid
    std::vector<float> velocities;   // 3 per rigid
    std::vector<float> angular;      // 3 per rigid, radians/sec
    std::vector<float> radii;        // per rigid, measured from its own rest particle cloud
    std::vector<uint8_t> resting;    // 1 once a piece has settled
    int count = 0;
};

// Contact response. Debris are small stone/metal chunks: they bounce a little, scrub off
// most tangential speed, and rest slightly proud of the surface to avoid re-colliding.
constexpr float kContactSkin = 0.5f;  // small margin on top of the piece's own radius
constexpr float kRestitution = 0.25f;
constexpr float kFriction = 0.6f;

// Gravity adds ~6.9 units/s of speed per 0.01s step, so anything slower than these is not
// meaningful motion — it is the solver arguing with itself. Below the bounce threshold a
// contact stops rebounding; below the sleep threshold the piece is parked for good.
constexpr float kNoBounceSpeed = 45.0f;      // units/s of normal approach
constexpr float kSleepSpeed = 25.0f;         // units/s total
constexpr float kStaticFrictionSpeed = 60.0f; // below this a resting piece grips the surface
constexpr float kRollBlend = 0.35f;           // how quickly contact converts sliding to rolling

// World collision geometry, as handed over by flexUpdateTriangleMesh.
struct TriMesh {
    std::vector<float> verts;   // xyz triples
    std::vector<int> indices;   // 3 per triangle
    float lower[3] = {0, 0, 0};
    float upper[3] = {0, 0, 0};
};

// A placed instance of a collision mesh: the mesh data is local-space, so each shape
// carries the transform that puts it in the world.
struct Shape {
    const TriMesh* mesh = nullptr;
    float pos[3] = {0, 0, 0};
    float rot[4] = {0, 0, 0, 1}; // quaternion x, y, z, w
};

// A blast published by the engine. Debris inside the radius get pushed away from it.
struct ForceField {
    float pos[3] = {0, 0, 0};
    float radius = 0.0f;
    float strength = 0.0f;
    bool linearFalloff = true;
};
constexpr size_t kForceFieldSize = 28; // 3 floats + radius + strength + mode + bool

std::mutex s_solverMutex;
std::vector<Container*> s_containers;
std::vector<ForceField> s_forceFields;
Rigids s_rigids;
std::unordered_map<void*, TriMesh> s_meshes;
std::vector<Shape> s_shapes;
int s_restStride = 0;     // floats per rigid rest position; measured, not assumed
int s_vertexStride = 0;   // 3 or 4 floats per vertex; determined from the data, not assumed
int s_geometryStride = 0; // bytes per FlexCollisionGeometry entry; likewise determined
size_t s_geometryFirstOffset = 0; // where the first mesh handle sits within the block
bool s_geometryScanned = false;

// FlexCollisionGeometry is a union of the collision primitives; the triangle-mesh member is
// a handle plus a scale, so entries are 16 bytes. The measured gap between two mesh handles
// was 304 bytes = 19 entries, consistent with meshes being sparse among other primitives.
constexpr size_t kGeometryEntrySize = 16;

// Collects the candidate handle values so the scan itself can be free of C++ objects —
// __try cannot appear in a function that needs unwinding.
void* s_meshHandleList[64] = {};
int s_meshHandleCount = 0;

bool IsKnownMeshHandle(const void* p)
{
    for (int i = 0; i < s_meshHandleCount; ++i)
        if (s_meshHandleList[i] == p)
            return true;
    return false;
}

// Walks the geometry block looking for handles we issued, returning how many were seen and
// where the first two sat. Guarded, because the block's true extent is not known.
int ScanForMeshHandles(const void* block, size_t bytes, size_t& firstOffset,
                       size_t& secondOffset)
{
    int found = 0;
    auto base = static_cast<const uint8_t*>(block);
    __try {
        for (size_t off = 0; off + sizeof(void*) <= bytes; off += sizeof(void*)) {
            auto p = *reinterpret_cast<void* const*>(base + off);
            if (!IsKnownMeshHandle(p))
                continue;
            if (found == 0)
                firstOffset = off;
            else if (found == 1)
                secondOffset = off;
            if (++found >= 8)
                break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1; // block shorter than assumed
    }
    return found;
}

// Rotate a vector by a quaternion (x, y, z, w).
void QuatRotate(const float* q, const float* v, float* out)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

void QuatConjugate(const float* q, float* out)
{
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

// Advance an orientation by an angular velocity (radians/sec) for dt seconds, using the
// standard q' = q + 0.5 * (omega as a pure quaternion) * q, renormalised.
void QuatIntegrate(float* q, const float* omega, float dt)
{
    const float wx = omega[0] * 0.5f * dt;
    const float wy = omega[1] * 0.5f * dt;
    const float wz = omega[2] * 0.5f * dt;

    const float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = x + (wx * w + wy * z - wz * y);
    q[1] = y + (wy * w + wz * x - wx * z);
    q[2] = z + (wz * w + wx * y - wy * x);
    q[3] = w - (wx * x + wy * y + wz * z);

    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-8f) {
        const float inv = 1.0f / len;
        q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

// Möller–Trumbore. Returns true and the hit distance along [from -> to] if the segment
// crosses the triangle.
bool SegmentHitsTriangle(const float* from, const float* dir, float maxDist, const float* v0,
                         const float* v1, const float* v2, float& outDist, float* outNormal)
{
    float e1[3], e2[3];
    for (int i = 0; i < 3; ++i) {
        e1[i] = v1[i] - v0[i];
        e2[i] = v2[i] - v0[i];
    }

    float pv[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
                   dir[0] * e2[1] - dir[1] * e2[0]};
    const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-8f)
        return false;

    const float invDet = 1.0f / det;
    float tv[3] = {from[0] - v0[0], from[1] - v0[1], from[2] - v0[2]};
    const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;

    float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                   tv[0] * e1[1] - tv[1] * e1[0]};
    const float v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * invDet;
    if (t < 0.0f || t > maxDist)
        return false;

    outDist = t;
    // Face normal, oriented against the direction of travel.
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-12f)
        return false;
    const float sign = (n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2]) > 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 3; ++i)
        outNormal[i] = sign * n[i] / len;
    return true;
}

// The handle we return from flexCreateSolver is the Solver itself.
Solver* AsSolver(void* handle)
{
    return static_cast<Solver*>(handle);
}

// A particle is only live if the engine gave it a finite position and a non-zero inverse
// mass; the rest of a container's slots are unused capacity that must be left alone.
bool IsLiveParticle(const float* p)
{
    return p[3] > 0.0f && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// ---- replacement implementations --------------------------------------------------------
// The engine does not check whether Flex initialised, so an inert stub is not enough: it
// happily writes into whatever the allocator hands back. Anything that returns memory or a
// handle must therefore return something real. Functions whose results are only consumed
// when there are active particles can stay no-ops while the active count is reported as 0.
//
// Handles are opaque to the engine, so a zeroed block is sufficient and is deliberately
// generous in case the engine treats a handle as a struct it may poke at.
constexpr size_t kHandleBytes = 4096;

void* NewHandle()
{
    void* h = calloc(1, kHandleBytes);
    return h;
}

// flexInit returns an error enum where 0 means success (eFlexErrorNone).
int   WINAPI Shim_flexInit(...)            { Note("flexInit");            return 0; }
void  WINAPI Shim_flexShutdown(...)        { Note("flexShutdown");        }
void* WINAPI Shim_flexAcquireContext(...)  { Note("flexAcquireContext");  return NewHandle(); }
void* WINAPI Shim_flexCreateSolver(int maxParticles, int)
{
    Note("flexCreateSolver");
    auto* s = new (std::nothrow) Solver();
    if (s)
        log::Write("flexshim: solver created (maxParticles=%d)", maxParticles);
    return s;
}

void WINAPI Shim_flexDestroySolver(void* handle)
{
    Note("flexDestroySolver");
    std::lock_guard<std::mutex> lock(s_solverMutex);
    delete AsSolver(handle);
}

// FlexParams begins with { int mNumIterations; float mGravity[3]; float mRadius; ... }.
// Reading gravity from the engine removes any guesswork about units or which axis is up.
void WINAPI Shim_flexSetParams(void* handle, const void* params)
{
    Note("flexSetParams");
    Solver* s = AsSolver(handle);
    if (!s || !params)
        return;

    auto* asInt = static_cast<const int*>(params);
    auto* asFloat = reinterpret_cast<const float*>(asInt + 1); // skip mNumIterations

    std::lock_guard<std::mutex> lock(s_solverMutex);
    s->gravity[0] = asFloat[0];
    s->gravity[1] = asFloat[1];
    s->gravity[2] = asFloat[2];
    if (asFloat[3] > 0.0f && asFloat[3] < 1000.0f)
        s->radius = asFloat[3];

    // FlexParams continues: solidRestDistance, fluidRestDistance, dynamicFriction,
    // staticFriction, particleFriction, restitution, adhesion, sleepThreshold, maxSpeed,
    // shockPropagation, dissipation, damping. Each is adopted only if it is in a sensible
    // range, so a wrong offset degrades to our defaults rather than to nonsense physics.
    auto adopt = [](float value, float lo, float hi, float& target) {
        if (std::isfinite(value) && value >= lo && value <= hi)
            target = value;
    };
    adopt(asFloat[6], 0.0f, 1.0f, s->dynamicFriction);
    adopt(asFloat[7], 0.0f, 1.0f, s->staticFriction);
    adopt(asFloat[9], 0.0f, 1.0f, s->restitution);
    adopt(asFloat[12], 0.0f, 1e6f, s->maxSpeed);
    adopt(asFloat[14], 0.0f, 10.0f, s->dissipation);
    adopt(asFloat[15], 0.0f, 10.0f, s->damping);

    if (!s->haveParams) {
        s->haveParams = true;
        log::Write("flexshim: engine gravity = (%.3f, %.3f, %.3f), radius = %.3f",
                   s->gravity[0], s->gravity[1], s->gravity[2], s->radius);
        log::Write("flexshim: material params — dynFriction=%.3f statFriction=%.3f "
                   "restitution=%.3f damping=%.3f dissipation=%.3f maxSpeed=%.1f",
                   s->dynamicFriction, s->staticFriction, s->restitution, s->damping,
                   s->dissipation, s->maxSpeed);
        log::Write("flexshim: raw params [4..15] = %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f "
                   "%.3f %.3f %.3f %.3f", asFloat[4], asFloat[5], asFloat[6], asFloat[7],
                   asFloat[8], asFloat[9], asFloat[10], asFloat[11], asFloat[12],
                   asFloat[13], asFloat[14], asFloat[15]);
    }
}

void WINAPI Shim_flexSetParticles(void* handle, const float* p, int n, int mem)
{
    Note("flexSetParticles");
    Solver* s = AsSolver(handle);
    if (!s || !p || n <= 0)
        return;

    // Diagnostic: confirms the assumed signature. A sane count and plausible world
    // coordinates mean the argument layout is right; garbage means it is not.
    static int logged = 0;
    if (logged < 4) {
        ++logged;
        log::Write("flexshim: SetParticles n=%d mem=%d first=(%.1f, %.1f, %.1f) invMass=%.3f",
                   n, mem, p[0], p[1], p[2], p[3]);
    }

    std::lock_guard<std::mutex> lock(s_solverMutex);
    s->count = n;
    s->particles.assign(p, p + size_t(n) * 4);
    s->velocities.resize(size_t(n) * 3, 0.0f);
}

void WINAPI Shim_flexGetParticles(void* handle, float* p, int n, int mem)
{
    Note("flexGetParticles");
    Solver* s = AsSolver(handle);
    if (!s || !p || n <= 0)
        return;
    std::lock_guard<std::mutex> lock(s_solverMutex);
    const size_t want = size_t(n) * 4;

    static int logged = 0;
    if (logged < 4) {
        ++logged;
        log::Write("flexshim: GetParticles n=%d mem=%d have=%zu returning=(%.1f, %.1f, %.1f)",
                   n, mem, s->particles.size() / 4,
                   s->particles.size() >= 4 ? s->particles[0] : 0.0f,
                   s->particles.size() >= 4 ? s->particles[1] : 0.0f,
                   s->particles.size() >= 4 ? s->particles[2] : 0.0f);
    }

    if (s->particles.size() >= want)
        memcpy(p, s->particles.data(), want * sizeof(float));
}

void WINAPI Shim_flexSetVelocities(void* handle, const float* v, int n, int)
{
    Note("flexSetVelocities");
    Solver* s = AsSolver(handle);
    if (!s || !v || n <= 0)
        return;
    std::lock_guard<std::mutex> lock(s_solverMutex);
    s->velocities.assign(v, v + size_t(n) * 3);
}

void WINAPI Shim_flexGetVelocities(void* handle, float* v, int n, int)
{
    Note("flexGetVelocities");
    Solver* s = AsSolver(handle);
    if (!s || !v || n <= 0)
        return;
    std::lock_guard<std::mutex> lock(s_solverMutex);
    const size_t want = size_t(n) * 3;
    if (s->velocities.size() >= want)
        memcpy(v, s->velocities.data(), want * sizeof(float));
}

// Semi-implicit Euler: velocity first, then position. Particles with invMass == 0 are
// pinned and left alone, matching Flex's convention.
void WINAPI Shim_flexUpdateSolver(void* handle, float dt, int, void*)
{
    Note("flexUpdateSolver");
    Solver* s = AsSolver(handle);
    if (!s)
        return;

    // Guard against a bad or paused frame time.
    if (!(dt > 0.0f) || dt > 0.25f)
        dt = 1.0f / 60.0f;

    std::lock_guard<std::mutex> lock(s_solverMutex);

    // Note: this engine never calls flexSetParticles, so the solver's own particle buffer
    // stays empty. That must NOT short-circuit the step — debris live in the rigid list,
    // which is integrated further down.
    const int n = (s->particles.size() >= size_t(s->count) * 4) ? s->count : 0;
    if (n > 0 && s->velocities.size() < size_t(n) * 3)
        s->velocities.resize(size_t(n) * 3, 0.0f);

    const float drag = 1.0f - std::min(s->damping * g_tune.dragScale * dt, 1.0f);

    auto integrate = [&](float* particles, float* velocities, int particleCount) {
        int moved = 0;
        for (int i = 0; i < particleCount; ++i) {
            float* pos = &particles[size_t(i) * 4];
            float* vel = &velocities[size_t(i) * 3];
            if (!IsLiveParticle(pos))
                continue;
            for (int a = 0; a < 3; ++a) {
                vel[a] = (vel[a] + s->gravity[a] * g_tune.gravityScale * dt) * drag;
                pos[a] += vel[a] * dt;
            }
            ++moved;
        }
        return moved;
    };

    int moved = (n > 0) ? integrate(s->particles.data(), s->velocities.data(), n) : 0;

    // The engine reads debris positions out of the Ext container's buffers, so those are
    // the ones that have to advance for anything to be visible on screen.
    for (Container* c : s_containers) {
        if (c && c->maxParticles > 0)
            moved += integrate(c->particles.data(), c->velocities.data(), c->maxParticles);
    }

    // Debris pieces are rigid bodies; each is integrated as a point mass so it falls under
    // the engine's own gravity. Orientation is left as spawned for now — tumbling needs the
    // shape-matching solve, which is the next step.
    int rigidsMoved = 0;
    int contacts = 0;
    for (int i = 0; i < s_rigids.count; ++i) {
        float* pos = &s_rigids.translations[size_t(i) * 3];
        float* vel = &s_rigids.velocities[size_t(i) * 3];
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]))
            continue;

        // A settled piece is left exactly where it is. Without this it re-collides every
        // step — gravity pulls it in, the contact pushes it out — and visibly jitters.
        if (i < int(s_rigids.resting.size()) && s_rigids.resting[size_t(i)])
            continue;

        float from[3] = {pos[0], pos[1], pos[2]};
        for (int a = 0; a < 3; ++a)
            vel[a] = (vel[a] + s->gravity[a] * g_tune.gravityScale * dt) * drag;

        // Blasts push debris away from their centre, falling off with distance.
        for (const ForceField& f : s_forceFields) {
            float d[3] = {pos[0] - f.pos[0], pos[1] - f.pos[1], pos[2] - f.pos[2]};
            const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dist2 > f.radius * f.radius || dist2 < 1e-6f)
                continue;
            const float dist = std::sqrt(dist2);
            const float falloff = f.linearFalloff ? (1.0f - dist / f.radius)
                                                  : (1.0f - dist2 / (f.radius * f.radius));
            for (int a = 0; a < 3; ++a)
                vel[a] += (d[a] / dist) * f.strength * falloff * dt;
            if (i < int(s_rigids.resting.size()))
                s_rigids.resting[size_t(i)] = 0; // a blast wakes settled debris
        }

        // Keep speeds sane if the engine published a cap.
        if (s->maxSpeed > 0.0f) {
            const float sp = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
            if (sp > s->maxSpeed) {
                const float k = s->maxSpeed / sp;
                vel[0] *= k; vel[1] *= k; vel[2] *= k;
            }
        }

        for (int a = 0; a < 3; ++a)
            pos[a] += vel[a] * dt;

        // Tumble while airborne.
        if (i < int(s_rigids.angular.size()) / 3)
            QuatIntegrate(&s_rigids.rotations[size_t(i) * 4],
                          &s_rigids.angular[size_t(i) * 3], dt);
        ++rigidsMoved;

        // Sweep the piece's movement against the world and stop it at the first surface it
        // crosses, so debris land instead of sinking through the floor.
        float delta[3] = {pos[0] - from[0], pos[1] - from[1], pos[2] - from[2]};
        float dist = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
        if (dist < 1e-5f)
            continue;
        float dir[3] = {delta[0] / dist, delta[1] / dist, delta[2] / dist};

        // Sweep the piece as a sphere: look one clearance beyond where its centre travels,
        // so a surface is detected while it is still radius-distance away. Testing only the
        // centre's path means the piece must fall its whole radius before the next contact
        // is noticed, which turns resting into a permanent bounce.
        const float pieceRadius = (i < int(s_rigids.radii.size())) ? s_rigids.radii[size_t(i)]
                                                                  : s->radius;
        const float clearance = pieceRadius + kContactSkin;
        const float sweepLen = dist + clearance;

        float bestT = sweepLen;
        float bestN[3] = {0, 0, 1};
        bool hit = false;

        // Mesh data is local-space, so the movement is transformed into each shape's frame
        // rather than transforming thousands of vertices into the world every step.
        for (const Shape& sh : s_shapes) {
            if (!sh.mesh)
                continue;
            const TriMesh& m = *sh.mesh;

            float invRot[4];
            QuatConjugate(sh.rot, invRot);

            float relFrom[3] = {from[0] - sh.pos[0], from[1] - sh.pos[1], from[2] - sh.pos[2]};
            float localFrom[3], localDir[3];
            QuatRotate(invRot, relFrom, localFrom);
            QuatRotate(invRot, dir, localDir);

            // Cheap reject in the shape's own space, where the mesh bounds live.
            const float localTo[3] = {localFrom[0] + localDir[0] * bestT,
                                      localFrom[1] + localDir[1] * bestT,
                                      localFrom[2] + localDir[2] * bestT};
            bool outside = false;
            for (int a = 0; a < 3 && !outside; ++a) {
                const float lo = std::min(localFrom[a], localTo[a]) - kContactSkin;
                const float hi = std::max(localFrom[a], localTo[a]) + kContactSkin;
                if (hi < m.lower[a] || lo > m.upper[a])
                    outside = true;
            }
            if (outside)
                continue;

            const size_t triCount = m.indices.size() / 3;
            const size_t maxIndex = m.verts.size() / 3;
            for (size_t t = 0; t < triCount; ++t) {
                const int i0 = m.indices[t * 3 + 0];
                const int i1 = m.indices[t * 3 + 1];
                const int i2 = m.indices[t * 3 + 2];
                if (i0 < 0 || i1 < 0 || i2 < 0 || size_t(i0) >= maxIndex ||
                    size_t(i1) >= maxIndex || size_t(i2) >= maxIndex)
                    continue;

                float hitT = 0.0f, localN[3];
                if (SegmentHitsTriangle(localFrom, localDir, bestT, &m.verts[size_t(i0) * 3],
                                        &m.verts[size_t(i1) * 3], &m.verts[size_t(i2) * 3],
                                        hitT, localN)) {
                    bestT = hitT;
                    QuatRotate(sh.rot, localN, bestN); // normal back into world space
                    hit = true;
                }
            }
        }

        if (hit) {
            ++contacts;
            // Push the piece out along the surface normal by its own radius. Resolving only
            // along the direction of travel leaves the chunk's centre on the surface, which
            // buries half of it — the visible "settling slightly below the terrain".
            for (int a = 0; a < 3; ++a)
                pos[a] = from[a] + dir[a] * bestT + bestN[a] * clearance;

            const float vn = vel[0] * bestN[0] + vel[1] * bestN[1] + vel[2] * bestN[2];
            const bool gentle = std::fabs(vn) < kNoBounceSpeed;

            float tangent[3];
            for (int a = 0; a < 3; ++a)
                tangent[a] = vel[a] - vn * bestN[a];
            const float tangentSpeed =
                std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] +
                          tangent[2] * tangent[2]);

            // Flex reports friction as a coefficient (higher = grippier) whereas this is a
            // velocity retention factor (higher = slippier), so it has to be inverted.
            const float friction = s->dynamicFriction * g_tune.frictionScale;
            const float retention = std::max(0.0f, std::min(1.0f, 1.0f - friction));
            const float bounce = s->restitution * g_tune.restitutionScale;

            float* w = (i < int(s_rigids.angular.size()) / 3)
                           ? &s_rigids.angular[size_t(i) * 3] : nullptr;
            const float spin = w ? std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) : 0.0f;

            // Static friction: below this the piece grips instead of creeping. It is gated
            // on spin as well, otherwise a rolling piece would be glued in place.
            const bool grips = tangentSpeed < kStaticFrictionSpeed &&
                               spin * pieceRadius < kStaticFrictionSpeed;
            const float tangentScale = grips ? 0.0f : retention;

            for (int a = 0; a < 3; ++a) {
                // A gentle touch should not rebound at all, otherwise the piece hops
                // forever on ever-smaller bounces instead of coming to rest.
                vel[a] = gentle ? tangent[a] * tangentScale
                                : tangent[a] * tangentScale - vn * bestN[a] * bounce;
            }

            if (w) {
                // Off-centre impacts impart spin. The contact sits at -N * radius from the
                // centre, so the normal impulse passes straight through it and contributes
                // nothing — it is the tangential (friction) impulse that sets a chunk
                // cartwheeling, exactly as a ball gripping the ground starts to rotate.
                const float jt[3] = {tangent[0] * (tangentScale - 1.0f),
                                     tangent[1] * (tangentScale - 1.0f),
                                     tangent[2] * (tangentScale - 1.0f)};
                // torque = r x J with r = -N * radius; inertia of a sphere = 0.4 m r^2.
                const float k = g_tune.impactTorque / std::max(0.4f * pieceRadius, 1e-3f);
                w[0] += -(bestN[1] * jt[2] - bestN[2] * jt[1]) * k;
                w[1] += -(bestN[2] * jt[0] - bestN[0] * jt[2]) * k;
                w[2] += -(bestN[0] * jt[1] - bestN[1] * jt[0]) * k;

                // Rolling: for contact without slipping the surface speed equals the
                // tangential speed, i.e. omega = (N x v_t) / radius. Blending toward that
                // makes round debris roll down slopes instead of only skidding to a halt.
                if (g_tune.rolling && !grips) {
                    const float inv = 1.0f / std::max(pieceRadius, 1e-3f);
                    const float target[3] = {
                        (bestN[1] * tangent[2] - bestN[2] * tangent[1]) * inv,
                        (bestN[2] * tangent[0] - bestN[0] * tangent[2]) * inv,
                        (bestN[0] * tangent[1] - bestN[1] * tangent[0]) * inv};
                    for (int a = 0; a < 3; ++a)
                        w[a] += (target[a] - w[a]) * kRollBlend;
                }
            }

            // Scrub spin on contact — a chunk skidding on the ground stops rolling quickly.
            if (i < int(s_rigids.angular.size()) / 3) {
                float* w = &s_rigids.angular[size_t(i) * 3];
                const float spinDamp = gentle ? 0.3f : 0.75f;
                w[0] *= spinDamp; w[1] *= spinDamp; w[2] *= spinDamp;
            }

            const float speed = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
            if (speed < kSleepSpeed && i < int(s_rigids.resting.size())) {
                vel[0] = vel[1] = vel[2] = 0.0f;
                if (i < int(s_rigids.angular.size()) / 3) {
                    float* w = &s_rigids.angular[size_t(i) * 3];
                    w[0] = w[1] = w[2] = 0.0f; // settled pieces must not keep spinning
                }
                s_rigids.resting[size_t(i)] = 1;
            }
        }
    }

    // Debris against each other, so chunks pile instead of interpenetrating. Pieces are few
    // (a handful per impact) so an all-pairs test is cheaper than any acceleration structure.
    int pairContacts = 0;
    for (int i = 0; i < s_rigids.count; ++i) {
        float* pi = &s_rigids.translations[size_t(i) * 3];
        const float ri = (i < int(s_rigids.radii.size())) ? s_rigids.radii[size_t(i)]
                                                          : s->radius;
        if (!std::isfinite(pi[0]))
            continue;

        for (int j = i + 1; j < s_rigids.count; ++j) {
            float* pj = &s_rigids.translations[size_t(j) * 3];
            const float rj = (j < int(s_rigids.radii.size())) ? s_rigids.radii[size_t(j)]
                                                              : s->radius;
            if (!std::isfinite(pj[0]))
                continue;

            float d[3] = {pi[0] - pj[0], pi[1] - pj[1], pi[2] - pj[2]};
            const float minDist = ri + rj;
            const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dist2 >= minDist * minDist || dist2 < 1e-8f)
                continue;

            const float dist = std::sqrt(dist2);
            const float n[3] = {d[0] / dist, d[1] / dist, d[2] / dist};
            const float overlap = minDist - dist;
            ++pairContacts;

            // Separate equally; equal masses is a fair assumption for chunks of one material.
            for (int a = 0; a < 3; ++a) {
                pi[a] += n[a] * overlap * 0.5f;
                pj[a] -= n[a] * overlap * 0.5f;
            }

            float* vi = &s_rigids.velocities[size_t(i) * 3];
            float* vj = &s_rigids.velocities[size_t(j) * 3];
            const float approach = (vi[0] - vj[0]) * n[0] + (vi[1] - vj[1]) * n[1] +
                                   (vi[2] - vj[2]) * n[2];
            if (approach < 0.0f) {
                // Exchange the closing velocity, damped by the material's restitution.
                const float impulse = -approach * (1.0f + s->restitution) * 0.5f;
                for (int a = 0; a < 3; ++a) {
                    vi[a] += n[a] * impulse;
                    vj[a] -= n[a] * impulse;
                }
                // Being shoved counts as motion, so a settled piece must wake up.
                if (std::fabs(approach) > kSleepSpeed) {
                    if (i < int(s_rigids.resting.size())) s_rigids.resting[size_t(i)] = 0;
                    if (j < int(s_rigids.resting.size())) s_rigids.resting[size_t(j)] = 0;
                }
            }
        }
    }

    static int logged = 0;
    if ((moved > 0 || rigidsMoved > 0) && logged < 8) {
        ++logged;
        float rmin = 1e9f, rmax = 0.0f;
        for (int i = 0; i < s_rigids.count && i < int(s_rigids.radii.size()); ++i) {
            rmin = std::min(rmin, s_rigids.radii[size_t(i)]);
            rmax = std::max(rmax, s_rigids.radii[size_t(i)]);
        }
        log::Write("flexshim: advanced %d particles, %d rigid pieces, %d world contacts, "
                   "%d piece-piece contacts (piece radii %.1f..%.1f)", moved, rigidsMoved,
                   contacts, pairContacts, rmin, rmax);
    }
}
void  WINAPI Shim_flexSetPhases(...)       { Note("flexSetPhases");       }
void  WINAPI Shim_flexSetActive(...)       { Note("flexSetActive");       }
// Reporting zero active particles is what keeps the rest of the API harmless: the engine
// has nothing to read back, so the getters below are never asked for real data.
int   WINAPI Shim_flexGetActiveCount(...)  { Note("flexGetActiveCount");  return 0; }
// flexSetRigids(solver, offsets, indices, restPositions, restNormals, stiffness,
//               rotations, translations, numRigids, memory)
// The engine supplies each piece's starting orientation and position here; we take those as
// the initial state and integrate from them.
void WINAPI Shim_flexSetRigids(void* handle, const int* offsets, const int*,
                               const float* restPositions, const float*, const float*,
                               const float* rotations, const float* translations,
                               int numRigids, int)
{
    Note("flexSetRigids");
    Solver* solver = AsSolver(handle);
    if (numRigids <= 0 || numRigids > (1 << 20))
        return;

    std::lock_guard<std::mutex> lock(s_solverMutex);

    const int previous = s_rigids.count;
    s_rigids.count = numRigids;
    s_rigids.rotations.resize(size_t(numRigids) * 4, 0.0f);
    s_rigids.translations.resize(size_t(numRigids) * 3, 0.0f);
    s_rigids.velocities.resize(size_t(numRigids) * 3, 0.0f);
    s_rigids.angular.resize(size_t(numRigids) * 3, 0.0f);
    s_rigids.radii.resize(size_t(numRigids), solver ? solver->radius : 3.5f);
    s_rigids.resting.resize(size_t(numRigids), 0);

    // Measure each piece from the rest positions of the particles it was rasterised into,
    // so a concrete slab collides at its real size instead of every chunk being one
    // particle-radius sphere. restPositions may be packed as float3 or padded to float4;
    // the stride is chosen by whichever yields a believable extent.
    if (offsets && restPositions && s_restStride == 0) {
        for (int stride : {4, 3}) {
            float worst = 0.0f;
            bool sane = true;
            for (int i = 0; i < numRigids && sane; ++i) {
                const int b = offsets[i];
                const int e = (i + 1 <= numRigids) ? offsets[i + 1] : b;
                if (b < 0 || e < b || e - b > 4096) { sane = false; break; }
                for (int p = b; p < e; ++p) {
                    const float* rp = restPositions + size_t(p) * stride;
                    const float d2 = rp[0] * rp[0] + rp[1] * rp[1] + rp[2] * rp[2];
                    if (!std::isfinite(d2)) { sane = false; break; }
                    worst = std::max(worst, d2);
                }
            }
            const float extent = std::sqrt(worst);
            if (sane && extent > 0.05f && extent < 500.0f) {
                s_restStride = stride;
                log::Write("flexshim: rigid rest-position stride = %d floats (largest piece "
                           "extent %.2f units)", stride, extent);
                break;
            }
        }
        if (s_restStride == 0)
            log::Write("flexshim: could not measure piece sizes — falling back to the "
                       "particle radius for all debris");
    }

    if (offsets && restPositions && s_restStride > 0) {
        for (int i = 0; i < numRigids; ++i) {
            const int b = offsets[i];
            const int e = offsets[i + 1];
            if (b < 0 || e < b || e - b > 4096)
                continue;
            float worst = 0.0f;
            for (int p = b; p < e; ++p) {
                const float* rp = restPositions + size_t(p) * s_restStride;
                worst = std::max(worst, rp[0] * rp[0] + rp[1] * rp[1] + rp[2] * rp[2]);
            }
            // The cloud's extent plus the particle radius is the piece's true half-size.
            const float r = std::sqrt(worst) + (solver ? solver->radius : 3.5f);
            if (std::isfinite(r) && r > 0.1f && r < 500.0f)
                s_rigids.radii[size_t(i)] = r;
        }
    }

    if (rotations)
        memcpy(s_rigids.rotations.data(), rotations, size_t(numRigids) * 4 * sizeof(float));

    // Only adopt positions for pieces we have not seen before; overwriting every frame
    // would snap them back to their spawn point and undo the simulation.
    if (translations) {
        for (int i = previous; i < numRigids; ++i) {
            memcpy(&s_rigids.translations[size_t(i) * 3], &translations[size_t(i) * 3],
                   3 * sizeof(float));
            s_rigids.velocities[size_t(i) * 3 + 0] = 0.0f;
            s_rigids.velocities[size_t(i) * 3 + 1] = 0.0f;
            s_rigids.velocities[size_t(i) * 3 + 2] = 0.0f;
            s_rigids.resting[size_t(i)] = 0;

            // Give each new piece a spin. Real Flex derives rotation by shape-matching the
            // particle cloud to its rest pose; for chunks this size an integrated angular
            // velocity is visually equivalent and vastly cheaper. The axis is derived from
            // the piece index so a given piece tumbles consistently rather than flickering.
            const float a1 = float(i) * 2.399963f;   // golden angle, well-spread axes
            const float a2 = float(i) * 1.618034f;
            float axis[3] = {std::cos(a1) * std::sin(a2), std::sin(a1) * std::sin(a2),
                             std::cos(a2)};
            const float alen = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] +
                                         axis[2] * axis[2]);
            const float spin = g_tune.spawnSpin * (0.6f + 0.8f * std::fabs(std::sin(a1 * 3.0f)));
            for (int a = 0; a < 3; ++a)
                s_rigids.angular[size_t(i) * 3 + a] = (alen > 1e-6f) ? axis[a] / alen * spin
                                                                     : 0.0f;
        }
    }

    static int logged = 0;
    if (logged < 4 && numRigids > 0) {
        ++logged;
        log::Write("flexshim: SetRigids numRigids=%d first translation=(%.1f, %.1f, %.1f)",
                   numRigids, s_rigids.translations[0], s_rigids.translations[1],
                   s_rigids.translations[2]);
    }
}

// flexGetRigidTransforms(solver, rotations, translations, memory)
void WINAPI Shim_flexGetRigidTransforms(void* handle, float* rotations, float* translations,
                                        int)
{
    Note("flexGetRigidTransforms");
    (void)handle;
    std::lock_guard<std::mutex> lock(s_solverMutex);
    if (s_rigids.count <= 0)
        return;

    if (rotations)
        memcpy(rotations, s_rigids.rotations.data(),
               size_t(s_rigids.count) * 4 * sizeof(float));
    if (translations)
        memcpy(translations, s_rigids.translations.data(),
               size_t(s_rigids.count) * 3 * sizeof(float));

    static int logged = 0;
    if (logged < 4) {
        ++logged;
        log::Write("flexshim: GetRigidTransforms n=%d returning first=(%.1f, %.1f, %.1f)",
                   s_rigids.count, s_rigids.translations[0], s_rigids.translations[1],
                   s_rigids.translations[2]);
    }
}
void  WINAPI Shim_flexGetBounds(...)       { Note("flexGetBounds");       }
// flexSetShapes(solver, geometry, numGeometryEntries, shapeAabbMin, shapeAabbMax,
//               shapeOffsets, shapePositions, shapeRotations, shapePrevPositions,
//               shapePrevRotations, shapeFlags, numShapes, memory)
//
// The FlexCollisionGeometry entry for a triangle mesh begins with the mesh handle. Since we
// allocated those handles ourselves, the entry size can be established by finding the stride
// at which the read-back pointers match handles we know — no guessing required.
void WINAPI Shim_flexSetShapes(void* solver, const void* geometry, int numGeometryEntries,
                               const float*, const float*, const int* shapeOffsets,
                               const float* positions, const float* rotations, const float*,
                               const float*, const int*, int numShapes, int)
{
    Note("flexSetShapes");
    (void)solver;
    if (!positions || numShapes <= 0 || numShapes > 65536)
        return;

    std::lock_guard<std::mutex> lock(s_solverMutex);

    // Locate our own mesh handles inside the geometry block rather than assuming a stride:
    // scan for the pointer values we issued and derive the spacing from where they land.
    if (geometry && numGeometryEntries > 0 && s_geometryStride == 0 && !s_meshes.empty() &&
        !s_geometryScanned) {
        s_geometryScanned = true;

        const size_t bytes = std::min<size_t>(size_t(numGeometryEntries) * 64, 256 * 1024);
        size_t firstOffset = 0, secondOffset = 0;
        const int found = ScanForMeshHandles(geometry, bytes, firstOffset, secondOffset);

        if (found < 0) {
            log::Write("flexshim: geometry scan faulted — block shorter than assumed");
        } else if (found >= 2) {
            s_geometryStride = int(secondOffset - firstOffset);
            s_geometryFirstOffset = firstOffset;
            log::Write("flexshim: found %d mesh handles in the geometry block; first at "
                       "+%zu, spacing %d bytes", found, firstOffset, s_geometryStride);
        } else if (found == 1) {
            log::Write("flexshim: only one mesh handle found at +%zu — cannot derive spacing",
                       firstOffset);
        } else {
            log::Write("flexshim: no mesh handles present in the geometry block "
                       "(entries=%d, known meshes=%zu) — meshes are referenced some other "
                       "way, e.g. by index", numGeometryEntries, s_meshes.size());
        }
    }

    s_shapes.clear();
    s_shapes.reserve(size_t(numShapes));

    for (int i = 0; i < numShapes; ++i) {
        Shape sh;
        // Positions and rotations are float4 per shape.
        sh.pos[0] = positions[size_t(i) * 4 + 0];
        sh.pos[1] = positions[size_t(i) * 4 + 1];
        sh.pos[2] = positions[size_t(i) * 4 + 2];
        if (rotations) {
            for (int a = 0; a < 4; ++a)
                sh.rot[a] = rotations[size_t(i) * 4 + a];
        }

        // A shape owns a run of geometry entries given by shapeOffsets, and only some of
        // those are triangle meshes — the rest are spheres, capsules and boxes. Walk this
        // shape's run and take the first entry that resolves to a mesh we know.
        if (geometry && shapeOffsets) {
            const int begin = shapeOffsets[i];
            const int end = (i + 1 < numShapes) ? shapeOffsets[i + 1] : numGeometryEntries;
            auto base = static_cast<const uint8_t*>(geometry);
            for (int g = begin; g < end && g < numGeometryEntries; ++g) {
                if (g < 0)
                    break;
                auto p = *reinterpret_cast<void* const*>(base + size_t(g) * kGeometryEntrySize);
                auto it = s_meshes.find(p);
                if (it != s_meshes.end()) {
                    sh.mesh = &it->second;
                    break;
                }
            }
        }
        s_shapes.push_back(sh);
    }

    static int logged = 0;
    if (logged < 4) {
        ++logged;
        int withMesh = 0;
        for (const Shape& sh : s_shapes)
            if (sh.mesh)
                ++withMesh;
        log::Write("flexshim: SetShapes numShapes=%d geomEntries=%d meshesResolved=%d "
                   "first pos=(%.1f, %.1f, %.1f)", numShapes, numGeometryEntries, withMesh,
                   s_shapes[0].pos[0], s_shapes[0].pos[1], s_shapes[0].pos[2]);
    }
}
void* WINAPI Shim_flexCreateTriangleMesh(...)  { Note("flexCreateTriangleMesh");  return NewHandle(); }
// flexUpdateTriangleMesh(mesh, vertices, indices, numVertices, numTriangles, lower, upper,
//                        memory)
// FleX pads vertices to float4 in some builds and packs them as float3 in others. Rather
// than assume, the stride is decided once by checking which reading puts every vertex inside
// the axis-aligned bounds the engine itself supplied.
void WINAPI Shim_flexUpdateTriangleMesh(void* mesh, const float* vertices, const int* indices,
                                        int numVertices, int numTriangles, const float* lower,
                                        const float* upper, int)
{
    Note("flexUpdateTriangleMesh");
    if (!mesh || !vertices || !indices || numVertices <= 0 || numTriangles <= 0)
        return;
    if (numVertices > 1'000'000 || numTriangles > 1'000'000)
        return; // implausible; refuse rather than read wild memory

    auto fitsBounds = [&](int stride) {
        if (!lower || !upper)
            return false;
        const int probes = std::min(numVertices, 64);
        for (int i = 0; i < probes; ++i) {
            const float* v = vertices + size_t(i) * stride;
            for (int a = 0; a < 3; ++a) {
                if (!std::isfinite(v[a]))
                    return false;
                // Allow a little slack for bounds that were rounded.
                const float pad = 1.0f + 0.001f * std::fabs(upper[a] - lower[a]);
                if (v[a] < lower[a] - pad || v[a] > upper[a] + pad)
                    return false;
            }
        }
        return true;
    };

    std::lock_guard<std::mutex> lock(s_solverMutex);

    if (s_vertexStride == 0) {
        if (fitsBounds(4))
            s_vertexStride = 4;
        else if (fitsBounds(3))
            s_vertexStride = 3;
        if (s_vertexStride)
            log::Write("flexshim: collision mesh vertex stride = %d floats (verified against "
                       "the engine's own bounds)", s_vertexStride);
        else
            log::Write("flexshim: could not determine vertex stride — collision disabled");
    }
    if (s_vertexStride == 0)
        return;

    TriMesh& m = s_meshes[mesh];
    if (!IsKnownMeshHandle(mesh) && s_meshHandleCount < 64)
        s_meshHandleList[s_meshHandleCount++] = mesh;
    m.verts.resize(size_t(numVertices) * 3);
    for (int i = 0; i < numVertices; ++i) {
        const float* v = vertices + size_t(i) * s_vertexStride;
        m.verts[size_t(i) * 3 + 0] = v[0];
        m.verts[size_t(i) * 3 + 1] = v[1];
        m.verts[size_t(i) * 3 + 2] = v[2];
    }
    m.indices.assign(indices, indices + size_t(numTriangles) * 3);
    for (int a = 0; a < 3; ++a) {
        m.lower[a] = lower ? lower[a] : -1e30f;
        m.upper[a] = upper ? upper[a] : 1e30f;
    }

    static int logged = 0;
    if (logged < 3) {
        ++logged;
        log::Write("flexshim: collision mesh %d verts / %d tris, bounds (%.0f %.0f %.0f)-"
                   "(%.0f %.0f %.0f)", numVertices, numTriangles, m.lower[0], m.lower[1],
                   m.lower[2], m.upper[0], m.upper[1], m.upper[2]);
    }
}
void  WINAPI Shim_flexDestroyTriangleMesh(void* m) { Note("flexDestroyTriangleMesh"); free(m); }

// flexAlloc is a real allocator in the SDK — the engine writes particle data straight into
// what it returns, so this has to hand back usable memory.
void* WINAPI Shim_flexAlloc(int size)
{
    Note("flexAlloc");
    return size > 0 ? calloc(1, size_t(size)) : NewHandle();
}
void  WINAPI Shim_flexFree(void* p)        { Note("flexFree");            free(p); }
void  WINAPI Shim_flexSetFence(...)        { Note("flexSetFence");        }
void  WINAPI Shim_flexWaitFence(...)       { Note("flexWaitFence");       }
void  WINAPI Shim_flexStartRecord(...)     { Note("flexStartRecord");     }
void  WINAPI Shim_flexStopRecord(...)      { Note("flexStopRecord");      }
void  WINAPI Shim_flexSnapshot(...)        { Note("flexSnapshot");        }

// The Ext container owns the particle storage the engine writes spawn data into and reads
// results back from — flexExtGetParticleData hands out pointers to it. Owning that memory
// is therefore what actually lets our integration be seen.
void* WINAPI Shim_flexExtCreateContainer(void* solver, int maxParticles)
{
    Note("flexExtCreateContainer");
    auto* c = new (std::nothrow) Container();
    if (!c)
        return nullptr;

    c->maxParticles = (maxParticles > 0 && maxParticles < 1 << 20) ? maxParticles : 6000;
    c->particles.assign(size_t(c->maxParticles) * 4, 0.0f);
    c->velocities.assign(size_t(c->maxParticles) * 3, 0.0f);
    c->phases.assign(size_t(c->maxParticles), 0);
    c->normals.assign(size_t(c->maxParticles) * 4, 0.0f);
    c->solver = AsSolver(solver);

    {
        std::lock_guard<std::mutex> lock(s_solverMutex);
        s_containers.push_back(c);
    }
    log::Write("flexshim: ext container created (maxParticles=%d)", c->maxParticles);
    return c;
}

void WINAPI Shim_flexExtDestroyContainer(void* handle)
{
    Note("flexExtDestroyContainer");
    auto* c = static_cast<Container*>(handle);
    std::lock_guard<std::mutex> lock(s_solverMutex);
    s_containers.erase(std::remove(s_containers.begin(), s_containers.end(), c),
                       s_containers.end());
    delete c;
}
void* WINAPI Shim_flexExtCreateRigidFromMesh(...){ Note("flexExtCreateRigidFromMesh");return NewHandle(); }
void  WINAPI Shim_flexExtDestroyAsset(void* a)   { Note("flexExtDestroyAsset");       free(a); }
// Hands the engine pointers into our own storage, so whatever we integrate is what it sees.
void WINAPI Shim_flexExtGetParticleData(void* handle, float** particles, float** velocities,
                                        int** phases, float** normals)
{
    Note("flexExtGetParticleData");
    auto* c = static_cast<Container*>(handle);
    if (!c)
        return;

    std::lock_guard<std::mutex> lock(s_solverMutex);
    if (particles)  *particles  = c->particles.data();
    if (velocities) *velocities = c->velocities.data();
    if (phases)     *phases     = c->phases.data();
    if (normals)    *normals    = c->normals.data();

    static int logged = 0;
    if (logged < 3) {
        ++logged;
        log::Write("flexshim: ExtGetParticleData -> our buffers (p=%p v=%p ph=%p n=%p)",
                   (void*)(particles ? *particles : nullptr),
                   (void*)(velocities ? *velocities : nullptr),
                   (void*)(phases ? *phases : nullptr),
                   (void*)(normals ? *normals : nullptr));
    }
}
// flexExtSetForceFields(container, forceFields, numForceFields, memory)
// FlexExtForceField = { float position[3]; float radius; float strength; int mode;
//                       bool linearFalloff; } — explosions publish these, which is how a
// blast is supposed to throw debris outward. Ignoring them made explosions inert.
void WINAPI Shim_flexExtSetForceFields(void* container, const void* fields, int numFields,
                                       int)
{
    Note("flexExtSetForceFields");
    (void)container;

    std::lock_guard<std::mutex> lock(s_solverMutex);
    s_forceFields.clear();
    if (!fields || numFields <= 0 || numFields > 256)
        return;

    auto base = static_cast<const uint8_t*>(fields);
    for (int i = 0; i < numFields; ++i) {
        auto f = reinterpret_cast<const float*>(base + size_t(i) * kForceFieldSize);
        ForceField ff;
        ff.pos[0] = f[0];
        ff.pos[1] = f[1];
        ff.pos[2] = f[2];
        ff.radius = f[3];
        ff.strength = f[4];
        ff.linearFalloff = *reinterpret_cast<const int*>(f + 6) != 0;

        // Reject anything that does not look like a real blast rather than trusting a
        // possibly-wrong struct layout.
        if (!std::isfinite(ff.pos[0]) || !std::isfinite(ff.radius) || ff.radius <= 0.0f ||
            ff.radius > 100000.0f || !std::isfinite(ff.strength))
            continue;
        s_forceFields.push_back(ff);
    }

    static int logged = 0;
    if (!s_forceFields.empty() && logged < 4) {
        ++logged;
        const ForceField& f = s_forceFields[0];
        log::Write("flexshim: force field at (%.0f, %.0f, %.0f) radius=%.0f strength=%.1f "
                   "(%d of %d accepted)", f.pos[0], f.pos[1], f.pos[2], f.radius, f.strength,
                   int(s_forceFields.size()), numFields);
    }
}

struct Replacement {
    const char* name;
    void* fn;
};

const Replacement kReplacements[] = {
    {"flexInit", reinterpret_cast<void*>(&Shim_flexInit)},
    {"flexShutdown", reinterpret_cast<void*>(&Shim_flexShutdown)},
    {"flexAcquireContext", reinterpret_cast<void*>(&Shim_flexAcquireContext)},
    {"flexCreateSolver", reinterpret_cast<void*>(&Shim_flexCreateSolver)},
    {"flexDestroySolver", reinterpret_cast<void*>(&Shim_flexDestroySolver)},
    {"flexUpdateSolver", reinterpret_cast<void*>(&Shim_flexUpdateSolver)},
    {"flexSetParams", reinterpret_cast<void*>(&Shim_flexSetParams)},
    {"flexSetParticles", reinterpret_cast<void*>(&Shim_flexSetParticles)},
    {"flexGetParticles", reinterpret_cast<void*>(&Shim_flexGetParticles)},
    {"flexSetVelocities", reinterpret_cast<void*>(&Shim_flexSetVelocities)},
    {"flexGetVelocities", reinterpret_cast<void*>(&Shim_flexGetVelocities)},
    {"flexSetPhases", reinterpret_cast<void*>(&Shim_flexSetPhases)},
    {"flexSetActive", reinterpret_cast<void*>(&Shim_flexSetActive)},
    {"flexGetActiveCount", reinterpret_cast<void*>(&Shim_flexGetActiveCount)},
    {"flexSetRigids", reinterpret_cast<void*>(&Shim_flexSetRigids)},
    {"flexGetRigidTransforms", reinterpret_cast<void*>(&Shim_flexGetRigidTransforms)},
    {"flexGetBounds", reinterpret_cast<void*>(&Shim_flexGetBounds)},
    {"flexSetShapes", reinterpret_cast<void*>(&Shim_flexSetShapes)},
    {"flexCreateTriangleMesh", reinterpret_cast<void*>(&Shim_flexCreateTriangleMesh)},
    {"flexUpdateTriangleMesh", reinterpret_cast<void*>(&Shim_flexUpdateTriangleMesh)},
    {"flexDestroyTriangleMesh", reinterpret_cast<void*>(&Shim_flexDestroyTriangleMesh)},
    {"flexAlloc", reinterpret_cast<void*>(&Shim_flexAlloc)},
    {"flexFree", reinterpret_cast<void*>(&Shim_flexFree)},
    {"flexSetFence", reinterpret_cast<void*>(&Shim_flexSetFence)},
    {"flexWaitFence", reinterpret_cast<void*>(&Shim_flexWaitFence)},
    {"flexStartRecord", reinterpret_cast<void*>(&Shim_flexStartRecord)},
    {"flexStopRecord", reinterpret_cast<void*>(&Shim_flexStopRecord)},
    {"flexSnapshot", reinterpret_cast<void*>(&Shim_flexSnapshot)},
    {"flexExtCreateContainer", reinterpret_cast<void*>(&Shim_flexExtCreateContainer)},
    {"flexExtDestroyContainer", reinterpret_cast<void*>(&Shim_flexExtDestroyContainer)},
    {"flexExtCreateRigidFromMesh", reinterpret_cast<void*>(&Shim_flexExtCreateRigidFromMesh)},
    {"flexExtDestroyAsset", reinterpret_cast<void*>(&Shim_flexExtDestroyAsset)},
    {"flexExtGetParticleData", reinterpret_cast<void*>(&Shim_flexExtGetParticleData)},
    {"flexExtSetForceFields", reinterpret_cast<void*>(&Shim_flexExtSetForceFields)},
};

const void* FindReplacement(const char* importName)
{
    for (const Replacement& r : kReplacements)
        if (strcmp(r.name, importName) == 0)
            return r.fn;
    return nullptr;
}

// Walks Fallout4.exe's import descriptors and rewrites the thunks for the Flex DLLs.
int PatchImports()
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base)
        return 0;

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;

    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    int patched = 0;

    for (; desc->Name; ++desc) {
        auto dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_strnicmp(dllName, "flex", 4) != 0)
            continue;

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        auto orig = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));

        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                continue; // imported by ordinal; no name to match
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            const void* replacement = FindReplacement(byName->Name);
            if (!replacement)
                continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(thunk, sizeof(*thunk), PAGE_READWRITE, &oldProtect))
                continue;
            thunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(thunk, sizeof(*thunk), oldProtect, &oldProtect);
            ++patched;
        }
        log::Write("flexshim: patched imports from %s", dllName);
    }
    return patched;
}

} // namespace

bool Install()
{
    if (s_installed)
        return true;
    if (!scan::Init())
        return false;

    const int patched = PatchImports();
    if (patched == 0) {
        log::Write("flexshim: no Flex imports found to patch");
        return false;
    }

    s_installed = true;
    log::Write("flexshim: %d Flex entry points redirected — the game cannot enter the "
               "unusable CUDA 7.5 solver", patched);
    return true;
}

int InterceptedCalls()
{
    return s_calls.load(std::memory_order_relaxed);
}

int ActivePieces()
{
    std::lock_guard<std::mutex> lock(s_solverMutex);
    return s_rigids.count;
}

float LargestPieceRadius()
{
    std::lock_guard<std::mutex> lock(s_solverMutex);
    float r = 0.0f;
    for (int i = 0; i < s_rigids.count && i < int(s_rigids.radii.size()); ++i)
        r = std::max(r, s_rigids.radii[size_t(i)]);
    return r;
}

bool Installed()
{
    return s_installed;
}

}
