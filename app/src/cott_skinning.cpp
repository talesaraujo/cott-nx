// cott - the game's CPU skinning in native code (characters' vertices moved by their bones).
//
// WHAT IT IS (PowerPC read instruction by instruction in the generated code)
//   sub_82353478(mesh, matrices, remap, box): for each of mesh's vertices, the position times up to four
//   bone matrices, blended by the vertex weights, plus up to three direction vectors (normal and two more)
//   turned by the first bone; and the box around the new positions.
//   mesh: +8 flags, +12 vertex count, +24/+32 positions in/out, +28/+36 (flag 0x10), +40/+44 (0x800) and
//     +48/+52 (0x1000) directions in/out, +80 four bone bytes per vertex, +84 three weights per vertex
//     (0 = one bone with weight 1). All vectors are three floats, 12 bytes apart.
//   matrices: +8 the bone matrices (64 bytes, row-major, translation in the last row); remap: +8 one u32
//     matrix index per bone byte. box: min x y z, max x y z.
//   One vertex: P_k = p * M_k for the bones b0..b3; out = P_0 * w0, and when w0 != 1:
//     out += P_1 * w1, out += P_2 * w2, out += P_3 * (1 - (w2 + w1 + w0)), each as one fused multiply-add.
//
// WHY NATIVE
//   In the levels this was 24% of the game's main thread (the profile at 480p in handheld mode), and that
//   thread is what limits the frame rate when the GPU has room (docked, and the lower handheld scales).
//   The recompiled code keeps every register in the context, every load and store is volatile, it reloads
//   the mesh's pointers for each component, and it switches the FPU's flush-to-zero mode twice per vertex
//   (the box's vmaxfp/vminfp run with it on, the scalar code with it off), which writes FPCR each time.
//
// WHY IT IS IDENTICAL
//   - The same operations in the same order on each component, with the original's operand order, done
//     the way the recompiled code does them: fmadds as a double fma rounded to float, fmuls/fadds/fsubs
//     rounded to float, flush-to-zero off.
//   - The box: vmaxfp/vminfp are "a > b ? a : b" / "a < b ? a : b" with flush-to-zero on, so denormals
//     compare as zero and a NaN keeps the box as it was. The value kept is the unflushed one on the Switch
//     (NEON) and the flushed one on the PC (SSE with DAZ), as simde does it on each.
//   - Memory: the same stores in the same order. The directions are reread after each component is
//     stored, as the original does, so in-place meshes (out = in) give the same numbers. The original's
//     stack below r1 is dead after the return.
//   - Registers: it saves and restores r27-r31 and f25-f31 around its work; the caller does not read the
//     volatile ones. The FPU is left with flush-to-zero off, as the original leaves it.
//
// SELF-CHECKING GUARD (cvar cott_skinning_native; project rule)
//   The first kChecks calls, then 1 of every kPeriod: what the call will write is saved, the native version
//   runs, its results are copied aside, the memory is put back, the original runs for real and the two
//   are compared (bit for bit, except that any NaN equals any NaN). A difference logs "[skin] DIFFERENCE"
//   and turns the native version off for the run.
//   cott_skinning_selftest=true also runs both on random meshes at the first call (for the PC). On the PC,
//   2026-10-19: 300,000 random meshes (flags, counts, weights, in-place outputs, denormals, NaNs) and the
//   game's first 2048 calls, all identical.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

REXCVAR_DEFINE_BOOL(cott_skinning_native, true, "Performance",
                    "The game's CPU skinning (characters' vertices and bones) in native code, self-checked");
REXCVAR_DEFINE_BOOL(cott_skinning_selftest, false, "Debug",
                    "At the first skinning call, compare the native skinning with the original on random meshes");

namespace {

constexpr uint32_t kOne = 0x82011E4C;      // lis -32255 + 7756: 1.0f
constexpr uint32_t kMinStart = 0x8200E04C; // lis -32255 - 8116: the box's starting minimum
constexpr uint32_t kMaxStart = 0x82035F74; // lis -32253 + 24436: the box's starting maximum
constexpr uint32_t kChecks = 2048;
constexpr uint32_t kPeriod = 512;

std::atomic<uint32_t> g_calls{0};
std::atomic<bool> g_failed{false};

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}

inline float LoadF(const uint8_t* base, uint32_t address) { return std::bit_cast<float>(Load32(base, address)); }

