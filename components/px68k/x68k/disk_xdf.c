#include "common.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_partition.h"
#endif
#include "../libretro/dosio.h"
#include "fdc.h"
#include "fdd.h"
#include "disk_xdf.h"

static char           XDFFile[4][MAX_PATH];
static int            XDFCur[4] = {0, 0, 0, 0};
static int            XDFTrk[4] = {0, 0, 0, 0};
static uint8_t       *XDFImg[4] = {0, 0, 0, 0};
static uint8_t        XDFEmbedded[4] = {0, 0, 0, 0};
#ifdef ESP_PLATFORM
static esp_partition_mmap_handle_t XDFMapHandle[4] = {0, 0, 0, 0};
#endif

#define XDF_FLASH_HUMAN_PATH ":FLASH:HUMAN302.XDF"
#define XDF_FLASH_HUMAN_LABEL "humanxdf"
#define XDF_FLASH_HUMAN_SIZE 1261568u

void XDF_Init(void)
{
	int drv;

	for (drv=0; drv<4; drv++) {
		XDFCur[drv] = 0;
		XDFImg[drv] = 0;
		XDFEmbedded[drv] = 0;
#ifdef ESP_PLATFORM
		XDFMapHandle[drv] = 0;
#endif
		memset(XDFFile[drv], 0, MAX_PATH);
	}
}


void XDF_Cleanup(void)
{
	int drv;
	for (drv=0; drv<4; drv++) XDF_Eject(drv);
}


