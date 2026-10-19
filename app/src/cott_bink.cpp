// cott - the Bink video decoder's hottest functions in native code (the game's opening videos).
//
// WHAT THEY ARE (PowerPC read instruction by instruction in the generated code)
//   sub_8243EDE0(block, bits): reads one block's DCT coefficients from the bitstream (Bink's
//     read_dct_coeffs, also in FFmpeg's bink.c). bits points to the reader state: the unused bits of the
//     current word (lowest first), the address of the next big-endian word and how many bits are left.
//     4 bits give the largest coefficient size n; then, for n down to 1, one pass over a list of
//     (coefficient << 2 | mode) bytes that starts as {4, 24, 44 mode 0; 1, 2, 3 mode 3}: a set flag bit
//     splits groups (modes 0, 1, 2) or reads a coefficient (mode 3: n - 1 magnitude bits under a set top
//     bit, then the sign). The coefficients go into a zeroed int16 stack table in coefficient order, and
//     at the end the state is written back and the table is copied into block (halfwords 1-63, two at a
//     time through a fixed order). Called three times by the block decoder sub_82436E38.
//   sub_8243EA58(dst, dst_stride, coefs, quant_index, pred, pred_stride): inverse DCT of one 8x8 block of
//     16-bit coefficients, added to an 8x8 block of prediction bytes (the inter blocks). The quantizer is
//     the 64-int table at 0x82086AA8 + quant_index * 256. Called by the block decoder sub_82436E38.
//   sub_8243E370(dst, dst_stride, coefs, quant_table): the same transform stored as is (the intra blocks);
//     sub_8243EA28 jumps to it with the table at 0x82085AA0 + quant_index * 256.
//   Both are two passes of the same integer butterfly (constants 2896, 3784, -5352, 2217, >> 11):
//   1. Columns: a_k = (coef[k][c] * quant[k][c]) >> 11, then the butterfly, into an 8x8 int stack table.
//      A column with no AC coefficients takes a shortcut that gives the same numbers (all a_0).
//   2. Rows: the butterfly on each row of that table, then ((x + 127) >> 8), plus the prediction byte in
//      sub_8243EA58, stored with stb (the low 8 bits, no clamping).
//   All arithmetic is 32-bit and wraps (mullw, mulli, add, subf); the shifts are arithmetic (srawi).
//
// WHY NATIVE
//   The opening videos are decoded on the game's main thread, and at 576p in handheld mode the two IDCTs
//   were 25% of it (22% + 3%) and the coefficient reader 21%, next to the block copies (sub_82436E38,
//   10%). The recompiled code keeps every register in the context and every load and store is volatile.
//   Here the IDCT's 8 columns go through the butterfly together in 128-bit vectors (NEON on the Switch),
//   with one transpose between the passes and one before the stores, and the reader keeps its state,
//   list and table in registers and on the host stack.
//
// WHY IT IS IDENTICAL
//   - The same integer operations in the same order, on 32-bit lanes that wrap like the PowerPC
//     registers; the shortcut for empty columns is skipped because it gives the same values.
//   - Memory: only the 64 destination bytes are written, a row at a time in row order, and each row's
//     prediction bytes are read just before that row is written, as the original does (the prediction
//     can be the destination). The original's stack table lies below r1 and is dead after the return.
//   - The reader does the same bit operations on every path (also when the leftover bits have garbage
//     above their count), and writes the same 12 state bytes and the same 126 block bytes.
//   - Registers: the three functions only save and restore r14-r31 around their work; what they leave in the
//     volatile registers (r3-r12, ctr, cr) is not read by the caller, which reloads everything it needs.
//
// SELF-CHECKING GUARD (cvar cott_bink_native; project rule)
//   The first kChecks calls of each function, then 1 of every kPeriod: the native result is computed into
//   local buffers, the original runs for real, and everything it wrote is compared (the 64 pixels, or
//   the block and the reader state). A difference logs "[bink] DIFFERENCE" and turns that function's
//   native version off for the rest of the run.
//   Tested on the PC too: the recompiled originals and these versions on millions of random blocks
//   (random coefficients, quantizers, strides, bitstreams and reader states), with no difference.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <atomic>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(cott_bink_native, true, "Performance",
                    "Bink video decoding (inverse DCTs, coefficient reader) in native code, self-checked");