inline void StoreF(uint8_t* base, uint32_t address, float value) {
  const uint32_t swapped = __builtin_bswap32(std::bit_cast<uint32_t>(value));
  std::memcpy(base + address, &swapped, 4);
}

// The recompiled code's arithmetic (see "WHY IT IS IDENTICAL").
inline float Mul(float a, float b) { return float(double(a) * double(b)); }
inline float Add(float a, float b) { return float(double(a) + double(b)); }
inline float Sub(float a, float b) { return float(double(a) - double(b)); }
inline float Fma(float a, float b, float c) { return float(std::fma(double(a), double(b), double(c))); }

// vmaxfp/vminfp compare with flush-to-zero on.
inline float Flushed(float x) {
  const uint32_t bits = std::bit_cast<uint32_t>(x);
  return (bits & 0x7F800000) == 0 ? std::bit_cast<float>(bits & 0x80000000) : x;
}

// What vmaxfp/vminfp keep: simde's NEON version selects the unflushed bits (vbslq on a vcgtq mask), while
// SSE's maxps/minps with DAZ give the flushed value.
#if defined(__aarch64__)
inline float Kept(float x) { return x; }
#else
inline float Kept(float x) { return Flushed(x); }
#endif

struct Mesh {
  uint32_t flags, count, pos_in, dir_in[3], pos_out, dir_out[3], bones, weights;
};

Mesh ReadMesh(const uint8_t* base, uint32_t mesh) {
  Mesh m;
  m.flags = Load32(base, mesh + 8);
  m.count = Load32(base, mesh + 12);
  m.pos_in = Load32(base, mesh + 24);
  m.pos_out = Load32(base, mesh + 32);
  m.dir_in[0] = Load32(base, mesh + 28);
  m.dir_out[0] = Load32(base, mesh + 36);
  m.dir_in[1] = Load32(base, mesh + 40);
  m.dir_in[2] = Load32(base, mesh + 48);
  m.dir_out[1] = Load32(base, mesh + 44);
  m.dir_out[2] = Load32(base, mesh + 52);
  m.bones = Load32(base, mesh + 80);
  m.weights = Load32(base, mesh + 84);
  return m;
}

constexpr uint32_t kDirFlags[3] = {0x10, 0x800, 0x1000};

struct Bone {
  float m[16];
};

inline void LoadBone(const uint8_t* base, uint32_t matrices, uint32_t remap, uint32_t bone, Bone& out) {
  const uint32_t address = matrices + (Load32(base, remap + bone * 4) << 6);
  for (int i = 0; i < 16; ++i) out.m[i] = LoadF(base, address + 4 * i);
}

struct V3 {
  float x, y, z;
};

// One bone's position: the three components each in the order the original computes them.
inline V3 Transform(const float* m, V3 p) {
  return {Add(Fma(p.x, m[0], Fma(m[4], p.y, Mul(m[8], p.z))), m[12]),
          Add(Fma(m[9], p.z, Fma(m[1], p.x, Mul(m[5], p.y))), m[13]),
          Add(Fma(m[10], p.z, Fma(m[2], p.x, Mul(m[6], p.y))), m[14])};
}

inline V3 LoadV3(const uint8_t* base, uint32_t address) {
  return {LoadF(base, address), LoadF(base, address + 4), LoadF(base, address + 8)};
}

