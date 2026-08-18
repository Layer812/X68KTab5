/*
 * PX68K source modified for the Tab5 port.
 * Intent: Connect standalone Tab5 media control to the original FDD model while preserving guest read/write behavior.
 * Layer8 Aug/17/2026
 */
#ifdef ESP_PLATFORM
/* BUILD3_FDD_TRACE_INCLUDE */
#include "esp_log.h"
#endif
/*
 *  FDD.C - FDD Unit
 */

#include <string.h>

#include "common.h"
#include "../libretro/dosio.h"
#include "status.h"
#include "irqh.h"
#include "ioc.h"
#include "fdc.h"
#include "fdd.h"
#include "disk_d88.h"
#include "disk_xdf.h"
#include "disk_dim.h"

typedef struct {
	int SetDelay[4];
	int Types[4];
	int ROnly[4];
	int EMask[4];
	int Blink[4];
	int Access;
} FDDINFO;

static FDDINFO fdd;
static int (*SetFD[4])(int, char*)                             = { 0, XDF_SetFD,        D88_SetFD,        DIM_SetFD };
static int (*Eject[4])(int)                                    = { 0, XDF_Eject,        D88_Eject,        DIM_Eject };
static int (*Seek[4])(int, int, FDCID*)                        = { 0, XDF_Seek,         D88_Seek,         DIM_Seek };
static int (*ReadID[4])(int, FDCID*)                           = { 0, XDF_ReadID,       D88_ReadID,       DIM_ReadID };
static int (*WriteID[4])(int, int, uint8_t*, int)        = { 0, XDF_WriteID,      D88_WriteID,      DIM_WriteID };
static int (*Read[4])(int, FDCID*, uint8_t*)             = { 0, XDF_Read,         D88_Read,         DIM_Read };
static int (*ReadDiag[4])(int, FDCID*, FDCID*, uint8_t*) = { 0, XDF_ReadDiag,     D88_ReadDiag,     DIM_ReadDiag };
static int (*Write[4])(int, FDCID*, uint8_t*, int)       = { 0, XDF_Write,        D88_Write,        DIM_Write };
static int (*GetCurrentID[4])(int, FDCID*)                     = { 0, XDF_GetCurrentID, D88_GetCurrentID, DIM_GetCurrentID };

int FDD_IsReading                                              = 0;

int FDD_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAY32(fdd.SetDelay, 4),
		SFARRAY32(fdd.Types, 4),
		SFARRAY32(fdd.ROnly, 4),
		SFARRAY32(fdd.EMask, 4),
		SFARRAY32(fdd.Blink, 4),

		SFVAR(fdd.Access),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_FDD", false);

	return ret;
}

static void ConvertCapital(char* buf)
{
	for ( ; *buf; buf++)
   {
		if ( ((*buf>=0x80)&&(*buf<=0x9f))||(*buf>=0xe0) )
			buf++;
		else if ( (*buf>='a')&&(*buf<='z') )
         *buf -= 0x20;
	}
}

static int GetDiskType(char* file)
{
	char tmp[8], *p;
	int ret = FD_XDF;
	p = strrchr(file, '.');
	if ( p ) {
		memset(tmp, 0, 8);
		strncpy(tmp, p+1, 3);
		ConvertCapital(tmp);
		if ( (!strncmp(tmp, "D88", 3))||(!strncmp(tmp, "88D", 3)) )
			ret = FD_D88;
		else if ( !strncmp(tmp, "DIM", 3) )
			ret = FD_DIM;
	}
	return ret;
}

static uint32_t FASTCALL FDD_Int(uint8_t irq)
{
	IRQH_IRQCallBack(irq);
	if ( irq==1 )
		return ((uint32_t)IOC_IntVect+1);
	return -1;
}

void FDD_SetFD(int drive, char* filename, int readonly)
{
	int type = GetDiskType(filename);
	if ( (drive<0)||(drive>3) ) return;
	FDD_EjectFD(drive);
	if ( SetFD[type] ) {
		if ( SetFD[type](drive, filename) ) {
			fdd.Types[drive]  = type;
			fdd.ROnly[drive] |= readonly;
			fdd.SetDelay[drive] = 3;
			fdd.EMask[drive] = 0;
			fdd.Blink[drive] = 0;
			StatBar_SetFDD(drive, filename);
			StatBar_ParamFDD(drive, (fdd.Types[drive]!=FD_Non)?((fdd.Access==drive)?2:1):0, ((fdd.Types[drive]!=FD_Non)&&(!fdd.EMask[drive]))?1:0, (fdd.Blink[drive])?1:0);
		}
	}
}