namespace {

using v4u = uint32_t __attribute__((vector_size(16)));
using v4s = int32_t __attribute__((vector_size(16)));

constexpr uint32_t kQuantAdd = 0x82086AA8;  // sub_8243EA58's table: lis -32248 + 27304
constexpr uint32_t kChecks = 4096;
constexpr uint32_t kPeriod = 1024;

struct Guard {
  const char* name;
  std::atomic<uint32_t> calls{0};
  std::atomic<bool> failed{false};

  bool Off() const { return !REXCVAR_GET(cott_bink_native) || failed.load(std::memory_order_relaxed); }
  bool ShouldCheck() {
    const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
    return n < kChecks || n % kPeriod == 0;
  }
};

Guard g_add{"sub_8243EA58"}, g_put{"sub_8243E370"}, g_reader{"sub_8243EDE0"};

inline v4u Sra(v4u x, int n) { return v4u(v4s(x) >> n); }

struct Vec8 {
  v4u lo, hi;
};

inline Vec8 operator+(Vec8 a, Vec8 b) { return {a.lo + b.lo, a.hi + b.hi}; }
inline Vec8 operator-(Vec8 a, Vec8 b) { return {a.lo - b.lo, a.hi - b.hi}; }
inline Vec8 operator*(Vec8 a, uint32_t k) { return {a.lo * k, a.hi * k}; }
inline Vec8 Sra(Vec8 x, int n) { return {Sra(x.lo, n), Sra(x.hi, n)}; }

// The butterfly shared by both passes: in[0..7] -> out[0..7].
__attribute__((always_inline)) inline void Butterfly(const Vec8 a[8], Vec8 out[8]) {
  const Vec8 s26 = a[2] + a[6], d26 = a[2] - a[6];
  const Vec8 e = Sra(d26 * 2896u, 11) - s26;
  const Vec8 s04 = a[0] + a[4], d04 = a[0] - a[4];
  const Vec8 t0 = s04 + s26, t3 = s04 - s26, t1 = d04 + e, t2 = d04 - e;
  const Vec8 d53 = a[5] - a[3], s53 = a[5] + a[3], d17 = a[1] - a[7], s17 = a[1] + a[7];
  const Vec8 o0 = s17 + s53;
  const Vec8 z = Sra((d17 + d53) * 3784u, 11);
  const Vec8 w = Sra((s17 - s53) * 2896u, 11);
  const Vec8 o1 = (Sra(d53 * uint32_t(-5352), 11) - o0) + z;
  const Vec8 o2 = w - o1;
  const Vec8 o3 = (Sra(d17 * 2217u, 11) - z) + o2;
  out[0] = t0 + o0;
  out[7] = t0 - o0;
  out[1] = t1 + o1;
  out[6] = t1 - o1;
  out[2] = t2 + o2;
  out[5] = t2 - o2;
  out[3] = t3 - o3;
  out[4] = t3 + o3;
}

inline void Transpose4(v4u& a, v4u& b, v4u& c, v4u& d) {
  const v4u t0 = __builtin_shuffle(a, b, v4u{0, 4, 1, 5});
  const v4u t1 = __builtin_shuffle(a, b, v4u{2, 6, 3, 7});
  const v4u t2 = __builtin_shuffle(c, d, v4u{0, 4, 1, 5});
  const v4u t3 = __builtin_shuffle(c, d, v4u{2, 6, 3, 7});
  a = __builtin_shuffle(t0, t2, v4u{0, 1, 4, 5});
  b = __builtin_shuffle(t0, t2, v4u{2, 3, 6, 7});
  c = __builtin_shuffle(t1, t3, v4u{0, 1, 4, 5});
  d = __builtin_shuffle(t1, t3, v4u{2, 3, 6, 7});
}

inline void Transpose8(Vec8 m[8]) {
  Transpose4(m[0].lo, m[1].lo, m[2].lo, m[3].lo);
  Transpose4(m[0].hi, m[1].hi, m[2].hi, m[3].hi);
  Transpose4(m[4].lo, m[5].lo, m[6].lo, m[7].lo);
  Transpose4(m[4].hi, m[5].hi, m[6].hi, m[7].hi);
  for (int i = 0; i < 4; ++i) {
    const v4u t = m[i].hi;
    m[i].hi = m[i + 4].lo;
    m[i + 4].lo = t;
  }
}

// Both passes; rows[r] lane k is ((x + 127) >> 8) of output row r, column k.
inline void Idct(const uint8_t* coefs, const uint8_t* quant, Vec8 rows[8]) {
  Vec8 a[8];
  for (int k = 0; k < 8; ++k) {
    uint16_t c16[8];
    uint32_t q32[8];
    std::memcpy(c16, coefs + k * 16, 16);
    std::memcpy(q32, quant + k * 32, 32);
    uint32_t c[8], q[8];
    for (int i = 0; i < 8; ++i) {
      c[i] = uint32_t(int32_t(int16_t(__builtin_bswap16(c16[i]))));
      q[i] = __builtin_bswap32(q32[i]);
    }
    Vec8 cv, qv;
    std::memcpy(&cv.lo, c, 16);
    std::memcpy(&cv.hi, c + 4, 16);
    std::memcpy(&qv.lo, q, 16);
    std::memcpy(&qv.hi, q + 4, 16);
    a[k] = Sra(Vec8{cv.lo * qv.lo, cv.hi * qv.hi}, 11);
  }
  Vec8 columns[8];
  Butterfly(a, columns);  // columns[r] lane c = table[r][c]
  Transpose8(columns);    // columns[k] lane r = table[r][k]
  Butterfly(columns, rows);
  const v4u round = {127, 127, 127, 127};
  for (int k = 0; k < 8; ++k) rows[k] = Sra(Vec8{rows[k].lo + round, rows[k].hi + round}, 8);
  Transpose8(rows);       // rows[r] lane k = output[r][k]
}

inline void LowBytes(const Vec8& row, uint8_t out[8]) {
  uint32_t v[8];
  std::memcpy(v, &row.lo, 16);
  std::memcpy(v + 4, &row.hi, 16);
  for (int i = 0; i < 8; ++i) out[i] = uint8_t(v[i]);
}

// dst/pred are host pointers; out (when not null) receives the 64 bytes instead of dst.
void IdctAdd(const uint8_t* coefs, const uint8_t* quant, uint8_t* dst, uint32_t stride, const uint8_t* pred,
             uint32_t pred_stride, uint8_t* out) {
  Vec8 rows[8];
  Idct(coefs, quant, rows);
  for (int r = 0; r < 8; ++r) {
    uint8_t bytes[8], p[8];
    LowBytes(rows[r], bytes);
    std::memcpy(p, pred + size_t(r) * pred_stride, 8);
    for (int i = 0; i < 8; ++i) bytes[i] = uint8_t(bytes[i] + p[i]);
    std::memcpy(out ? out + r * 8 : dst + size_t(r) * stride, bytes, 8);
  }
}

void IdctPut(const uint8_t* coefs, const uint8_t* quant, uint8_t* dst, uint32_t stride, uint8_t* out) {
  Vec8 rows[8];
  Idct(coefs, quant, rows);
  for (int r = 0; r < 8; ++r) {
    uint8_t bytes[8];
    LowBytes(rows[r], bytes);
    std::memcpy(out ? out + r * 8 : dst + size_t(r) * stride, bytes, 8);
  }
}

uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}

