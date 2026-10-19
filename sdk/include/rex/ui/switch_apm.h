/* nfsmw - console performance profile (apm). See switch_apm.cpp. */
#pragma once

#include "rex/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GPU MHz to request in handheld mode: 0 = touch nothing, 384 or 460. Called before
 * RexSwitchApmAplicar.
 */
void RexSwitchApmPedirGpuMhz(int mhz);

/*
 * Accept a configuration even if it raises the RAM to 1600. Off by default: if this firmware does
 * not have the "high GPU + unchanged RAM" pair, keeping the GPU as it was is preferred. Called
 * before RexSwitchApmAplicar.
 */
void RexSwitchApmPermitirRam1600(int permitir);

/*
 * Requests the profile. Does nothing if 0 was requested or if it was already called; docked, the
 * request waits for the first undocking (RexSwitchApmVigilar makes it then). If the configuration
 * does not exist in this firmware, apm returns an error and changes nothing.
 */
void RexSwitchApmAplicar(void);

/*
 * Test only: sets the GPU clock to mhz with clkrst, beyond the handheld configurations (768 = the
 * docked clock), and keeps it there from RexSwitchApmVigilar. 0 does nothing. Called after
 * RexSwitchApmAplicar.
 */
void RexSwitchApmForzarGpuMhz(int mhz);

/*
 * One tick per second from the profile thread. If the memory clock went up on its own after the
 * configuration was applied (the clock change takes time, so an immediate check read the old
 * value), it is set back with clkrst to what the console had originally. It does nothing, and does
 * not even open clkrst, if nothing was changed.
 */
void RexSwitchApmVigilar(void);

#ifdef __cplusplus
}
#endif
