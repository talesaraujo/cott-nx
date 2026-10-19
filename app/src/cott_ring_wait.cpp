// cott - measures the game's waits for room in the Direct3D ring (diagnostic, off by default).
//
// sub_8226C958(device, position, kind) is the game's D3D waiting until the GPU's read pointer
// (*[device+10768]) has passed `position`: it kicks what is pending and then spins in sub_8226D968
// (db16cyc, 5000-tick timeout). On the Switch at 480p it is ~41% of the main thread, while the
// command thread is ~54% idle waiting for the game: the two take turns. Twelve places call it; this
// hook says which ones wait, how often and for how long, and how far ahead the game was.
//
// sub_8226D968 itself is also timed per call site: it is one short spin, called in a loop by
// sub_8226C958 and by five other D3D functions that wait on their own.
//
// cott_ring_wait_stats=true: every 10 s, one line per call site (the guest return address).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

REXCVAR_DEFINE_BOOL(cott_ring_wait_stats, false, "Debug",
                    "Every 10 s, log where the game waits for room in its Direct3D ring and for how long");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return rex::byte_swap(value);
}

struct Site {
  uint32_t caller = 0;
  uint64_t calls = 0;
  uint64_t waits = 0;  // calls that took more than 20 us
  uint64_t ns = 0;
  uint64_t max_ns = 0;
  uint64_t ahead = 0;  // sum of (pending end - read pointer) at entry, in bytes
  uint32_t last_position = 0, last_end = 0, last_read = 0, last_kind = 0;
};

std::mutex g_mutex;
std::array<Site, 16> g_sites;
std::array<Site, 16> g_spins;  // sub_8226D968: calls and time only
auto g_report_start = std::chrono::steady_clock::now();

void Report(std::chrono::steady_clock::time_point now) {
  const double seconds = std::chrono::duration<double>(now - g_report_start).count();
  std::vector<Site> sites;
  for (Site& site : g_sites) {
    if (site.calls) sites.push_back(site);
    site = Site{};
  }
  g_report_start = now;
  std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.ns > b.ns; });
  double total_ms = 0.0;
  for (const Site& site : sites) total_ms += site.ns * 1e-6;
  std::vector<Site> spins;
  for (Site& spin : g_spins) {
    if (spin.calls) spins.push_back(spin);
    spin = Site{};
  }
  std::sort(spins.begin(), spins.end(), [](const Site& a, const Site& b) { return a.ns > b.ns; });
  for (const Site& spin : spins) {
    REXLOG_INFO("[ring wait] spin from {:08X}: {:.0f} calls/s, {:.1f} ms/s", spin.caller,
                spin.calls / seconds, spin.ns * 1e-6 / seconds);
  }
  REXLOG_INFO("[ring wait] {:.0f} ms per second waiting, {} call sites", total_ms / seconds,
              sites.size());
  for (const Site& site : sites) {
    REXLOG_INFO("[ring wait]   from {:08X}: {:.0f} calls/s, {:.0f} waits/s, {:.1f} ms/s (longest {:.2f} ms), "
                "{:.0f} KB ahead on average; last: kind {} position {:08X} end {:08X} read {:08X}",
                site.caller, site.calls / seconds, site.waits / seconds, site.ns * 1e-6 / seconds,
                site.max_ns * 1e-6, site.calls ? double(site.ahead) / site.calls / 1024.0 : 0.0,
                site.last_kind, site.last_position, site.last_end, site.last_read);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_8226C958);
REX_HOOK_RAW(sub_8226C958) {
  if (!REXCVAR_GET(cott_ring_wait_stats)) {
    __imp__sub_8226C958(ctx, base);
    return;
  }
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t device = ctx.r3.u32;
  const uint32_t position = ctx.r4.u32;
  const uint32_t kind = ctx.r5.u32;
  const uint32_t end = Load32(base, device + 10780);
  const uint32_t read = Load32(base, Load32(base, device + 10768));
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_8226C958(ctx, base);
  const auto now = std::chrono::steady_clock::now();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());

  std::lock_guard<std::mutex> lock(g_mutex);
  Site* site = nullptr;
  for (Site& s : g_sites) {
    if (s.caller == caller || s.calls == 0) {
      site = &s;
      break;
    }
  }
  if (site) {
    site->caller = caller;
    ++site->calls;
    site->waits += ns > 20000;
    site->ns += ns;
    site->max_ns = std::max(site->max_ns, ns);
    site->ahead += end - read;
    site->last_position = position;
    site->last_end = end;
    site->last_read = read;
    site->last_kind = kind;
  }
  if (now - g_report_start >= std::chrono::seconds(10)) {
    Report(now);
  }
}

REX_EXTERN(__imp__sub_8226D968);
REX_HOOK_RAW(sub_8226D968) {
  if (!REXCVAR_GET(cott_ring_wait_stats)) {
    __imp__sub_8226D968(ctx, base);
    return;
  }
  const uint32_t caller = uint32_t(ctx.lr);
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_8226D968(ctx, base);
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Site& spin : g_spins) {
    if (spin.caller == caller || spin.calls == 0) {
      spin.caller = caller;
      ++spin.calls;
      spin.ns += ns;
      break;
    }
  }
}
