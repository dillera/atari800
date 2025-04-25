/*
 * sio.c - Serial I/O emulation
 *
 * Copyright (C) 1995-1998 David Firth
 * Copyright (C) 1998-2010 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#define _POSIX_C_SOURCE 200112L /* for snprintf */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "afile.h"
#include "antic.h"  /* ANTIC_ypos */
#include "atari.h"
#include "binload.h"
#include "cassette.h"
#include "compfile.h"
#include "cpu.h"
#include "esc.h"
#include "log.h"
#include "memory.h"
#include "platform.h"
#include "pokey.h"
#include "pokeysnd.h"
#include "sio.h"
#include "util.h"
#include "fujinet_netsio.h" /* Needed for FujiNet_NetSIO_ForwardSIOCommand */
#include "fujinet.h" /* ADDED for FujiNet support */
#ifndef BASIC
#include "statesav.h"
#endif

#ifdef USE_FUJINET
#include "fujinet_netsio.h"
#endif

#undef DEBUG_PRO
#undef DEBUG_VAPI

/* If ATR image is in double density (256 bytes per sector),
   then the boot sectors (sectors 1-3) can be:
   - logical (as seen by Atari) - 128 bytes in each sector
   - physical (as stored on the disk) - 256 bytes in each sector.
     Only the first half of sector is used for storing data, the rest is zero.
   - SIO2PC (the type used by the SIO2PC program) - 3 * 128 bytes for data
     of boot sectors, then 3 * 128 unused bytes (zero)
   The XFD images in double density have either logical or physical
   boot sectors. */
#define BOOT_SECTORS_LOGICAL	0
#define BOOT_SECTORS_PHYSICAL	1
#define BOOT_SECTORS_SIO2PC		2
static int boot_sectors_type[SIO_MAX_DRIVES];

static int image_type[SIO_MAX_DRIVES];
#define IMAGE_TYPE_XFD  0
#define IMAGE_TYPE_ATR  1
#define IMAGE_TYPE_PRO  2
#define IMAGE_TYPE_VAPI 3
static FILE *disk[SIO_MAX_DRIVES] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static int sectorcount[SIO_MAX_DRIVES];
static int sectorsize[SIO_MAX_DRIVES];
/* these two are used by the 1450XLD parallel disk device */
int SIO_format_sectorcount[SIO_MAX_DRIVES];
int SIO_format_sectorsize[SIO_MAX_DRIVES];
static int io_success[SIO_MAX_DRIVES];
/* stores dup sector counter for PRO images */
typedef struct tagpro_additional_info_t {
	int max_sector;
	unsigned char *count;
} pro_additional_info_t;

#define MAX_VAPI_PHANTOM_SEC  		40
#define VAPI_BYTES_PER_TRACK        26042.0 
#define VAPI_CYCLES_PER_ROT 		372706
#define VAPI_CYCLES_PER_TRACK_STEP 	35780 /*70937*/
#define VAPI_CYCLES_HEAD_SETTLE 	70134
#define VAPI_CYCLES_TRACK_READ_DELTA 1426
#define VAPI_CYCLES_CMD_ACK_TRANS 	3188
#define VAPI_CYCLES_SECTOR_READ 	29014
#define VAPI_CYCLES_MISSING_SECTOR	(2*VAPI_CYCLES_PER_ROT + 14453)
#define VAPI_CYCLES_BAD_SECTOR_NUM	1521

/* stores dup sector information for VAPI images */
typedef struct tagvapi_sec_info_t {
	int sec_count;
	unsigned int sec_offset[MAX_VAPI_PHANTOM_SEC];
	unsigned char sec_status[MAX_VAPI_PHANTOM_SEC];
	unsigned int sec_rot_pos[MAX_VAPI_PHANTOM_SEC];
} vapi_sec_info_t;

typedef struct tagvapi_additional_info_t {
	vapi_sec_info_t *sectors;
	int sec_stat_buff[4];
	int vapi_delay_time;
} vapi_additional_info_t;

/* VAPI Format Header */
typedef struct tagvapi_file_header_t {
	unsigned char signature[4];
	unsigned char majorver;
	unsigned char minorver;
	unsigned char reserved1[22];
	unsigned char startdata[4];
	unsigned char reserved[16];
} vapi_file_header_t;

typedef struct tagvapi_track_header_t {
	unsigned char  next[4];
	unsigned char  type[2];
	unsigned char  reserved1[2];
	unsigned char  tracknum;
	unsigned char  reserved2;
	unsigned char  sectorcnt[2];
	unsigned char  reserved3[8];
	unsigned char  startdata[4];
	unsigned char  reserved4[8];
} vapi_track_header_t;

typedef struct tagvapi_sector_list_header_t {
	unsigned char  sizelist[4];
	unsigned char  type;
	unsigned char  reserved[3];
} vapi_sector_list_header_t;

typedef struct tagvapi_sector_header_t {
	unsigned char  sectornum;
	unsigned char  sectorstatus;
	unsigned char  sectorpos[2];
	unsigned char  startdata[4];
} vapi_sector_header_t;

#define VAPI_32(x) (x[0] + (x[1] << 8) + (x[2] << 16) + (x[3] << 24))
#define VAPI_16(x) (x[0] + (x[1] << 8))

/* Additional Info for all copy protected disk types */
static void *additional_info[SIO_MAX_DRIVES];

SIO_UnitStatus SIO_drive_status[SIO_MAX_DRIVES];
char SIO_filename[SIO_MAX_DRIVES][FILENAME_MAX];

Util_tmpbufdef(static, sio_tmpbuf[SIO_MAX_DRIVES])

int SIO_last_op;
int SIO_last_op_time = 0;
int SIO_last_drive;
int SIO_last_sector;
char SIO_status[256];

/* Serial I/O emulation support */
#define SIO_NoFrame         (0x00)
#define SIO_CommandFrame    (0x01)
#define SIO_StatusRead      (0x02)
#define SIO_ReadFrame       (0x03)
#define SIO_WriteFrame      (0x04)
#define SIO_FinalStatus     (0x05)
#define SIO_FormatFrame     (0x06)
static UBYTE CommandFrame[6];
static int CommandIndex = 0;
static UBYTE DataBuffer[256 + 3];
static int DataIndex = 0;
int TransferStatus = SIO_NoFrame;
SIO_State_t SIO;
static int ExpectedBytes = 0;

int ignore_header_writeprotect = FALSE;

/* Forward declarations for static SIO handler functions */
static UBYTE handle_status_request(int drive);
static UBYTE handle_format(int drive, UBYTE command);
static UBYTE handle_read_sector(int drive, int sector);
static UBYTE handle_write_sector(int drive, int sector, int verify);
static UBYTE SIO_ProcessCommandFrame(void);

#ifdef USE_FUJINET
static BOOL fujinet_poll_initialized = FALSE; /* Flag to ensure we only initialize polling once */
#endif

int SIO_Initialise(int *argc, char *argv[])
{
	int i;
	for (i = 0; i < SIO_MAX_DRIVES; i++) {
		strcpy(SIO_filename[i], "Off");
		SIO_drive_status[i] = SIO_OFF;
		SIO_format_sectorsize[i] = 128;
		SIO_format_sectorcount[i] = 720;
	}
	TransferStatus = SIO_NoFrame;

#ifdef USE_FUJINET
	/* Initialize FujiNet system */
	if (!FujiNet_Initialise()) {
		Log_print("Error initializing FujiNet support");
	}
	else {
		Log_print("FujiNet initialized successfully");
	}
#endif

	return TRUE;
}

/* umount disks so temporary files are deleted */
void SIO_Exit(void)
{
	int i;
	for (i = 1; i <= SIO_MAX_DRIVES; i++)
		SIO_Dismount(i);

#ifdef USE_FUJINET
	/* Shutdown FujiNet */
	FujiNet_Shutdown();
#endif
}