// The bit reader at [r4]: the bits not used yet of the current word (lowest first), the next word and
// how many bits are left. The same operations as the PowerPC on every path, including when the
// leftover value has bits above its count.
struct BitReader {
  const uint8_t* base;
  uint32_t value, address, count;

  uint32_t Bit() {
    if (count == 0) {
      const uint32_t word = LoadBE32(base, address);
      address += 4;
      count = 31;
      value = word >> 1;
      return word & 1;
    }
    const uint32_t bit = value & 1;
    value >>= 1;
    --count;
    return bit;
  }
  // n in 2..16; the caller masks the result.
  uint32_t Read(uint32_t n) {
    if (count >= n) {
      const uint32_t bits = value;
      value >>= n;
      count -= n;
      return bits;
    }
    const uint32_t word = LoadBE32(base, address);
    address += 4;
    const uint32_t bits = value | (word << count);
    value = word >> (n - count);
    count = count + 32 - n;
    return bits;
  }
  // A coefficient of n bits: n - 1 bits of magnitude below a top bit that is always set, then the sign.
  int32_t Coefficient(uint32_t n) {
    if (n == 1) return Bit() ? -1 : 1;
    const uint32_t bits = Read(n), top = 1u << (n - 1);
    const int32_t magnitude = int32_t((bits & (top - 1)) | top);
    return (bits & top) ? -magnitude : magnitude;
  }
};