void Skin(uint8_t* base, uint32_t mesh_address, uint32_t matrix_holder, uint32_t remap_holder, uint32_t box) {
  const Mesh mesh = ReadMesh(base, mesh_address);
  const uint32_t matrices = Load32(base, matrix_holder + 8), remap = Load32(base, remap_holder + 8);
  const float one = LoadF(base, kOne);
  float lo[3], hi[3];
  lo[0] = lo[1] = lo[2] = LoadF(base, kMinStart);
  hi[0] = hi[1] = hi[2] = LoadF(base, kMaxStart);

  Bone b[4];
  for (uint32_t i = 0; i < mesh.count; ++i) {
    const uint32_t at = 12 * i, bones = mesh.bones + 4 * i;
    LoadBone(base, matrices, remap, base[bones], b[0]);
    const float* m = b[0].m;
    for (int k = 0; k < 3; ++k) {
      if (!(mesh.flags & kDirFlags[k])) continue;
      const uint32_t in = mesh.dir_in[k] + at, out = mesh.dir_out[k] + at;
      for (int c = 0; c < 3; ++c) {
        const float y = LoadF(base, in + 4), z = LoadF(base, in + 8), x = LoadF(base, in);
        // The original's operand order: x * m0, but m1 * x and m2 * x.
        const float t = Fma(z, m[8 + c], Mul(y, m[4 + c]));
        StoreF(base, out + 4 * c, c == 0 ? Fma(x, m[0], t) : Fma(m[c], x, t));
      }
    }

    const uint32_t weights = mesh.weights + at;
    const float w0 = mesh.weights ? LoadF(base, weights) : one;
    const V3 p = LoadV3(base, mesh.pos_in + at);
    const V3 p0 = Transform(m, p);
    V3 sum = {Mul(p0.x, w0), Mul(p0.y, w0), Mul(p0.z, w0)};
    if (!(w0 == one)) {
      const float w1 = LoadF(base, weights + 4), w2 = LoadF(base, weights + 8);
      const float w3 = Sub(one, Add(Add(w2, w1), LoadF(base, weights)));
      const float w[3] = {w1, w2, w3};
      for (int k = 1; k < 4; ++k) {
        LoadBone(base, matrices, remap, base[bones + k], b[k]);
        const V3 pk = Transform(b[k].m, p);
        sum = {Fma(pk.x, w[k - 1], sum.x), Fma(pk.y, w[k - 1], sum.y), Fma(pk.z, w[k - 1], sum.z)};
      }
    }

    const float v[3] = {sum.x, sum.y, sum.z};
    for (int c = 0; c < 3; ++c) {
      hi[c] = Kept(Flushed(v[c]) > Flushed(hi[c]) ? v[c] : hi[c]);
      lo[c] = Kept(Flushed(v[c]) < Flushed(lo[c]) ? v[c] : lo[c]);
    }
    StoreF(base, mesh.pos_out + at, sum.x);
    StoreF(base, mesh.pos_out + at + 4, sum.y);
    StoreF(base, mesh.pos_out + at + 8, sum.z);
  }
  for (int c = 0; c < 3; ++c) {
    StoreF(base, box + 4 * c, lo[c]);
    StoreF(base, box + 12 + 4 * c, hi[c]);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82353478);

namespace {

struct Region {
  uint32_t address, size;
};

// A big-endian float word that is a NaN. Which NaN comes out of an operation with two NaN inputs depends
// on the order the compiler gives the operands, so all NaNs count as the same.
inline bool IsNan(uint32_t word) { return (__builtin_bswap32(word) & 0x7FFFFFFF) > 0x7F800000; }

// Runs both versions from the same memory and compares what they wrote. The original's results stay.
bool Compare(PPCContext& ctx, uint8_t* base, bool log) {
  const uint32_t mesh_address = ctx.r3.u32, matrices = ctx.r4.u32, remap = ctx.r5.u32, box = ctx.r6.u32;
  const Mesh mesh = ReadMesh(base, mesh_address);
  std::vector<Region> regions = {{box, 24}};
  if (mesh.count) {
    regions.push_back({mesh.pos_out, 12 * mesh.count});
    for (int k = 0; k < 3; ++k)
      if (mesh.flags & kDirFlags[k]) regions.push_back({mesh.dir_out[k], 12 * mesh.count});
  }
  std::vector<std::vector<uint8_t>> before, native;
  for (const Region& r : regions) before.emplace_back(base + r.address, base + r.address + r.size);
  ctx.fpscr.disableFlushMode();
  Skin(base, mesh_address, matrices, remap, box);
  for (const Region& r : regions) native.emplace_back(base + r.address, base + r.address + r.size);
  for (size_t i = regions.size(); i-- > 0;) std::memcpy(base + regions[i].address, before[i].data(), regions[i].size);
  __imp__sub_82353478(ctx, base);
  for (size_t i = 0; i < regions.size(); ++i) {
    const Region& r = regions[i];
    if (std::memcmp(native[i].data(), base + r.address, r.size) == 0) continue;
    uint32_t at = 0, got = 0, want = 0;
    for (; at < r.size; at += 4) {
      std::memcpy(&got, native[i].data() + at, 4);
      std::memcpy(&want, base + r.address + at, 4);
      if (got != want && !(IsNan(got) && IsNan(want))) break;
    }
    if (at >= r.size) continue;
    if (log) {
      REXLOG_ERROR("[skin] DIFFERENCE: mesh {:08X} flags {:X} {} vertices, region {:08X} + {}: native {:08X}, "
                   "original {:08X}; native off",
                   mesh_address, mesh.flags, mesh.count, r.address, at, __builtin_bswap32(got),
                   __builtin_bswap32(want));
    }
    return false;
  }
  return true;
}

float RandomFloat(std::mt19937& rng) {
  switch (rng() % 16) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return std::bit_cast<float>(uint32_t(rng() & 0x807FFFFF));  // denormal
    case 3: return std::bit_cast<float>(uint32_t(rng()));                // anything, NaN included
    default: return std::uniform_real_distribution<float>(-8.0f, 8.0f)(rng);
  }
}

void SelfTest(PPCContext& ctx, uint8_t* base) {
  auto* memory = rex::system::kernel_memory();
  constexpr uint32_t kBytes = 0xB000, kMaxVertices = 256;
  const uint32_t block = memory ? memory->SystemHeapAlloc(kBytes, 16) : 0;
  if (!block) return;
  const uint64_t saved[4] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64};
  std::mt19937 rng(1234);
  auto put32 = [&](uint32_t address, uint32_t value) {
    value = __builtin_bswap32(value);
    std::memcpy(base + address, &value, 4);
  };
  constexpr int kMeshes = 20000;
  int same = 0;
  for (int t = 0; t < kMeshes; ++t) {
    const uint32_t mesh = block, holder4 = block + 0x100, holder5 = block + 0x120, box = block + 0x140;
    const uint32_t matrices = block + 0x1000, remap = block + 0x1400, bones = block + 0x1500;
    const uint32_t weights = block + 0x2000, arrays = block + 0x3000;
    for (uint32_t a = matrices; a < matrices + 16 * 64; a += 4) StoreF(base, a, RandomFloat(rng));
    for (uint32_t i = 0; i < 16; ++i) put32(remap + 4 * i, rng() % 16);
    for (uint32_t i = 0; i < 4 * kMaxVertices; ++i) base[bones + i] = uint8_t(rng() % 16);
    for (uint32_t i = 0; i < 3 * kMaxVertices; ++i) {
      const bool whole = i % 3 == 0 && rng() % 4 == 0;
      StoreF(base, weights + 4 * i, whole ? 1.0f : std::uniform_real_distribution<float>(0.0f, 1.0f)(rng));
    }
    for (uint32_t a = arrays; a < arrays + 8 * 0x1000; a += 4) StoreF(base, a, RandomFloat(rng));
    std::memset(base + mesh, 0, 0x100);
    put32(mesh + 8, rng() & 0x1810);
    put32(mesh + 12, rng() % 4 == 0 ? rng() % 3 : rng() % kMaxVertices);
    // +24..+52: positions in, normals in, positions out, normals out, then two more (in, out) pairs.
    // An out is sometimes its own in (in place).
    constexpr uint32_t kInOf[8] = {0, 1, 0, 1, 4, 4, 6, 6};
    for (uint32_t k = 0; k < 8; ++k)
      put32(mesh + 24 + 4 * k, arrays + 0x1000 * (kInOf[k] != k && rng() % 4 == 0 ? kInOf[k] : k));
    put32(mesh + 80, bones);
    put32(mesh + 84, rng() % 5 == 0 ? 0 : weights);
    put32(holder4 + 8, matrices);
    put32(holder5 + 8, remap);
    ctx.r3.u64 = mesh;
    ctx.r4.u64 = holder4;
    ctx.r5.u64 = holder5;
    ctx.r6.u64 = box;
    if (Compare(ctx, base, same == t)) ++same;
  }
  ctx.r3.u64 = saved[0];
  ctx.r4.u64 = saved[1];
  ctx.r5.u64 = saved[2];
  ctx.r6.u64 = saved[3];
  memory->SystemHeapFree(block);
  REXLOG_INFO("[skin] self-test: {} of {} random meshes identical", same, kMeshes);
}

}  // namespace

REX_HOOK_RAW(sub_82353478) {
  if (!REXCVAR_GET(cott_skinning_native) || g_failed.load(std::memory_order_relaxed)) {
    __imp__sub_82353478(ctx, base);
    return;
  }
  const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed);
  if (n == 0 && REXCVAR_GET(cott_skinning_selftest)) SelfTest(ctx, base);
  if (n < kChecks || n % kPeriod == 0) {
    if (!Compare(ctx, base, true)) g_failed.store(true);
    else if (n == kChecks - 1) REXLOG_INFO("[skin] the first {} calls identical to the original", kChecks);
    return;
  }
  ctx.fpscr.disableFlushMode();
  Skin(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