int SIO_Mount(int diskno, const char *filename, int b_open_readonly)
{
	FILE *f = NULL;
	SIO_UnitStatus status = SIO_READ_WRITE;
	struct AFILE_ATR_Header header;

	/* avoid overruns in SIO_filename[] */
	if (strlen(filename) >= FILENAME_MAX)
		return FALSE;

	/* release previous disk */
	SIO_Dismount(diskno);

	/* open file */
	if (!b_open_readonly)
		f = Util_fopen(filename, "rb+", sio_tmpbuf[diskno - 1]);
	if (f == NULL) {
		f = Util_fopen(filename, "rb", sio_tmpbuf[diskno - 1]);
		if (f == NULL)
			return FALSE;
		status = SIO_READ_ONLY;
	}

	/* read header */
	if (fread(&header, 1, sizeof(struct AFILE_ATR_Header), f) != sizeof(struct AFILE_ATR_Header)) {
		fclose(f);
		return FALSE;
	}

	/* detect compressed image and uncompress */
	switch (header.magic1) {
	case 0xf9:
	case 0xfa:
		/* DCM */
		{
			FILE *f2 = Util_tmpopen(sio_tmpbuf[diskno - 1]);
			if (f2 == NULL)
				return FALSE;
			Util_rewind(f);
			if (!CompFile_DCMtoATR(f, f2)) {
				Util_fclose(f2, sio_tmpbuf[diskno - 1]);
				fclose(f);
				return FALSE;
			}
			fclose(f);
			f = f2;
		}
		Util_rewind(f);
		if (fread(&header, 1, sizeof(struct AFILE_ATR_Header), f) != sizeof(struct AFILE_ATR_Header)) {
			Util_fclose(f, sio_tmpbuf[diskno - 1]);
			return FALSE;
		}
		status = SIO_READ_ONLY;
		/* XXX: status = b_open_readonly ? SIO_READ_ONLY : SIO_READ_WRITE; */
		break;
	case 0x1f:
		if (header.magic2 == 0x8b) {
			/* ATZ/ATR.GZ, XFZ/XFD.GZ */
			fclose(f);
			f = Util_tmpopen(sio_tmpbuf[diskno - 1]);
			if (f == NULL)
				return FALSE;
			if (!CompFile_ExtractGZ(filename, f)) {
				Util_fclose(f, sio_tmpbuf[diskno - 1]);
				return FALSE;
			}
			Util_rewind(f);
			if (fread(&header, 1, sizeof(struct AFILE_ATR_Header), f) != sizeof(struct AFILE_ATR_Header)) {
				Util_fclose(f, sio_tmpbuf[diskno - 1]);
				return FALSE;
			}
			status = SIO_READ_ONLY;
			/* XXX: status = b_open_readonly ? SIO_READ_ONLY : SIO_READ_WRITE; */
		}
		break;
	default:
		break;
	}

	boot_sectors_type[diskno - 1] = BOOT_SECTORS_LOGICAL;

	if (header.magic1 == AFILE_ATR_MAGIC1 && header.magic2 == AFILE_ATR_MAGIC2) {
		/* ATR (may be temporary from DCM or ATR/ATR.GZ) */
		image_type[diskno - 1] = IMAGE_TYPE_ATR;

		sectorsize[diskno - 1] = (header.secsizehi << 8) + header.secsizelo;
		if (sectorsize[diskno - 1] != 128 && sectorsize[diskno - 1] != 256) {
			Util_fclose(f, sio_tmpbuf[diskno - 1]);
			return FALSE;
		}

		if (header.writeprotect != 0 && !ignore_header_writeprotect)
			status = SIO_READ_ONLY;

		/* ATR header contains length in 16-byte chunks. */
		/* First compute number of 128-byte chunks
		   - it's number of sectors on single density disk */
		sectorcount[diskno - 1] = ((header.hiseccounthi << 24)
			+ (header.hiseccountlo << 16)
			+ (header.seccounthi << 8)
			+ header.seccountlo) >> 3;

		/* Fix number of sectors if double density */
		if (sectorsize[diskno - 1] == 256) {
			if ((sectorcount[diskno - 1] & 1) != 0)
				/* logical (128-byte) boot sectors */
				sectorcount[diskno - 1] += 3;
			else {
				/* 256-byte boot sectors */
				/* check if physical or SIO2PC: physical if there's
				   a non-zero byte in bytes 0x190-0x30f of the ATR file */
				UBYTE buffer[0x180];
				int i;
				fseek(f, 0x190, SEEK_SET);
				if (fread(buffer, 1, 0x180, f) != 0x180) {
					Util_fclose(f, sio_tmpbuf[diskno - 1]);
					return FALSE;
				}
				boot_sectors_type[diskno - 1] = BOOT_SECTORS_SIO2PC;
				for (i = 0; i < 0x180; i++)
					if (buffer[i] != 0) {
						boot_sectors_type[diskno - 1] = BOOT_SECTORS_PHYSICAL;
						break;
					}
			}
			sectorcount[diskno - 1] >>= 1;
		}
	}
	else if (header.magic1 == 'A' && header.magic2 == 'T' && header.seccountlo == '8' &&
		 header.seccounthi == 'X') {
		int file_length = Util_flen(f);
		vapi_additional_info_t *info;
		vapi_file_header_t fileheader;
		vapi_track_header_t trackheader;
		int trackoffset, totalsectors;

		/* .atx is read only for now */
#ifndef VAPI_WRITE_ENABLE
		if (!b_open_readonly) {
			fclose(f);
			f = Util_fopen(filename, "rb", sio_tmpbuf[diskno - 1]);
			if (f == NULL)
				return FALSE;
			status = SIO_READ_ONLY;
		}
#endif
		
		image_type[diskno - 1] = IMAGE_TYPE_VAPI;
		sectorsize[diskno - 1] = 128;
		sectorcount[diskno - 1] = 720;
		fseek(f,0,SEEK_SET);
		if (fread(&fileheader,1,sizeof(fileheader),f) != sizeof(fileheader)) {
			Util_fclose(f, sio_tmpbuf[diskno - 1]);
			Log_print("VAPI: Bad File Header");
			return(FALSE);
			}
		trackoffset = VAPI_32(fileheader.startdata);	
		if (trackoffset > file_length) {
			Util_fclose(f, sio_tmpbuf[diskno - 1]);
			Log_print("VAPI: Bad Track Offset");
			return(FALSE);
			}
#ifdef DEBUG_VAPI
		Log_print("VAPI File Version %d.%d",fileheader.majorver,fileheader.minorver);
#endif
		/* Read all of the track headers to get the total sector count */
		totalsectors = 0;
		while (trackoffset > 0 && trackoffset < file_length) {
			ULONG next;
			UWORD tracktype;

			fseek(f,trackoffset,SEEK_SET);
			if (fread(&trackheader,1,sizeof(trackheader),f) != sizeof(trackheader)) {
				Util_fclose(f, sio_tmpbuf[diskno - 1]);
				Log_print("VAPI: Bad Track Header");
				return(FALSE);
				}
			next = VAPI_32(trackheader.next);
			tracktype = VAPI_16(trackheader.type);
			if (tracktype == 0) {
				totalsectors += VAPI_16(trackheader.sectorcnt);
				}
			trackoffset += next;
		}

		info = (vapi_additional_info_t *)Util_malloc(sizeof(vapi_additional_info_t));
		additional_info[diskno-1] = info;
		info->sectors = (vapi_sec_info_t *)Util_malloc(sectorcount[diskno - 1] * 
 					    sizeof(vapi_sec_info_t));
		memset(info->sectors, 0, sectorcount[diskno - 1] * 
 					 sizeof(vapi_sec_info_t));

		/* Now read all the sector data */
		trackoffset = VAPI_32(fileheader.startdata);
		while (trackoffset > 0 && trackoffset < file_length) {
			int sectorcnt, seclistdata,next;
			vapi_sector_list_header_t sectorlist;
			vapi_sector_header_t sectorheader;
			vapi_sec_info_t *sector;
			UWORD tracktype;
			int j;

			fseek(f,trackoffset,SEEK_SET);
			if (fread(&trackheader,1,sizeof(trackheader),f) != sizeof(trackheader)) {
				free(info->sectors);
				free(info);
				Util_fclose(f, sio_tmpbuf[diskno - 1]);
				Log_print("VAPI: Bad Track Header while reading sectors");
				return(FALSE);
				}
			next = VAPI_32(trackheader.next);
			sectorcnt = VAPI_16(trackheader.sectorcnt);
			tracktype = VAPI_16(trackheader.type);
			seclistdata = VAPI_32(trackheader.startdata) + trackoffset;
#ifdef DEBUG_VAPI
			Log_print("Track %d: next %x type %d seccnt %d secdata %x",trackheader.tracknum,
				trackoffset + next,VAPI_16(trackheader.type),sectorcnt,seclistdata);
#endif
			if (tracktype == 0) {
				if (seclistdata > file_length) {
					free(info->sectors);
					free(info);
					Util_fclose(f, sio_tmpbuf[diskno - 1]);
					Log_print("VAPI: Bad Sector List Offset");
					return(FALSE);
					}
				fseek(f,seclistdata,SEEK_SET);
				if (fread(&sectorlist,1,sizeof(sectorlist),f) != sizeof(sectorlist)) {
					free(info->sectors);
					free(info);
					Util_fclose(f, sio_tmpbuf[diskno - 1]);
					Log_print("VAPI: Bad Sector List");
					return(FALSE);
					}
#ifdef DEBUG_VAPI
				Log_print("Size sec list %x type %d",VAPI_32(sectorlist.sizelist),sectorlist.type);
#endif
				for (j=0;j<sectorcnt;j++) {
					double percent_rot;

					if (fread(&sectorheader,1,sizeof(sectorheader),f) != sizeof(sectorheader)) {
						free(info->sectors);
						free(info);
						Util_fclose(f, sio_tmpbuf[diskno - 1]);
						Log_print("VAPI: Bad Sector Header");
						return(FALSE);
						}
					if (sectorheader.sectornum > 18)  {
						Util_fclose(f, sio_tmpbuf[diskno - 1]);
						Log_print("VAPI: Bad Sector Index: Track %d Sec Num %d Index %d",
								trackheader.tracknum,j,sectorheader.sectornum);
						return(FALSE);
						}
					sector = &info->sectors[trackheader.tracknum * 18 + sectorheader.sectornum - 1];

					percent_rot = ((double) VAPI_16(sectorheader.sectorpos))/VAPI_BYTES_PER_TRACK;
					sector->sec_rot_pos[sector->sec_count] = (unsigned int) (percent_rot * VAPI_CYCLES_PER_ROT);
					sector->sec_offset[sector->sec_count] = VAPI_32(sectorheader.startdata) + trackoffset;
					sector->sec_status[sector->sec_count] = ~sectorheader.sectorstatus;
					sector->sec_count++;
					if (sector->sec_count > MAX_VAPI_PHANTOM_SEC) {
						free(info->sectors);
						free(info);
						Util_fclose(f, sio_tmpbuf[diskno - 1]);
						Log_print("VAPI: Too many Phantom Sectors");
						return(FALSE);
						}
#ifdef DEBUG_VAPI
					Log_print("Sector %d status %x position %f %d %d data %x",sectorheader.sectornum,
						sector->sec_status[sector->sec_count-1],percent_rot,
						sector->sec_rot_pos[sector->sec_count-1],
						VAPI_16(sectorheader.sectorpos),
						sector->sec_offset[sector->sec_count-1]);				
#endif				
				}
#ifdef DEBUG_VAPI
				Log_flushlog();
#endif
			} else {
				Log_print("Unknown VAPI track type Track:%d Type:%d",trackheader.tracknum,tracktype);
			}
			trackoffset += next;
		}			
	}
	else {
		int file_length = Util_flen(f);
		/* check for PRO */
		if ((file_length-16)%(128+12) == 0 &&
				(header.magic1*256 + header.magic2 == (file_length-16)/(128+12)) &&
				header.seccountlo == 'P') {
			pro_additional_info_t *info;
			/* .pro is read only for now */
			if (!b_open_readonly) {
				fclose(f);
				f = Util_fopen(filename, "rb", sio_tmpbuf[diskno - 1]);
				if (f == NULL)
					return FALSE;
				status = SIO_READ_ONLY;
			}
			image_type[diskno - 1] = IMAGE_TYPE_PRO;
			sectorsize[diskno - 1] = 128;
			if (file_length >= 1040*(128+12)+16) {
				/* assume enhanced density */
				sectorcount[diskno - 1] = 1040;
			}
			else {
				/* assume single density */
				sectorcount[diskno - 1] = 720;
			}

			info = (pro_additional_info_t *)Util_malloc(sizeof(pro_additional_info_t));
			additional_info[diskno-1] = info;
			info->count = (unsigned char *)Util_malloc(sectorcount[diskno - 1]);
			memset(info->count, 0, sectorcount[diskno -1]);
			info->max_sector = (file_length-16)/(128+12);
		}
		else {
			/* XFD (may be temporary from XFZ/XFD.GZ) */

			image_type[diskno - 1] = IMAGE_TYPE_XFD;

			if (file_length <= (1040 * 128)) {
				/* single density */
				sectorsize[diskno - 1] = 128;
				sectorcount[diskno - 1] = file_length >> 7;
			}
			else {
				/* double density */
				sectorsize[diskno - 1] = 256;
				if ((file_length & 0xff) == 0) {
					boot_sectors_type[diskno - 1] = BOOT_SECTORS_PHYSICAL;
					sectorcount[diskno - 1] = file_length >> 8;
				}
				else
					sectorcount[diskno - 1] = (file_length + 0x180) >> 8;
			}
		}
	}

#ifdef DEBUG
	Log_print("sectorcount = %d, sectorsize = %d",
		   sectorcount[diskno - 1], sectorsize[diskno - 1]);
#endif
	SIO_format_sectorsize[diskno - 1] = sectorsize[diskno - 1];
	SIO_format_sectorcount[diskno - 1] = sectorcount[diskno - 1];
	strcpy(SIO_filename[diskno - 1], filename);
	SIO_drive_status[diskno - 1] = status;
	disk[diskno - 1] = f;
	return TRUE;
}