// sub_8243EDE0's copy from its stack table (coefficient order) to the output block, two coefficients at a
// time: output byte offset <- table byte offset. Halfword 0 (the DC) is not written; halfword 1 is.
constexpr uint8_t kPairs[31][2] = {
    {4, 8},     {8, 16},    {12, 24},   {16, 4},    {20, 12},   {24, 20},   {28, 28},  {32, 48},
    {36, 88},   {40, 32},   {44, 40},   {48, 52},   {52, 92},   {56, 36},   {60, 44},  {64, 56},
    {68, 64},   {72, 96},   {76, 104},  {80, 60},   {84, 68},   {88, 100},  {92, 108}, {96, 72},
    {100, 80},  {104, 112}, {108, 120}, {112, 76},  {116, 84},  {120, 116}, {124, 124}};

// out: host pointer to the 128-byte block (big-endian halfwords); state: host pointer to [r4].
void ReadCoefficients(const uint8_t* base, uint8_t* state, uint8_t* out) {
  BitReader bits{base, LoadBE32(state, 0), LoadBE32(state, 4), LoadBE32(state, 8)};
  int16_t table[80] = {};  // 64, plus room for the indices a damaged stream can reach (up to 66)
  uint8_t list[256];       // (coefficient << 2) | mode; new mode-3 entries go below the start
  uint32_t start = 128, end = 128;
  for (uint8_t entry : {16, 96, 176, 7, 11, 15}) list[end++] = entry;

  for (uint32_t n = bits.Read(4) & 15; n >= 1; --n) {
    for (uint32_t pos = start; pos < end;) {
      const uint8_t entry = list[pos];
      if (entry == 0 || !bits.Bit()) {
        ++pos;
        continue;
      }
      uint32_t coef = entry >> 2;
      switch (entry & 3) {
        case 0:
          list[pos] = uint8_t((coef << 2) + 17);  // coef + 4, mode 1
          break;
        case 1: {
          const uint8_t group = entry & ~3;
          list[pos] = uint8_t(group + 2);
          list[end++] = uint8_t(group + 18);
          list[end++] = uint8_t(group + 34);
          list[end++] = uint8_t(group + 50);
          continue;  // the same entry again, now mode 2
        }
        case 2:
          list[pos++] = 0;
          break;
        default:
          table[coef] = int16_t(bits.Coefficient(n));
          list[pos++] = 0;
          continue;
      }
      for (int i = 0; i < 4; ++i, ++coef) {
        if (bits.Bit()) {
          list[--start] = uint8_t((coef << 2) + 3);
        } else {
          table[coef] = int16_t(bits.Coefficient(n));
        }
      }
    }
  }

  const uint32_t words[3] = {__builtin_bswap32(bits.value), __builtin_bswap32(bits.address),
                             __builtin_bswap32(bits.count)};
  std::memcpy(state, words, 12);
  uint16_t be[64];
  for (int i = 0; i < 64; ++i) be[i] = __builtin_bswap16(uint16_t(table[i]));
  const uint8_t* from = reinterpret_cast<const uint8_t*>(be);
  std::memcpy(out + 2, from + 2, 2);
  for (const auto& pair : kPairs) std::memcpy(out + pair[0], from + pair[1], 4);
}

