/*
 * Tab5 port-specific implementation.
 * Intent: Human68k HostFS bridge declarations for the Tab5 SD-card drive.
 * Layer8 Aug/17/2026
 */
#ifndef PX68K_TAB5_HOSTFS_H
#define PX68K_TAB5_HOSTFS_H

#include <stdint.h>

/* Build 5.96a: Human68k remote-disk bridge for the already-mounted Tab5 SD. */
void HostFS_Reset(void);
int HostFS_Available(void);
uint32_t HostFS_InstallDriver(uint32_t guest_addr);
void HostFS_StrategyTrap(void);
void HostFS_InterruptTrap(void);
uint32_t HostFS_DebugCalls(void);
int HostFS_DebugDrive(void);
/* X68KTAB_R1A15_HOSTFS_FASTPATH diagnostics, observation only. */
void HostFS_R1A15GetStats(uint32_t out[9]);

#endif