void SIO_Dismount(int diskno)
{
	if (disk[diskno - 1] != NULL) {
		Util_fclose(disk[diskno - 1], sio_tmpbuf[diskno - 1]);
		disk[diskno - 1] = NULL;
		SIO_drive_status[diskno - 1] = SIO_NO_DISK;
		strcpy(SIO_filename[diskno - 1], "Empty");
		if (image_type[diskno - 1] == IMAGE_TYPE_PRO) {
			free(((pro_additional_info_t *)additional_info[diskno-1])->count);
		}
		else if (image_type[diskno - 1] == IMAGE_TYPE_VAPI) {
			free(((vapi_additional_info_t *)additional_info[diskno-1])->sectors);
		}
		free(additional_info[diskno - 1]);
		additional_info[diskno - 1] = 0;
	}
}

void SIO_DisableDrive(int diskno)
{
	SIO_Dismount(diskno);
	SIO_drive_status[diskno - 1] = SIO_OFF;
	strcpy(SIO_filename[diskno - 1], "Off");
}

void SIO_SizeOfSector(UBYTE unit, int sector, int *sz, ULONG *ofs)
{
	int size;
	ULONG offset;
	int header_size = (image_type[unit] == IMAGE_TYPE_ATR ? 16 : 0);

	if (BINLOAD_start_binloading) {
		if (sz)
			*sz = 128;
		if (ofs)
			*ofs = 0;
		return;
	}

	if (image_type[unit] == IMAGE_TYPE_PRO) {
		size = 128;
		offset = 16 + (128+12)*(sector -1); /* returns offset of header */
	}
	else if (image_type[unit] == IMAGE_TYPE_VAPI) {
		vapi_additional_info_t *info;
		vapi_sec_info_t *secinfo;

		size = 128;
		info = (vapi_additional_info_t *)additional_info[unit];
		if (info == NULL)
			offset = 0;
		else if (sector > sectorcount[unit])
			offset = 0;
		else {
			secinfo = &info->sectors[sector-1];
			if (secinfo->sec_count == 0  )
				offset = 0;
			else
				offset = secinfo->sec_offset[0];
		}
	}
	else if (sector < 4) {
		/* special case for first three sectors in ATR and XFD image */
		size = 128;
		offset = header_size + (sector - 1) * (boot_sectors_type[unit] == BOOT_SECTORS_PHYSICAL ? 256 : 128);
	}
	else {
		size = sectorsize[unit];
		offset = header_size + (boot_sectors_type[unit] == BOOT_SECTORS_LOGICAL ? 0x180 : 0x300) + (sector - 4) * size;
	}

	if (sz)
		*sz = size;

	if (ofs)
		*ofs = offset;
}