void FDD_EjectFD(int drive)
{
	int type;
	if ( (drive<0)||(drive>3) ) return;
	type = fdd.Types[drive];
	if ( Eject[type] ) {
		Eject[type](drive);
		if ( IOC_IntStat&2 ) IRQH_Int(1, &FDD_Int);
	}
	fdd.Types[drive] = FD_Non;
	fdd.ROnly[drive] = 0;
	fdd.EMask[drive] = 0;
	fdd.Blink[drive] = 0;
	StatBar_SetFDD(drive, "");
	StatBar_ParamFDD(drive, (fdd.Types[drive]!=FD_Non)?((fdd.Access==drive)?2:1):0, ((fdd.Types[drive]!=FD_Non)&&(!fdd.EMask[drive]))?1:0, (fdd.Blink[drive])?1:0);
}


/*
 *   Eject Mask / Blink / AccessDrive
 */
void FDD_SetEMask(int drive, int emask)
{
	if ( (drive<0)||(drive>3) ) return;
	if ( fdd.EMask[drive]==emask ) return;
	fdd.EMask[drive] = emask;
	StatBar_ParamFDD(drive, (fdd.Types[drive]!=FD_Non)?((fdd.Access==drive)?2:1):0, ((fdd.Types[drive]!=FD_Non)&&(!fdd.EMask[drive]))?1:0, (fdd.Blink[drive])?1:0);
}

void FDD_SetAccess(int drive)
{
	if ( fdd.Access==drive ) return;
	fdd.Access = drive;
	StatBar_ParamFDD(0, (fdd.Types[0]!=FD_Non)?((fdd.Access==0)?2:1):0, ((fdd.Types[0]!=FD_Non)&&(!fdd.EMask[0]))?1:0, (fdd.Blink[0])?1:0);
	StatBar_ParamFDD(1, (fdd.Types[1]!=FD_Non)?((fdd.Access==1)?2:1):0, ((fdd.Types[1]!=FD_Non)&&(!fdd.EMask[1]))?1:0, (fdd.Blink[1])?1:0);
}

void FDD_SetBlink(int drive, int blink)
{
	if ( (drive<0)||(drive>3) ) return;
	if ( fdd.Blink[drive]==blink ) return;
	fdd.Blink[drive] = blink;
	StatBar_ParamFDD(drive, (fdd.Types[drive]!=FD_Non)?((fdd.Access==drive)?2:1):0, ((fdd.Types[drive]!=FD_Non)&&(!fdd.EMask[drive]))?1:0, (fdd.Blink[drive])?1:0);
}

void FDD_Init(void)
{
	memset(&fdd,0 , sizeof(FDDINFO));
	fdd.Access = -1;
	D88_Init();
	XDF_Init();
	DIM_Init();
}


void FDD_Cleanup(void)
{
	D88_Cleanup();
	XDF_Cleanup();
	DIM_Cleanup();
}


void FDD_Reset(void)
{
	int i;
	FDD_SetAccess(-1);
	for (i=0; i<4; i++) {
		FDD_SetEMask(i, 0);
		FDD_SetBlink(i, 0);
	}
}


void FDD_SetFDInt(void)
{
	int i;
	for (i=0; i<4; i++) {
		if ( fdd.SetDelay[i] ) {
#if defined(ESP_PLATFORM) && PX68K_TAB5_DIAG_VERBOSE
            /* Legacy insert-delay trace; disabled by default in Build 5.8. */
            int before_delay = fdd.SetDelay[i];
#endif

            fdd.SetDelay[i]--;

#if defined(ESP_PLATFORM) && PX68K_TAB5_DIAG_VERBOSE
            ESP_LOGI("PX68K_FDD",
                     "SETDELAY drv=%d %d->%d IOC_IntStat=%02X",
                     i,
                     before_delay,
                     fdd.SetDelay[i],
                     (unsigned)IOC_IntStat);

            if (fdd.SetDelay[i] <= 0)
            {
                ESP_LOGI("PX68K_FDD",
                         "INSERT EVENT drv=%d FDD_IEN=%d",
                         i,
                         (IOC_IntStat & 2) ? 1 : 0);
            }
#endif
			if ( fdd.SetDelay[i]<=0 ) {
				if ( IOC_IntStat&2 ) IRQH_Int(1, &FDD_Int);
				fdd.SetDelay[i] = 0;
			}
		}
	}
}