void Report(Guard& guard, uint32_t dst, uint32_t stride, uint32_t coefs, uint32_t quant,
            const uint8_t* got, const uint8_t* expected) {
  guard.failed.store(true);
  REXLOG_ERROR("[bink] DIFFERENCE in {}: dst {:08X} stride {} coefs {:08X} quant {:08X}; native off", guard.name, dst,
               stride, coefs, quant);
  for (int r = 0; r < 8; ++r) {
    REXLOG_ERROR("[bink]   row {}: native {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}, original "
                 "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                 r, got[r * 8], got[r * 8 + 1], got[r * 8 + 2], got[r * 8 + 3], got[r * 8 + 4], got[r * 8 + 5],
                 got[r * 8 + 6], got[r * 8 + 7], expected[r * 8], expected[r * 8 + 1], expected[r * 8 + 2],
                 expected[r * 8 + 3], expected[r * 8 + 4], expected[r * 8 + 5], expected[r * 8 + 6],
                 expected[r * 8 + 7]);
  }
}

bool Same(const uint8_t* got, const uint8_t* dst, uint32_t stride, uint8_t* expected) {
  for (int r = 0; r < 8; ++r) std::memcpy(expected + r * 8, dst + size_t(r) * stride, 8);
  return std::memcmp(got, expected, 64) == 0;
}

}  // namespace

REX_EXTERN(__imp__sub_8243EA58);
REX_HOOK_RAW(sub_8243EA58) {
  if (g_add.Off()) {
    __imp__sub_8243EA58(ctx, base);
    return;
  }
  const uint32_t dst = ctx.r3.u32, stride = ctx.r4.u32, coefs = ctx.r5.u32;
  const uint32_t quant = kQuantAdd + (ctx.r6.u32 << 8), pred = ctx.r7.u32, pred_stride = ctx.r8.u32;
  if (g_add.ShouldCheck()) {
    uint8_t got[64], expected[64];
    IdctAdd(base + coefs, base + quant, nullptr, 0, base + pred, pred_stride, got);
    __imp__sub_8243EA58(ctx, base);
    if (!Same(got, base + dst, stride, expected)) Report(g_add, dst, stride, coefs, quant, got, expected);
    return;
  }
  IdctAdd(base + coefs, base + quant, base + dst, stride, base + pred, pred_stride, nullptr);
}

REX_EXTERN(__imp__sub_8243E370);
REX_HOOK_RAW(sub_8243E370) {
  if (g_put.Off()) {
    __imp__sub_8243E370(ctx, base);
    return;
  }
  const uint32_t dst = ctx.r3.u32, stride = ctx.r4.u32, coefs = ctx.r5.u32, quant = ctx.r6.u32;
  if (g_put.ShouldCheck()) {
    uint8_t got[64], expected[64];
    IdctPut(base + coefs, base + quant, nullptr, 0, got);
    __imp__sub_8243E370(ctx, base);
    if (!Same(got, base + dst, stride, expected)) Report(g_put, dst, stride, coefs, quant, got, expected);
    return;
  }
  IdctPut(base + coefs, base + quant, base + dst, stride, nullptr);
}

REX_EXTERN(__imp__sub_8243EDE0);
REX_HOOK_RAW(sub_8243EDE0) {
  if (g_reader.Off()) {
    __imp__sub_8243EDE0(ctx, base);
    return;
  }
  const uint32_t block = ctx.r3.u32, state = ctx.r4.u32;
  if (g_reader.ShouldCheck()) {
    uint8_t got_block[128], got_state[12];
    std::memcpy(got_block, base + block, 128);
    std::memcpy(got_state, base + state, 12);
    ReadCoefficients(base, got_state, got_block);
    __imp__sub_8243EDE0(ctx, base);
    if (std::memcmp(got_block, base + block, 128) != 0 || std::memcmp(got_state, base + state, 12) != 0) {
      g_reader.failed.store(true);
      REXLOG_ERROR("[bink] DIFFERENCE in sub_8243EDE0: block {:08X} state {:08X} (block {}, state {}); native off",
                   block, state, std::memcmp(got_block, base + block, 128) != 0 ? "differs" : "same",
                   std::memcmp(got_state, base + state, 12) != 0 ? "differs" : "same");
    }
    return;
  }
  ReadCoefficients(base, base + state, base + block);
}