static int SeekSector(int unit, int sector)
{
	ULONG offset;
	int size;

	SIO_last_sector = sector;
	snprintf(SIO_status, sizeof(SIO_status), "%d: %d", unit + 1, sector);
	SIO_SizeOfSector((UBYTE) unit, sector, &size, &offset);
	fseek(disk[unit], offset, SEEK_SET);

	return size;
}

/* Unit counts from zero up */
int SIO_ReadSector(int unit, int sector, UBYTE *buffer)
{
	int size;
	if (BINLOAD_start_binloading)
		return BINLOAD_LoaderStart(buffer);

	io_success[unit] = -1;
	if (SIO_drive_status[unit] == SIO_OFF)
		return 0;
	if (disk[unit] == NULL)
		return 'N';
	if (sector <= 0 || sector > sectorcount[unit])
		return 'E';
	SIO_last_op = SIO_LAST_READ;
	SIO_last_op_time = 1;
	SIO_last_drive = unit + 1;
	/* FIXME: what sector size did the user expect? */
	size = SeekSector(unit, sector);
	if (image_type[unit] == IMAGE_TYPE_PRO) {
		pro_additional_info_t *info;
		unsigned char *count;
		info = (pro_additional_info_t *)additional_info[unit];
		count = info->count;
		if (fread(buffer, 1, 12, disk[unit]) < 12) {
			Log_print("Error in header of .pro image: sector:%d", sector);
			return 'E';
		}
		/* handle duplicate sectors */
		if (buffer[5] != 0) {
			int dupnum = count[sector];
#ifdef DEBUG_PRO
			Log_print("duplicate sector:%d dupnum:%d",sector, dupnum);
#endif
			count[sector] = (count[sector]+1) % (buffer[5]+1);
			if (dupnum != 0)  {
				sector = sectorcount[unit] + buffer[6+dupnum];
				/* can dupnum be 5? */
				if (dupnum > 4 || sector <= 0 || sector > info->max_sector) {
					Log_print("Error in .pro image: sector:%d dupnum:%d", sector, dupnum);
					return 'E';
				}
				size = SeekSector(unit, sector);
				/* read sector header */
				if (fread(buffer, 1, 12, disk[unit]) < 12) {
					Log_print("Error in header2 of .pro image: sector:%d dupnum:%d", sector, dupnum);
					return 'E';
				}
			}
		}
		/* bad sector */
		if (buffer[1] != 0xff) {
			if (fread(buffer, 1, size, disk[unit]) < size) {
				Log_print("Error in bad sector of .pro image: sector:%d", sector);
			}
			io_success[unit] = sector;
#ifdef DEBUG_PRO
			Log_print("bad sector:%d", sector);
#endif
			return 'E';
		}
	}
	else if (image_type[unit] == IMAGE_TYPE_VAPI) {
		vapi_additional_info_t *info;
		vapi_sec_info_t *secinfo;
		ULONG secindex = 0;
		static int lasttrack = 0;
		unsigned int currpos, time, delay, rotations, bestdelay;
/*		unsigned char beststatus;*/
		int fromtrack, trackstostep, j;

		info = (vapi_additional_info_t *)additional_info[unit];
		info->vapi_delay_time = 0;

		if (sector > sectorcount[unit]) {
#ifdef DEBUG_VAPI
			Log_print("bad sector num:%d", sector);
#endif
			info->sec_stat_buff[0] = 9;
			info->sec_stat_buff[1] = 0xFF; 
			info->sec_stat_buff[2] = 0xe0;
			info->sec_stat_buff[3] = 0;
			info->vapi_delay_time= VAPI_CYCLES_BAD_SECTOR_NUM;
			return 'E';
		}

		secinfo = &info->sectors[sector-1];
		fromtrack = lasttrack;
		lasttrack = (sector-1)/18;

		if (secinfo->sec_count == 0) {
#ifdef DEBUG_VAPI
			Log_print("missing sector:%d", sector);
#endif
			info->sec_stat_buff[0] = 0xC;
			info->sec_stat_buff[1] = 0xEF; 
			info->sec_stat_buff[2] = 0xe0;
			info->sec_stat_buff[3] = 0;
			info->vapi_delay_time= VAPI_CYCLES_MISSING_SECTOR;
			return 'E';
		}

		trackstostep = abs((sector-1)/18 - fromtrack);
		time = (unsigned int) ANTIC_CPU_CLOCK;
		if (trackstostep)
			time += trackstostep * VAPI_CYCLES_PER_TRACK_STEP + VAPI_CYCLES_HEAD_SETTLE ;
		time += VAPI_CYCLES_CMD_ACK_TRANS;
		rotations = time/VAPI_CYCLES_PER_ROT;
		currpos = time - rotations*VAPI_CYCLES_PER_ROT;

#ifdef DEBUG_VAPI
		Log_print(" sector:%d sector count :%d time %d", sector,secinfo->sec_count,ANTIC_CPU_CLOCK);
#endif

		bestdelay = 10 * VAPI_CYCLES_PER_ROT;
/*		beststatus = 0;*/
		for (j=0;j<secinfo->sec_count;j++) {
			if (secinfo->sec_rot_pos[j]  < currpos)
				delay = (VAPI_CYCLES_PER_ROT - currpos) + secinfo->sec_rot_pos[j];
			else
				delay = secinfo->sec_rot_pos[j] - currpos; 
#ifdef DEBUG_VAPI
			Log_print("%d %d %d %d %d %x",j,secinfo->sec_rot_pos[j],
					  ((unsigned int) ANTIC_CPU_CLOCK) - ((((unsigned int) ANTIC_CPU_CLOCK)/VAPI_CYCLES_PER_ROT)*VAPI_CYCLES_PER_ROT),
					  currpos,delay,secinfo->sec_status[j]);
#endif
			if (delay < bestdelay) {
				bestdelay = delay;
/*				beststatus = secinfo->sec_status[j];*/
				secindex = j;
			}
		}
		if (trackstostep)
			info->vapi_delay_time = bestdelay + trackstostep * VAPI_CYCLES_PER_TRACK_STEP + 
				     VAPI_CYCLES_HEAD_SETTLE   +  VAPI_CYCLES_TRACK_READ_DELTA +
						       VAPI_CYCLES_CMD_ACK_TRANS + VAPI_CYCLES_SECTOR_READ;
		else
			info->vapi_delay_time = bestdelay + 
						       VAPI_CYCLES_CMD_ACK_TRANS + VAPI_CYCLES_SECTOR_READ;
#ifdef DEBUG_VAPI
		Log_print("Bestdelay = %d VapiDelay = %d",bestdelay,info->vapi_delay_time);
		if (secinfo->sec_count > 1)
			Log_print("duplicate sector:%d dupnum:%d delay:%d",sector, secindex,info->vapi_delay_time);
#endif
		fseek(disk[unit],secinfo->sec_offset[secindex],SEEK_SET);
		info->sec_stat_buff[0] = 0x8 | ((secinfo->sec_status[secindex] == 0xFF) ? 0 : 0x04);
		info->sec_stat_buff[1] = secinfo->sec_status[secindex];
		info->sec_stat_buff[2] = 0xe0;
		info->sec_stat_buff[3] = 0;
		if (secinfo->sec_status[secindex] != 0xFF) {
			if (fread(buffer, 1, size, disk[unit]) < size) {
				Log_print("error reading sector:%d", sector);
			}
			io_success[unit] = sector;
			info->vapi_delay_time += VAPI_CYCLES_PER_ROT + 10000;
#ifdef DEBUG_VAPI
			Log_print("bad sector:%d 0x%0X delay:%d", sector, secinfo->sec_status[secindex],info->vapi_delay_time );
#endif
			{
			int i;
				if (secinfo->sec_status[secindex] == 0xB7) {
					for (i=0;i<128;i++) {
						Log_print("0x%02x",buffer[i]);
						if (buffer[i] == 0x33)
							buffer[i] = rand() & 0xFF;
					}
				}
			}
			return 'E';
		}
#ifdef DEBUG_VAPI
		Log_flushlog();
#endif		
	}
	if (fread(buffer, 1, size, disk[unit]) < size) {
		Log_print("incomplete sector num:%d", sector);
	}
	io_success[unit] = 0;
	return 'C';
}