int FDD_Seek(int drv, int trk, FDCID* id)
{
#if defined(ESP_PLATFORM) && PX68K_TAB5_DIAG_VERBOSE
    /* Legacy seek trace; disabled by default in Build 5.8. */
    {
        static unsigned s_seek_trace = 0;

        if (s_seek_trace < 32)
        {
            ESP_LOGI("PX68K_FDD",
                     "SEEK #%u drv=%d trk=%d",
                     s_seek_trace,
                     drv,
                     trk);
        }

        s_seek_trace++;
    }
#endif
	int type;
	if ( (drv<0)||(drv>3) ) return 0;
	type = fdd.Types[drv];
	if ( Seek[type] )
		return Seek[type](drv, trk, id);
	return 0;
}

int FDD_ReadID(int drv, FDCID* id)
{
#if defined(ESP_PLATFORM) && PX68K_TAB5_DIAG_VERBOSE
    /* Legacy read-ID trace; disabled by default in Build 5.8. */
    {
        static unsigned s_readid_trace = 0;

        if (s_readid_trace < 32)
        {
            ESP_LOGI("PX68K_FDD",
                     "READID #%u drv=%d",
                     s_readid_trace,
                     drv);
        }

        s_readid_trace++;
    }
#endif
	int type;
	if ( (drv<0)||(drv>3) ) return 0;
	type = fdd.Types[drv];
	if ( ReadID[type] )
		return ReadID[type](drv, id);
	return 0;
}

int FDD_WriteID(int drv, int trk, uint8_t* buf, int num)
{
	int type;
	if ( (drv<0)||(drv>3) ) return 0;
	type = fdd.Types[drv];
	if ( WriteID[type] )
		return WriteID[type](drv, trk, buf, num);
	return 0;
}


int FDD_Read(int drv, FDCID* id, uint8_t* buf)
{
	int type;
	if ( (drv<0)||(drv>3) ) return 0;
	type = fdd.Types[drv];
	if ( Read[type] )
	{
		FDD_IsReading = 1;

#if defined(ESP_PLATFORM) && PX68K_TAB5_DIAG_VERBOSE
        /* Legacy sector-read trace; disabled by default in Build 5.8. */
        {
            static unsigned s_fdd_read_trace = 0;

            if (s_fdd_read_trace < 24)
            {
                ESP_LOGI("PX68K_FDD",
                         "READ #%u drv=%d type=%d C=%u H=%u R=%u N=%u",
                         s_fdd_read_trace,
                         drv,
                         type,
                         (unsigned)id->c,
                         (unsigned)id->h,
                         (unsigned)id->r,
                         (unsigned)id->n);
            }

            s_fdd_read_trace++;
        }
#endif

return Read[type](drv, id, buf);
	}
	return 0;
}


int FDD_ReadDiag(int drv, FDCID* id, FDCID* retid, uint8_t* buf)
{
	int type;
	if ( (drv<0)||(drv>3) ) return 0;
	type = fdd.Types[drv];
	if ( ReadDiag[type] )
		return ReadDiag[type](drv, id, retid, buf);
	return 0;
}


int FDD_Write(int drv, FDCID* id, uint8_t* buf, int del)
{
    int type;
    int ret = 0;
    if ( (drv<0)||(drv>3) ) return 0;
    type = fdd.Types[drv];
    if ( Write[type] )
        ret = Write[type](drv, id, buf, del);
#ifdef ESP_PLATFORM
    {
        static unsigned s_write_trace = 0;
#if PX68K_TAB5_DIAG_VERBOSE
        if (s_write_trace < 64 || !ret)
        {
            ESP_LOGI("PX68K_FDD",
                     "WRITE #%u drv=%d type=%d ro=%d C=%u H=%u R=%u N=%u del=%d ret=%d",
                     s_write_trace, drv, type, FDD_IsReadOnly(drv),
                     id ? (unsigned)id->c : 0u,
                     id ? (unsigned)id->h : 0u,
                     id ? (unsigned)id->r : 0u,
                     id ? (unsigned)id->n : 0u, del, ret);
        }
#else
        if (!ret)
            ESP_LOGW("PX68K_FDD", "WRITE FAILED drv=%d type=%d ro=%d",
                     drv, type, FDD_IsReadOnly(drv));
#endif
        s_write_trace++;
    }
#endif
    return ret;
}


int FDD_GetCurrentID(int drv, FDCID* id)
{
	int type;
	if ( (drv<0)||(drv>3) ) return 0;
	type = fdd.Types[drv];
	if ( GetCurrentID[type] )
		return GetCurrentID[type](drv, id);
	return 0;
}


int FDD_IsReady(int drv)
{
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (fdd.Types[drv]!=FD_Non)&&(!fdd.SetDelay[drv]) )
		return 1;
	return 0;
}


int FDD_IsReadOnly(int drv)
{
	if ( (drv<0)||(drv>3) ) return 0;
	return fdd.ROnly[drv];
}


void FDD_SetReadOnly(int drv)
{
	fdd.ROnly[drv] |= 1;
}