int XDF_SetFD(int drv, char* filename)
{
	void *fp;

	strncpy(XDFFile[drv], filename, MAX_PATH);
	XDFFile[drv][MAX_PATH-1] = 0;
	XDFEmbedded[drv] = 0;

#ifdef ESP_PLATFORM
	if (!strcmp(filename, XDF_FLASH_HUMAN_PATH)) {
		const esp_partition_t *part = esp_partition_find_first(
			ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, XDF_FLASH_HUMAN_LABEL);
		const void *mapped = NULL;
		esp_err_t err;

		if (!part || part->size < XDF_FLASH_HUMAN_SIZE) {
			ESP_LOGE("PX68K_XDF", "Flash Human partition missing/small label=%s need=%u",
			         XDF_FLASH_HUMAN_LABEL, (unsigned)XDF_FLASH_HUMAN_SIZE);
			memset(XDFFile[drv], 0, MAX_PATH);
			return 0;
		}
		err = esp_partition_mmap(part, 0, XDF_FLASH_HUMAN_SIZE,
		                         ESP_PARTITION_MMAP_DATA, &mapped, &XDFMapHandle[drv]);
		if (err != ESP_OK || !mapped) {
			ESP_LOGE("PX68K_XDF", "Flash Human mmap failed err=%d", (int)err);
			XDFMapHandle[drv] = 0;
			memset(XDFFile[drv], 0, MAX_PATH);
			return 0;
		}

		/* Standard HUMAN302.XDF starts 60 3C 90 58 36 38 49 50.  This also
		 * catches a partition that exists but was not flashed by upload. */
		{
			static const uint8_t expected[8] = {0x60,0x3c,0x90,0x58,0x36,0x38,0x49,0x50};
			if (memcmp(mapped, expected, sizeof(expected)) != 0) {
				const uint8_t *b = (const uint8_t *)mapped;
				ESP_LOGE("PX68K_XDF", "Flash Human header invalid: %02X %02X %02X %02X %02X %02X %02X %02X",
				         b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
				esp_partition_munmap(XDFMapHandle[drv]);
				XDFMapHandle[drv] = 0;
				memset(XDFFile[drv], 0, MAX_PATH);
				return 0;
			}
		}

		XDFImg[drv] = (uint8_t *)(uintptr_t)mapped;
		XDFEmbedded[drv] = 1;
		ESP_LOGI("PX68K_XDF", "Flash Human partition attached drv=%d label=%s off=0x%06lX bytes=%u ptr=%p",
		         drv, XDF_FLASH_HUMAN_LABEL, (unsigned long)part->address,
		         (unsigned)XDF_FLASH_HUMAN_SIZE, mapped);
		return 1;
	}
#endif

	XDFImg[drv] = (uint8_t*)malloc(1261568);
	if ( !XDFImg[drv] )
      return 0;
	memset(XDFImg[drv], 0xe5, 1261568);
	fp = file_open(XDFFile[drv]);
	if ( !fp )
   {
		free(XDFImg[drv]);
		XDFImg[drv] = 0;
		memset(XDFFile[drv], 0, MAX_PATH);
		FDD_SetReadOnly(drv);
		return 0;
	}
	file_seek(fp, 0, FSEEK_SET);
	file_lread(fp, XDFImg[drv], 1261568);
	file_close(fp);
	return 1;
}


int XDF_Eject(int drv)
{
	void *fp;

	if ( !XDFImg[drv] ) {
		memset(XDFFile[drv], 0, MAX_PATH);
		XDFEmbedded[drv] = 0;
		return 0;
	}
	if ( XDFEmbedded[drv] ) {
#ifdef ESP_PLATFORM
        ESP_LOGI("PX68K_XDF", "Flash Human partition eject drv=%d (munmap, no flush)", drv);
        if (XDFMapHandle[drv]) {
            esp_partition_munmap(XDFMapHandle[drv]);
            XDFMapHandle[drv] = 0;
        }
#endif
        XDFImg[drv] = 0;
        XDFEmbedded[drv] = 0;
        memset(XDFFile[drv], 0, MAX_PATH);
        return 1;
    }
	if ( !FDD_IsReadOnly(drv) ) {
#ifdef ESP_PLATFORM
        ESP_LOGI("PX68K_XDF", "FLUSH BEGIN drv=%d path=%s bytes=%u", drv, XDFFile[drv], 1261568u);
#endif
		if (!(fp = file_open(XDFFile[drv]))) goto xdf_eject_error;
		file_seek(fp, 0, FSEEK_SET);
		if ( file_lwrite(fp, XDFImg[drv], 1261568)!=1261568 ) { file_close(fp); goto xdf_eject_error; }
		file_close(fp);
#ifdef ESP_PLATFORM
        ESP_LOGI("PX68K_XDF", "FLUSH OK drv=%d path=%s", drv, XDFFile[drv]);
#endif
	}
#ifdef ESP_PLATFORM
    else {
        ESP_LOGI("PX68K_XDF", "FLUSH SKIP drv=%d read_only=1 path=%s", drv, XDFFile[drv]);
    }
#endif
	free(XDFImg[drv]);
	XDFImg[drv] = 0;
	XDFEmbedded[drv] = 0;
	memset(XDFFile[drv], 0, MAX_PATH);
	return 1;

xdf_eject_error:
#ifdef ESP_PLATFORM
    ESP_LOGE("PX68K_XDF", "FLUSH FAILED drv=%d path=%s", drv, XDFFile[drv]);
#endif
	free(XDFImg[drv]);
	XDFImg[drv] = 0;
	XDFEmbedded[drv] = 0;
	memset(XDFFile[drv], 0, MAX_PATH);
	return 0;
}


int XDF_Seek(int drv, int trk, FDCID* id)
{
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (trk<0)||(trk>153) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	if ( XDFTrk[drv]!=trk ) XDFCur[drv] = 0;
	id->c = trk>>1;
	id->h = trk&1;
	id->r = XDFCur[drv]+1;
	id->n = 3;
	XDFTrk[drv] = trk;
	return 1;
}


int XDF_GetCurrentID(int drv, FDCID* id)
{
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (XDFTrk[drv]<0)||(XDFTrk[drv]>153) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	id->c = XDFTrk[drv]>>1;
	id->h = XDFTrk[drv]&1;
	id->r = XDFCur[drv]+1;
	id->n = 3;
	return 1;
}


int XDF_ReadID(int drv, FDCID* id)
{
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (XDFTrk[drv]<0)||(XDFTrk[drv]>153) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	id->c = XDFTrk[drv]>>1;
	id->h = XDFTrk[drv]&1;
	id->r = XDFCur[drv]+1;
	id->n = 3;
	XDFCur[drv] = (XDFCur[drv]+1)&7;
	return 1;
}


int XDF_WriteID(int drv, int trk, uint8_t *buf, int num)
{
	int i;
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (trk<0)||(trk>153) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	if ( num!=8 ) return 0;
	for (i=0; i<8; i++, buf+=4) {
		if ( (((buf[0]<<1)+buf[1])!=trk)||(buf[2]<1)||(buf[2]>8)||(buf[3]!=3) ) return 0;
	}
	XDFTrk[drv] = trk;
	return 1;
}


int XDF_Read(int drv, FDCID* id, uint8_t *buf)
{
	int pos;
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (XDFTrk[drv]<0)||(XDFTrk[drv]>153) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	if ( (((id->c<<1)+id->h)!=XDFTrk[drv]) ) return 0;
	if ( (id->r<1)||(id->r>8) ) return 0;
	if ( (id->h!=0)&&(id->h!=1) ) return 0;
	if ( id->n!=3 ) return 0;
	pos = ((((id->c<<1)+(id->h))*8)+(id->r-1))<<10;
	memcpy(buf, XDFImg[drv]+pos, 1024);
	XDFCur[drv] = (id->r)&7;
	return 1;
}


int XDF_ReadDiag(int drv, FDCID* id, FDCID* retid, uint8_t *buf)
{
	int pos;
	(void)id;
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (XDFTrk[drv]<0)||(XDFTrk[drv]>153) ) return 0;
	if ( (XDFCur[drv]<0)||(XDFCur[drv]>8) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	pos = ((XDFTrk[drv]*8)+XDFCur[drv])<<10;
	memcpy(buf, XDFImg[drv]+pos, 1024);
	retid->c = XDFTrk[drv]>>1;
	retid->h = XDFTrk[drv]&1;
	retid->r = XDFCur[drv]+1;
	retid->n = 3;
	XDFCur[drv] = (XDFCur[drv]+1)&7;
	return 1;
}


int XDF_Write(int drv, FDCID* id, uint8_t *buf, int del)
{
	int pos;
	(void)del;
	if ( (drv>=0)&&(drv<4)&&XDFEmbedded[drv] ) return 0;
	if ( (drv<0)||(drv>3) ) return 0;
	if ( (XDFTrk[drv]<0)||(XDFTrk[drv]>153) ) return 0;
	if ( !XDFImg[drv] ) return 0;
	if ( (((id->c<<1)+id->h)!=XDFTrk[drv]) ) return 0;
	if ( (id->r<1)||(id->r>8) ) return 0;
	if ( (id->h!=0)&&(id->h!=1) ) return 0;
	if ( id->n!=3 ) return 0;
	pos = ((((id->c<<1)+(id->h))*8)+(id->r-1))<<10;
	memcpy(XDFImg[drv]+pos, buf, 1024);
	XDFCur[drv] = (id->r)&7;
	return 1;
}