int SIO_WriteSector(int unit, int sector, const UBYTE *buffer)
{
	int size;
	io_success[unit] = -1;
	if (SIO_drive_status[unit] == SIO_OFF)
		return 0;
	if (disk[unit] == NULL)
		return 'N';
	if (SIO_drive_status[unit] != SIO_READ_WRITE || sector <= 0 || sector > sectorcount[unit])
		return 'E';
	SIO_last_op = SIO_LAST_WRITE;
	SIO_last_op_time = 1;
	SIO_last_drive = unit + 1;
	size = SeekSector(unit, sector);
#ifdef VAPI_WRITE_ENABLE 	
 	if (image_type[unit] == IMAGE_TYPE_VAPI) {
		vapi_additional_info_t *info;
		vapi_sec_info_t *secinfo;

		info = (vapi_additional_info_t *)additional_info[unit];
		secinfo = &info->sectors[sector-1];
		
		if (secinfo->sec_count != 1) {
			/* No writes to sectors with duplicates or missing sectors */
			return 'E';
		}
		
		if (secinfo->sec_status[0] != 0xFF) {
			/* No writes to bad sectors */
			return 'E';
		}
		
		fseek(disk[unit],secinfo->sec_offset[0],SEEK_SET);
		fwrite(buffer, 1, size, disk[unit]);
		io_success[unit] = 0;
		return 'C';
#if 0		
	} else if (image_type[unit] == IMAGE_TYPE_PRO) {
		pro_additional_info_t *info;
		pro_phantom_sec_info_t *phantom;
		
		info = (pro_additional_info_t *)additional_info[unit];
		phantom = &info->phantom[sector-1];
		
		if (phantom->phantom_count != 0) {
			/* No writes to sectors with duplicates */
			return 'E';
		}
		
		size = SeekSector(unit, sector);
		if (buffer[1] != 0xff) {
#endif			
	} 
#endif
	fseek(disk[unit], size, SEEK_SET);
	fwrite(buffer, 1, size, disk[unit]);
	io_success[unit] = 0;
	return 'C';
}

int SIO_FormatDisk(int unit, UBYTE *buffer, int sectsize, int sectcount)
{
	char fname[FILENAME_MAX];
	int is_atr;
	int save_boot_sectors_type;
	int bootsectsize;
	int bootsectcount;
	FILE *f;
	int i;
	io_success[unit] = -1;
	if (SIO_drive_status[unit] == SIO_OFF)
		return 0;
	if (disk[unit] == NULL)
		return 'N';
	if (SIO_drive_status[unit] != SIO_READ_WRITE)
		return 'E';
	/* Note formatting the disk can change size of the file.
	   There is no portable way to truncate the file at given position.
	   We have to close the "rb+" open file and open it in "wb" mode.
	   First get the information about the disk image, because we are going
	   to umount it. */
	memcpy(fname, SIO_filename[unit], FILENAME_MAX);
	is_atr = (image_type[unit] == IMAGE_TYPE_ATR);
	save_boot_sectors_type = boot_sectors_type[unit];
	bootsectsize = 128;
	if (sectsize == 256 && save_boot_sectors_type != BOOT_SECTORS_LOGICAL)
		bootsectsize = 256;
	bootsectcount = sectcount < 3 ? sectcount : 3;
	/* Umount the file and open it in "wb" mode (it will truncate the file) */
	SIO_Dismount(unit + 1);
	f = fopen(fname, "wb");
	if (f == NULL) {
		Log_print("SIO_FormatDisk: failed to open %s for writing", fname);
		return 'E';
	}
	/* Write ATR header if necessary */
	if (is_atr) {
		struct AFILE_ATR_Header header;
		ULONG disksize = (bootsectsize * bootsectcount + sectsize * (sectcount - bootsectcount)) >> 4;
		memset(&header, 0, sizeof(header));
		header.magic1 = AFILE_ATR_MAGIC1;
		header.magic2 = AFILE_ATR_MAGIC2;
		header.secsizelo = (UBYTE) sectsize;
		header.secsizehi = (UBYTE) (sectsize >> 8);
		header.seccountlo = (UBYTE) disksize;
		header.seccounthi = (UBYTE) (disksize >> 8);
		header.hiseccountlo = (UBYTE) (disksize >> 16);
		header.hiseccounthi = (UBYTE) (disksize >> 24);
		fwrite(&header, 1, sizeof(header), f);
	}
	/* Write boot sectors */
	memset(buffer, 0, sectsize);
	for (i = 1; i <= bootsectcount; i++)
		fwrite(buffer, 1, bootsectsize, f);
	/* Write regular sectors */
	for ( ; i <= sectcount; i++)
		fwrite(buffer, 1, sectsize, f);
	/* Close file and mount the disk back */
	fclose(f);
	SIO_Mount(unit + 1, fname, FALSE);
	/* We want to keep the current PHYSICAL/SIO2PC boot sectors type
	   (since the image is blank it can't be figured out by SIO_Mount) */
	if (bootsectsize == 256)
		boot_sectors_type[unit] = save_boot_sectors_type;
	/* Return information for Atari (buffer filled with ff's - no bad sectors) */
	memset(buffer, 0xff, sectsize);
	io_success[unit] = 0;
	return 'C';
}

/* Set density and number of sectors
   This function is used before the format (0x21) command
   to set how the disk will be formatted.
   Note this function does *not* affect the currently attached disk
   (previously sectorsize/sectorcount were used which could result in
   a corrupted image).
*/
int SIO_WriteStatusBlock(int unit, const UBYTE *buffer)
{
	int size;
#ifdef DEBUG
	Log_print("Write Status-Block: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		buffer[0], buffer[1], buffer[2], buffer[3],
		buffer[4], buffer[5], buffer[6], buffer[7],
		buffer[8], buffer[9], buffer[10], buffer[11]);
#endif
	if (SIO_drive_status[unit] == SIO_OFF)
		return 0;
	/* We only care about the density and the sector count here.
	   Setting everything else right here seems to be non-sense.
	   I'm not sure about this density settings, my XF551
	   honors only the sector size and ignores the density */
	size = buffer[6] * 256 + buffer[7];
	if (size == 128 || size == 256)
		SIO_format_sectorsize[unit] = size;
	/* Note that the number of heads are minus 1 */
	SIO_format_sectorcount[unit] = buffer[0] * (buffer[2] * 256 + buffer[3]) * (buffer[4] + 1);
	if (SIO_format_sectorcount[unit] < 1 || SIO_format_sectorcount[unit] > 65535)
		SIO_format_sectorcount[unit] = 720;
	return 'C';
}

int SIO_ReadStatusBlock(int unit, UBYTE *buffer)
{
	UBYTE tracks;
	UBYTE heads;
	int spt;
	if (SIO_drive_status[unit] == SIO_OFF)
		return 0;
	/* default to 1 track, 1 side for non-standard images */
	tracks = 1;
	heads = 1;
	spt = sectorcount[unit];

	if (spt % 40 == 0) {
		/* standard disk */
		tracks = 40;
		spt /= 40;
		if (spt > 26 && spt % 2 == 0) {
			/* double-sided */
			heads = 2;
			spt >>= 1;
			if (spt > 26 && spt % 2 == 0) {
				/* double-sided, 80 tracks */
				tracks = 80;
				spt >>= 1;
			}
		}
	}

	buffer[0] = tracks;              /* # of tracks */
	buffer[1] = 1;                   /* step rate */
	buffer[2] = (UBYTE) (spt >> 8);  /* sectors per track. HI byte */
	buffer[3] = (UBYTE) spt;         /* sectors per track. LO byte */
	buffer[4] = (UBYTE) (heads - 1); /* # of heads minus 1 */
	/* FM for single density, MFM otherwise */
	buffer[5] = (sectorsize[unit] == 128 && sectorcount[unit] <= 720) ? 0 : 4;
	buffer[6] = (UBYTE) (sectorsize[unit] >> 8); /* bytes per sector. HI byte */
	buffer[7] = (UBYTE) sectorsize[unit];        /* bytes per sector. LO byte */
	buffer[8] = 1;                   /* drive is online */
	buffer[9] = 192;                 /* transfer speed, whatever this means */
	buffer[10] = 0;
	buffer[11] = 0;
	return 'C';
}

int SIO_DriveStatus(int unit, UBYTE *buffer)
{
	if (BINLOAD_start_binloading) {
		buffer[0] = 16 + 8;
		buffer[1] = 255;
		buffer[2] = 1;
		buffer[3] = 0 ;
		return 'C';
	}

	if (SIO_drive_status[unit] == SIO_OFF)
		return 0;

	/* .PRO contains status information in the sector header */
	if (io_success[unit] != 0  && image_type[unit] == IMAGE_TYPE_PRO) {
		int sector = io_success[unit];
		SeekSector(unit, sector);
		if (fread(buffer, 1, 4, disk[unit]) < 4) {
			Log_print("SIO_DriveStatus: failed to read sector header");
		}
		return 'C';
	}
	else if (io_success[unit] != 0  && image_type[unit] == IMAGE_TYPE_VAPI &&
			 SIO_drive_status[unit] != SIO_NO_DISK) {
		vapi_additional_info_t *info;
		info = (vapi_additional_info_t *)additional_info[unit];
		buffer[0] = info->sec_stat_buff[0];
		buffer[1] = info->sec_stat_buff[1];
		buffer[2] = info->sec_stat_buff[2];
		buffer[3] = info->sec_stat_buff[3];
		Log_print("Drive Status unit %d %x %x %x %x",unit,buffer[0], buffer[1], buffer[2], buffer[3]);
		return 'C';
	}	
	buffer[0] = 16;         /* drive active */
	buffer[1] = disk[unit] != NULL ? 255 /* WD 177x OK */ : 127 /* no disk */;
	if (io_success[unit] != 0)
		buffer[0] |= 4;     /* failed RW-operation */
	if (SIO_drive_status[unit] == SIO_READ_ONLY)
		buffer[0] |= 8;     /* write protection */
	if (SIO_format_sectorsize[unit] == 256)
		buffer[0] |= 32;    /* double density */
	if (SIO_format_sectorcount[unit] == 1040)
		buffer[0] |= 128;   /* 1050 enhanced density */
	buffer[2] = 1;
	buffer[3] = 0;
	return 'C';
}

#ifndef NO_SECTOR_DELAY
/* A hack for the "Overmind" demo.  This demo verifies if sectors aren't read
   faster than with a typical disk drive.  We introduce a delay
   of SECTOR_DELAY scanlines between successive reads of sector 1. */
#define SECTOR_DELAY 3200
static int delay_counter = 0;
static int last_ypos = 0;
#endif

/* SIO patch emulation routine */
void SIO_Handler(void)
{
	UBYTE devic = MEMORY_dGetByte(0x300);    /* Device ID ($D300 -> $0300) */
	UBYTE comnd = MEMORY_dGetByte(0x302);    /* Command ($D302 -> $0302) */
	UBYTE aux1 = MEMORY_dGetByte(0x303);     /* Aux1 ($D303 -> $0303) */
	UBYTE aux2 = MEMORY_dGetByte(0x304);     /* Aux2 ($D304 -> $0304) */
	UWORD data = MEMORY_dGetWordAligned(0x304); /* Data buffer address ($D304/5 -> $0304/5) */
	UWORD length = MEMORY_dGetWordAligned(0x308); /* Buffer length ($D308/9 -> $0308/9) */
	int sector = (aux2 << 8) | aux1;         /* Combine aux bytes for sector number */
	UBYTE unit;
	UBYTE result = 0x00;

	
	#ifdef USE_FUJINET
    /* Forward SIO commands to FujiNet via NetSIO */
    {
        static int not_connected_counter = 0;
        if (!FujiNet_NetSIO_IsClientConnected()) {
            if (not_connected_counter++ % 1000 == 0) { // Log only every 1000 calls
                Log_print("SIO: FujiNet not connected - returning NACK"); // Changed log message
            }
            CPU_regY = 139; /* DNACK: device does not acknowledge command error */
            CPU_SetN;
            CPU_regA = 0;
            MEMORY_dPutByte(0x023a, CPU_regY);
            MEMORY_dPutByte(0x42, 0);
            CPU_SetC;
            CPU_regPC = 0xe459;
            return;
        } else {
            not_connected_counter = 0; // Reset counter if connected
        }
    }
	#endif


	Log_print("SIO: Raw command dev:%02X cmd:%02X aux1:%02X aux2:%02X", devic, comnd, aux1, aux2);

	/* If FujiNet is enabled, forward ALL SIO traffic */
	if (FujiNet_IsConnected()) {
		Log_print("SIO: FujiNet Enabled - Forwarding command for device %02X (Cmd:%02X Aux1:%02X Aux2:%02X)", devic, comnd, aux1, aux2);
		result = FujiNet_ProcessSIO(devic, comnd, aux1, aux2);
	} else {
		/* FujiNet not enabled - behave as if no device responded */
		Log_print("SIO: FujiNet NOT Enabled - Responding NACK for device %02X (Cmd:%02X)", devic, comnd);
		result = 'N'; /* DNACK: device does not acknowledge command */
		TransferStatus = SIO_NoFrame;
	}

	/* Update registers based on result */
	if (result == 'C' || result == 'A') {
		CPU_regY = 1; /* SUCCES: successful operation */
		CPU_ClrN;
	} else if (result == 'N') {
		CPU_regY = 139; /* DNACK: device does not acknowledge command error */
		CPU_SetN;
	} else if (result == 'E') {
		CPU_regY = 144; /* DERROR: device done (operation incomplete) error */
		CPU_SetN;
	} else {
		CPU_regY = 146; /* FNCNOT: function not implemented in handler error */
		CPU_SetN;
	}
	
	/* Update registers and perform final processing */
	CPU_regA = 0;
	MEMORY_dPutByte(0x023a, CPU_regY);
	MEMORY_dPutByte(0x42, 0);
	CPU_SetC;
	
	/* Set return address */
	CPU_regPC = 0xe459;
}

/* Fixed checksum Which ensures at most two 8-bit folds are applied — same as the standard 8-bit IP-style checksum fold */
UBYTE SIO_ChkSum(const UBYTE *buffer, int length)
{
    unsigned int checksum = 0;
    while (--length >= 0)
        checksum += *buffer++;

    checksum = (checksum & 0xFF) + (checksum >> 8);
    checksum = (checksum & 0xFF) + (checksum >> 8); // in case there's a new carry

    return checksum & 0xFF;
}

/* Flawed checksum that ignores hibyte */
/*
UBYTE SIO_ChkSum(const UBYTE *buffer, int length)
{
	int checksum = 0;
	while (--length >= 0)
		checksum += *buffer++;
	do
		checksum = (checksum & 0xff) + (checksum >> 8);
	while (checksum > 255);
	return checksum;
}
*/


/* Enable/disable command frame processing */
void SIO_SwitchCommandFrame(int onoff)
{
#ifdef DEBUG_SIO
	Log_print("SIO SWITCH COMMAND FRAME %d", onoff);
#endif
	Log_print("SIO DEBUG: SwitchCommandFrame called with onoff=%d", onoff);

#ifdef USE_FUJINET
	/* Disable SIO patch programmatically as a safeguard */
	extern int ESC_enable_sio_patch;
	ESC_enable_sio_patch = 0;
#endif

	if (onoff) { /* Enabled */
		/* When turning on command frame processing, only log unexpected states 
		 * if they're not related to normal operation. The emulator frequently
		 * switches between these states during normal operation. */
		if (TransferStatus != SIO_NoFrame && 
		    TransferStatus != SIO_StatusRead &&  /* State 2 */
		    TransferStatus != SIO_WriteFrame) {
			Log_print("Unexpected command frame at state %x.", TransferStatus);
		}
		
		/* Reset transfer state to receive commands regardless of previous state */
		CommandIndex = 0;
		DataIndex = 0;
		ExpectedBytes = 5;
		TransferStatus = SIO_CommandFrame;
		
#ifdef USE_FUJINET
		/* Initialize FujiNet command polling in the main emulator loop if needed */
		if (fujinet_connected && !fujinet_poll_initialized) {
			/* Set up polling for FujiNet NetSIO responses */
			Log_print("SIO DEBUG: Enabling FujiNet NetSIO packet polling");
			/* Call FujiNet_Update() from Atari800_Frame() to process UDP packets */
			fujinet_poll_initialized = TRUE;
		}
#endif /* USE_FUJINET */
	}
	else { /* Disabled */
		if (TransferStatus == SIO_CommandFrame) {
			/* Only log if we're actually aborting a command in progress */
			Log_print("Command frame aborted.");
			TransferStatus = SIO_NoFrame;
		}
		
#ifdef USE_FUJINET
		/* If we've received a command frame and FujiNet is connected, log it for debugging */
		if (fujinet_connected && TransferStatus == SIO_StatusRead) {
			Log_print("SIO DEBUG: Command received - D:%02X C:%02X A1:%02X A2:%02X CK:%02X", 
				CommandFrame[0], CommandFrame[1], CommandFrame[2], CommandFrame[3], CommandFrame[4]);
		}
#endif
	}
}

/* Intercept SIO command frames and forward to FujiNet if appropriate */
static int SIO_ForwardToFujiNetIfNeeded(void) /* Returns SIO status code if handled, -1 otherwise */
{
#ifdef USE_FUJINET
	/* Check if FujiNet is initialized and connected */
	if (fujinet_enabled && fujinet_sockfd >= 0 && FujiNet_NetSIO_IsClientConnected()) {
		UBYTE device_id = CommandFrame[0];
		/* Check if device ID is for Tape (0x70) or Disk (0x31-0x38) */
		if (device_id == 0x70 || (device_id >= 0x31 && device_id <= 0x38)) { /* TODO: Add check for Printer 0x40? */
			UBYTE command = CommandFrame[1];
			UBYTE aux1 = CommandFrame[2];
			UBYTE aux2 = CommandFrame[3];
			UBYTE status;

			Log_print("SIO: Forwarding command to FujiNet D:%02X C:%02X A1:%02X A2:%02X", device_id, command, aux1, aux2);
			status = FujiNet_ProcessSIO(device_id, command, aux1, aux2);
			/* FujiNet_ProcessSIO now blocks and returns the SIO status */
			/* TransferStatus should have been set by FujiNet_ProcessSIO if data transfer occurred */
			Log_print("SIO: FujiNet returned status %c (0x%02X)", status, status);
			return (int)status; /* Return SIO status code */
		}
	}
#endif
	return -1; /* not handled by FujiNet */
}

/* Patch SIO_ProcessCommandFrame to call our forwarding routine and bypass local handling if needed */
static UBYTE SIO_ProcessCommandFrame(void)
{
	int fujinet_status = SIO_ForwardToFujiNetIfNeeded();
	if (fujinet_status >= 0) {
		/* Command was handled by FujiNet, return the status it provided */
		/* TransferStatus should have been set correctly by FujiNet_ProcessSIO */
		return (UBYTE)fujinet_status;
	}

	UBYTE devic = CommandFrame[0];
	UBYTE commd = CommandFrame[1];
	UBYTE aux1 = CommandFrame[2];
	UBYTE aux2 = CommandFrame[3];
	UBYTE cksum = CommandFrame[4];
	UBYTE status = SIO_NAK; /* Default to error */

	Log_print("SIO DEBUG: ProcessCommandFrame called - D:%02X C:%02X A1:%02X A2:%02X CK:%02X", 
		devic, commd, aux1, aux2, cksum);

	/* Verify checksum */
	if (SIO_ChkSum(CommandFrame, 4) != cksum) {
		Log_print("SIO DEBUG: Checksum error %02x %02x %02x %02x %02x",
			CommandFrame[0], CommandFrame[1], CommandFrame[2],
			CommandFrame[3], CommandFrame[4]);
		TransferStatus = SIO_NoFrame;
		return SIO_ERR; /* Checksum Error */
	}

	switch (devic) {
		/* Disk drive commands (D1:-D8: devices 0x31-0x38) */
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		case 0x35:
		case 0x36:
		case 0x37:
		case 0x38:
			/* Check if drive is available */
			if (disk[devic - 0x31] == NULL && commd != 0x53) {
				Log_print("SIO DEBUG: Drive %d is OFF", devic - 0x30);
				status = 'E';
				TransferStatus = SIO_NoFrame;
			} 
			else {
				/* Process different disk commands */
				switch (commd) {
				case 0x53:	/* Status */
					status = handle_status_request(devic - 0x31);
					break;
				case 0x21:	/* Format drive (no density) */
				case 0x22:	/* Format drive (double density) */
					status = handle_format(devic - 0x31, commd);
					break;
				case 0x52:	/* Read sector */
					status = handle_read_sector(devic - 0x31, aux1 | (aux2 << 8));
					break;
				case 0x57:	/* Write sector (no verify) */
					status = handle_write_sector(devic - 0x31, aux1 | (aux2 << 8), 0);
					break;
				case 0x50:	/* Write sector (verify) */
					status = handle_write_sector(devic - 0x31, aux1 | (aux2 << 8), 1);
					break;
				default:
					Log_print("SIO DEBUG: Unknown disk command %x", commd);
					status = 'E';
					TransferStatus = SIO_NoFrame;
					break;
				}
			}
			break;
			
		/* Handle other devices - these will never be called with FujiNet connected */
		default:
			status = 'N';
			TransferStatus = SIO_NoFrame;
			Log_print("SIO DEBUG: Device %x not handled locally", devic);
			break;
	}

	return status;
}

/* Put Byte into transfer buffer from serial port */
void SIO_PutByte(int byte)
{
	static int LastByte = 0xff;

	/* Update the transfer buffer based on current state */
	switch (TransferStatus) {
	case SIO_CommandFrame:
		if (CommandIndex < 5) {
			CommandFrame[CommandIndex++] = byte;
			if (CommandIndex >= ExpectedBytes)
				TransferStatus = SIO_StatusRead;
		}
		else {
			Log_print("Expected %d command bytes, got %d bytes.",
				ExpectedBytes, CommandIndex + 1);
			TransferStatus = SIO_NoFrame;
		}
		break;
	case SIO_ReadFrame:
		if (DataIndex < 256) {
			DataBuffer[DataIndex++] = byte;
			if (DataIndex >= ExpectedBytes)
				TransferStatus = SIO_NoFrame;
		}
		else {
			Log_print("Expected %d data bytes, got %d bytes.",
				ExpectedBytes, DataIndex + 1);
			TransferStatus = SIO_NoFrame;
		}
		break;
	default:
		break;
	}
	LastByte = byte;
}

/* Get a byte from the transfer buffer */
int SIO_GetByte(void)
{
	if (TransferStatus != SIO_WriteFrame || DataIndex >= ExpectedBytes) {
		return 0xff; /* Nothing to get */
	}
	
	/* Get the next byte from DataBuffer */
	int byte = DataBuffer[DataIndex++];
	
	/* If we've gotten all the data, update the transfer status */
	if (DataIndex >= ExpectedBytes) {
		TransferStatus = SIO_NoFrame;
	}
	
	return byte;
}

/* Rotate disks in drives - swap D1: with D2:, etc. */
int SIO_RotateDisks(void)
{
	int i;
	UBYTE tmp_status;
	char tmp_filename[FILENAME_MAX];
	FILE *tmp_file;
	int tmp_sector_size, tmp_sector_count;
	
	Log_print("SIO: Rotating disks");
	
	/* Save D1: values to temporary variables */
	tmp_status = SIO_drive_status[0];
	strcpy(tmp_filename, SIO_filename[0]);
	tmp_file = disk[0];
	tmp_sector_size = sectorsize[0];
	tmp_sector_count = sectorcount[0];
	
	/* Rotate drive values */
	for (i = 0; i < SIO_MAX_DRIVES - 1; i++) {
		SIO_drive_status[i] = SIO_drive_status[i + 1];
		strcpy(SIO_filename[i], SIO_filename[i + 1]);
		disk[i] = disk[i + 1];
		sectorsize[i] = sectorsize[i + 1];
		sectorcount[i] = sectorcount[i + 1];
	}
	
	/* Put D1: values in D8: */
	SIO_drive_status[SIO_MAX_DRIVES - 1] = tmp_status;
	strcpy(SIO_filename[SIO_MAX_DRIVES - 1], tmp_filename);
	disk[SIO_MAX_DRIVES - 1] = tmp_file;
	sectorsize[SIO_MAX_DRIVES - 1] = tmp_sector_size;
	sectorcount[SIO_MAX_DRIVES - 1] = tmp_sector_count;
	
	return TRUE; /* Success */
}

/* Save SIO state to a file */
void SIO_StateSave(void)
{
	int i;
	
	/* Save disk data (simplified for now) */
	for (i = 0; i < SIO_MAX_DRIVES; i++) {
		/* Save drive status */
		/* In a real implementation, this would save file pointers and disk state */
		/* Here we just log that we would save disk state */
		if (disk[i] != NULL) {
			Log_print("SIO: Would save disk state for drive %d", i + 1);
		}
	}
}

/* Read SIO state from a file */
void SIO_StateRead(void)
{
	int i;
	
	/* Read disk data (simplified for now) */
	for (i = 0; i < SIO_MAX_DRIVES; i++) {
		/* Read drive status */
		/* In a real implementation, this would restore file pointers and disk state */
		/* Here we just log that we would read disk state */
		if (disk[i] != NULL) {
			Log_print("SIO: Would read disk state for drive %d", i + 1);
		}
	}
}

static UBYTE handle_status_request(int diskno)
{
#ifdef USE_FUJINET
	/* Check if FujiNet has a response ready from a previously forwarded command */
	if (fujinet_connected && FujiNet_NetSIO_IsResponseReady()) {
		/* Get the status from FujiNet */
		UBYTE status = FujiNet_NetSIO_GetResponseStatus();
		Log_print("SIO: Using FujiNet status response for disk %d: %c", diskno + 1, status);
		
		/* If successful, get the status data */
		if (status == 'C') {
			/* Copy the status data into the buffer */
			int bytes = FujiNet_NetSIO_GetResponseData(DataBuffer, 4);
			Log_print("SIO: Got %d bytes of status data from FujiNet", bytes);
			ExpectedBytes = bytes;
			DataIndex = 0;
			TransferStatus = SIO_ReadFrame;
		}
		
		return status;
	}
#endif

	if (SIO_drive_status[diskno] == SIO_OFF)
		return 0;
	if (disk[diskno] == NULL)
		return 'N';
	DataBuffer[0] = SIO_drive_status[diskno];
	DataBuffer[1] = 0xff; /* unused */
	DataBuffer[2] = 0; /* no write protect */
	/* Check if disk is read-only */
	if (SIO_drive_status[diskno] == SIO_READ_ONLY)
		DataBuffer[2] = 1; /* write protect */
	DataBuffer[3] = 0; /* motor not running */
	ExpectedBytes = 4;
	DataIndex = 0;
	TransferStatus = SIO_ReadFrame;
	return 'C';
}

static UBYTE handle_format(int diskno, UBYTE comnd)
{
#ifdef USE_FUJINET
	/* Check if FujiNet has a response ready from a previously forwarded command */
	if (fujinet_connected && FujiNet_NetSIO_IsResponseReady()) {
		/* Get the status from FujiNet */
		UBYTE status = FujiNet_NetSIO_GetResponseStatus();
		Log_print("SIO: Using FujiNet format response for disk %d: %c", diskno + 1, status);
		return status;
	}
#endif

	int single_density = comnd == 0x21;
	int sectsize = single_density ? 128 : 256;
	int sectcount = single_density ? 720 : 720;
	UBYTE status = 'E'; /* Default to error */
	
	/* Make sure disk is mounted and writable */
	if (SIO_drive_status[diskno] != SIO_OFF && disk[diskno] != NULL) {
		if (SIO_drive_status[diskno] == SIO_READ_WRITE) {
			/* In real implementation, format the disk here */
			Log_print("SIO: Format drive %d - %d sectors of %d bytes", 
				diskno + 1, sectcount, sectsize);
			status = 'C'; /* Success */
		} else {
			Log_print("SIO: Cannot format write-protected disk %d", diskno + 1);
		}
	} else {
		Log_print("SIO: Cannot format - disk %d not available", diskno + 1);
	}
	
	return status;
}

static UBYTE handle_read_sector(int diskno, int sector)
{
#ifdef USE_FUJINET
	/* Check if FujiNet has a response ready from a previously forwarded command */
	if (fujinet_connected && FujiNet_NetSIO_IsResponseReady()) {
		/* Get the status from FujiNet */
		UBYTE status = FujiNet_NetSIO_GetResponseStatus();
		Log_print("SIO: Using FujiNet read sector response for disk %d, sector %d: %c", 
			diskno + 1, sector, status);
		
		/* If successful, get the sector data */
		if (status == 'C') {
			/* Copy the sector data into the buffer */
			int bytes = FujiNet_NetSIO_GetResponseData(DataBuffer, 256);
			Log_print("SIO: Got %d bytes of sector data from FujiNet", bytes);
			ExpectedBytes = bytes;
			DataIndex = 0;
			TransferStatus = SIO_ReadFrame;
		}
		
		return status;
	}
#endif

	int size;
	int result;
	
	if (SIO_drive_status[diskno] == SIO_OFF)
		return 0;
	if (disk[diskno] == NULL)
		return 'N';
	if (sector <= 0 || sector > sectorcount[diskno])
		return 'E';
	SIO_last_op = SIO_LAST_READ;
	SIO_last_op_time = 1;
	SIO_last_drive = diskno + 1;
	
	/* Calculate sector size based on disk format */
	size = sectorsize[diskno];
	if (size <= 0)
		return 'E';

	/* Read the sector data using the internal SIO function */
	/* For Project Able Archer, this code path shouldn't execute when FujiNet is connected */
	result = SIO_ReadSector(diskno, sector, DataBuffer);
	if (result != 'C') {
		Log_print("SIO: Error reading sector %d from disk %d (result: %c)", 
			sector, diskno + 1, result);
		return 'E';
	}
	
	ExpectedBytes = size;
	DataIndex = 0;
	TransferStatus = SIO_ReadFrame;
	return 'C';
}

static UBYTE handle_write_sector(int diskno, int sector, int verify)
{
#ifdef USE_FUJINET
	/* Check if FujiNet has a response ready from a previously forwarded command */
	if (fujinet_connected && FujiNet_NetSIO_IsResponseReady()) {
		/* Get the status from FujiNet */
		UBYTE status = FujiNet_NetSIO_GetResponseStatus();
		Log_print("SIO: Using FujiNet write sector response for disk %d, sector %d: %c", 
			diskno + 1, sector, status);
		return status;
	}
#endif

	int size;
	
	if (SIO_drive_status[diskno] == SIO_OFF)
		return 0;
	if (disk[diskno] == NULL)
		return 'N';
	if (SIO_drive_status[diskno] != SIO_READ_WRITE || sector <= 0 || sector > sectorcount[diskno])
		return 'E';
	
	/* Get the sector size and seek to sector position */
	size = SeekSector(diskno, sector);
	if (size <= 0)
		return 'E';
		
	ExpectedBytes = size;
	DataIndex = 0;
	TransferStatus = SIO_WriteFrame;

	SIO_last_op = SIO_LAST_WRITE;
	SIO_last_op_time = 1;
	SIO_last_drive = diskno + 1;

	return 'A';
}
