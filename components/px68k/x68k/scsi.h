/*
 * PX68K source modified for the Tab5 port.
 * Intent: Tab5 HDS/SCSI bridge and direct-boot declarations.
 * Layer8 Aug/17/2026
 */
#ifndef _WINX68K_SCSI_H
#define _WINX68K_SCSI_H

#include <stdint.h>
#include "../libretro/common.h"

extern uint8_t SCSIIPL[0x2000];

void SCSI_Init(void);
void SCSI_Cleanup(void);

uint8_t FASTCALL SCSI_Read(uint32_t adr);
void FASTCALL SCSI_Write(uint32_t adr, uint8_t data);

/* Build 5.15 host-side HDS attachment. */
int SCSI_MountImage(int target, const char *path, int readonly);
void SCSI_UnmountImage(int target);
int SCSI_ImageReady(int target);
uint32_t SCSI_ImageBlocks(int target);

/* Build 5.15a deterministic guest/HDS probe helpers. */
int SCSI_ProbeFirstBlock(int target, uint32_t *hash_out);
int SCSI_ProbeLayout(int target, uint32_t *partition_count);
int SCSI_ArmDirectBoot(int target);
uint32_t SCSI_DebugIOCSCalls(void);
uint32_t SCSI_DebugReads(void);
uint32_t SCSI_DebugInstallerCalls(void);
uint32_t SCSI_DebugInitCalls(void);
uint32_t SCSI_DebugDriverInstalls(void);
uint32_t SCSI_DebugPartitionCount(void);
uint32_t SCSI_DebugIOCSVector(void);
void SCSI_InstallIOCSVector(void);

#endif /* _WINX68K_SCSI_H */
