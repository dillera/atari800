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
#ifndef BASIC
#include "statesav.h"
#endif
#ifdef USE_FUJINET
#include "fujinet.h"
#endif

/* SIO Status Codes (internal) */
#define SIO_OK             0
#define SIO_ERROR          1
#define SIO_COMPLETE       2
#define SIO_CHECKSUM_ERROR 3

/* SIO Protocol Bytes */
#define SIO_ACK            'A'
#define SIO_NAK            'N'
#define SIO_COMPLETE_FRAME 'C'
#define SIO_ERROR_FRAME    'E'

#define SIO_BUFFER_SIZE    132 /* Command frame (4) + Max data (128) */

/* Forward Declarations */

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
#define VAPI_BYTES_PER_TRACK          (40*26*16)
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
	/* Incompatible VAPI code - struct members missing */
	/* int last_read; */
	/* int last_write; */
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

static int SIO_FrameCounter = 0;
static int SIO_last_result = SIO_OK; /* SIO_OK, SIO_ERROR, SIO_COMPLETE, SIO_CHECKSUM_ERROR */
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
static int TransferStatus = SIO_NoFrame;
static int ExpectedBytes = 0;

int ignore_header_writeprotect = FALSE;

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
	/* Initialize FujiNet with default settings */
	printf("\n\n***** ATTEMPTING TO INITIALIZE FUJINET *****\n");
	fflush(stdout);
	if (FujiNet_Initialise(NULL)) {
		printf("***** FUJINET INITIALIZED SUCCESSFULLY *****\n");
		fflush(stdout);
		Log_print("FujiNet: Initialized successfully");
	}
	else {
		printf("***** FUJINET INITIALIZATION FAILED *****\n");
		fflush(stdout);
		Log_print("FujiNet: Initialization failed");
	}
	printf("***** FUJINET INITIALIZATION COMPLETE *****\n\n");
	fflush(stdout);
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
	Log_print("FujiNet: Shutdown complete");
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
		(void)totalsectors; /* Suppress unused variable warning */

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
	SIO_SizeOfSector((UBYTE)unit, sector, &size, &offset);
	fseek(disk[unit], offset, SEEK_SET);

	return size;
}

int SIO_ReadSector(int unit, int sector, UBYTE *buffer /* UNUSED */)
{
	int size;
	int i = 0;
	int read_result;
	ULONG offset;
	vapi_sec_info_t *secinfo = NULL;
	int index;
	int phantom_count;

	(void)i;      /* Suppress unused variable warning */
	(void)offset; /* Suppress unused variable warning */

	if (disk[unit] == NULL)
		return FALSE;

	if (image_type[unit] == IMAGE_TYPE_PRO) {
		pro_additional_info_t *info = (pro_additional_info_t *)additional_info[unit];
		size = SeekSector(unit, sector);
		if (fread(DataBuffer + 4, 1, 12, disk[unit]) != 12)
			return FALSE;
		if (sector > info->max_sector)
			return FALSE;
		info->count[sector-1]++;
		read_result = fread(DataBuffer + 16, 1, size, disk[unit]);
	}
	else if (image_type[unit] == IMAGE_TYPE_VAPI) {
		vapi_additional_info_t *info;
		vapi_sec_info_t *secinfo;
		int phantom_count;
		int index;

		size = SeekSector(unit, sector);
		info = (vapi_additional_info_t *)additional_info[unit];
		if (info == NULL)
			return FALSE;
		if (sector > sectorcount[unit])
			return FALSE;

		secinfo = &info->sectors[sector-1];
		phantom_count = secinfo->sec_count;
		index = 0;
		if (phantom_count > 1) {
			/* Incompatible VAPI code - struct members missing */
			/* if (secinfo->last_read == -1)
				secinfo->last_read = secinfo->last_write;
			index = (secinfo->last_read + 1) % phantom_count; */
		}

		if (phantom_count == 0)
			return FALSE;
		if (secinfo->sec_offset[index] == 0)
			return FALSE;
		if (fseek(disk[unit],secinfo->sec_offset[index], SEEK_SET) != 0) {
			return(FALSE);
		}
		/* Incompatible VAPI code - struct members missing */
		/* secinfo->last_read = index; */
		read_result = fread(DataBuffer + 4, 1, size, disk[unit]);
	}
	else {
		size = SeekSector(unit, sector);
		read_result = fread(DataBuffer + 4, 1, size, disk[unit]);
	}

	if (read_result != size) {
		if (read_result < 0)
			read_result = 0;
		/* fill rest of buffer with zeros */
		memset(DataBuffer + 4 + read_result, 0, size - read_result);
	}

	return TRUE;
}

int SIO_WriteSector(int unit, int sector, const UBYTE *buffer /* UNUSED */)
{
	int size;
	ULONG offset;

	if (SIO_drive_status[unit] == SIO_READ_ONLY || disk[unit] == NULL)
		return FALSE;

	SIO_SizeOfSector((UBYTE)unit, sector, &size, &offset);

	if (image_type[unit] == IMAGE_TYPE_PRO) {
		int i;
		pro_additional_info_t *info = (pro_additional_info_t *)additional_info[unit];
		size = SeekSector(unit, sector);
		if (info->count[sector-1] > 0) {
			fseek(disk[unit], 12, SEEK_CUR);
		}
		else {
			for (i=0; i<12; i++) {
				if (fputc(0, disk[unit]) != 0) return FALSE;
			}
			info->count[sector-1]++;
		}
		if (fwrite(DataBuffer + 4, 1, size, disk[unit]) != size)
			return FALSE;
	}
	else if (image_type[unit] == IMAGE_TYPE_VAPI) {
		vapi_additional_info_t *info;
		vapi_sec_info_t *secinfo;
		int phantom_count;
		int index;

		size = SeekSector(unit, sector);
		info = (vapi_additional_info_t *)additional_info[unit];
		if (info == NULL)
			return FALSE;
		if (sector > sectorcount[unit])
			return FALSE;

		secinfo = &info->sectors[sector-1];
		phantom_count = secinfo->sec_count;
		index = 0;
		if (phantom_count > 1) {
			/* Incompatible VAPI code - struct members missing */
			/* if (secinfo->last_write == -1)
				secinfo->last_write = secinfo->last_read;
			index = (secinfo->last_write + 1) % phantom_count; */
		}

		if (phantom_count == 0)
			return FALSE;
		if (secinfo->sec_offset[index] == 0)
			return FALSE;
		if (fseek(disk[unit],secinfo->sec_offset[index], SEEK_SET) != 0) {
			return(FALSE);
		}
		/* Incompatible VAPI code - struct members missing */
		/* secinfo->last_write = index; */
		if (fwrite(DataBuffer + 4, 1, size, disk[unit]) != size)
			return FALSE;
	}
	else {
		if (sector < 4 && size == 128 && sectorsize[unit] == 256) {
			/* write 128 byte sector to 256 byte sector */
			size = 256;
			fseek(disk[unit], offset, SEEK_SET);
			if (fwrite(DataBuffer + 4, 1, 128, disk[unit]) != 128) return FALSE;
			fseek(disk[unit], offset + 128, SEEK_SET);
			if (fwrite(DataBuffer + 4, 1, 128, disk[unit]) != 128) return FALSE;
		}
		else {
			fseek(disk[unit], offset, SEEK_SET);
			if (fwrite(DataBuffer + 4, 1, size, disk[unit]) != size) return FALSE;
		}
	}

	return TRUE;
}

int SIO_FormatDisk(int unit, UBYTE *buffer /* UNUSED */, int sectsize /* UNUSED */, int sectcount /* UNUSED */)
{
	int size;
	ULONG offset;
	int i;
	(void)i; /* Suppress unused variable warning */

	if (SIO_drive_status[unit] == SIO_READ_ONLY || disk[unit] == NULL)
		return FALSE;

	size = MEMORY_dGetByte(0x0302);
	sectcount = (MEMORY_dGetByte(0x0301) << 8) + MEMORY_dGetByte(0x0300);
	if ((size != 128 && size != 256) || sectcount == 0)
		return FALSE;

	memset(DataBuffer, 0, size);

	if (image_type[unit] == IMAGE_TYPE_ATR || image_type[unit] == IMAGE_TYPE_XFD) {
		/* Erase whole disk */
		for (i = 1; i <= sectcount; i++) {
			SIO_SizeOfSector((UBYTE)unit, i, &size, &offset);
			fseek(disk[unit], offset, SEEK_SET);
			if (i < 4 && size == 128 && sectorsize[unit] == 256) {
				if (fwrite(DataBuffer, 1, 128, disk[unit]) != 128) return FALSE;
				fseek(disk[unit], offset + 128, SEEK_SET);
				if (fwrite(DataBuffer, 1, 128, disk[unit]) != 128) return FALSE;
			} else {
				if (fwrite(DataBuffer, 1, size, disk[unit]) != size) return FALSE;
			}
		}
		return TRUE;
	}

	return FALSE;
}

int SIO_ReadStatusBlock(int unit, UBYTE *buffer /* UNUSED */)
{
	int sectors_per_track;
	int secsize;

	if (image_type[unit] == IMAGE_TYPE_PRO || image_type[unit] == IMAGE_TYPE_VAPI) {
		sectors_per_track = 18;
		secsize = 128;
	}
	else {
		sectors_per_track = (sectorsize[unit] == 128 ? 18 : 26);
		secsize = sectorsize[unit];
	}

	DataBuffer[4] = (SIO_drive_status[unit] == SIO_READ_ONLY ? 0x80 : 0x00) | 0x04; /* 0x04 -> drive on line */
	DataBuffer[5] = sectorcount[unit] & 0xff;
	DataBuffer[6] = sectorcount[unit] >> 8;
	DataBuffer[7] = 0; /* unused */
	DataBuffer[8] = secsize & 0xff;
	DataBuffer[9] = secsize >> 8;
	DataBuffer[10] = sectors_per_track & 0xff;
	DataBuffer[11] = sectors_per_track >> 8;

	return TRUE;
}

int SIO_WriteStatusBlock(int unit, const UBYTE *buffer /* UNUSED */)
{
	/* This is not used */
	return TRUE;
}

int SIO_DriveStatus(int unit, UBYTE *buffer /* UNUSED */)
{
	/* Request drive status */
	DataBuffer[4] = 0x00; /* drive on line */
	DataBuffer[5] = 0x80; /* timeout ack byte */
	DataBuffer[6] = SIO_drive_status[unit] == SIO_READ_ONLY ? 0x80 : 0x00;
	DataBuffer[7] = 0x00;
	return TRUE;
}

static int Command_Frame(UBYTE *data_buffer)
{
	int data_len;
	UBYTE checksum;
	int frame_resent = 0;
	int timeout = 15;
	static int last_atari_frame_num = -1; /* Redeclare */
	int atari_frame_num;
	int i;
#ifdef USE_FUJINET
	unsigned char response_frame[4]; /* For FujiNet response */
#endif

	(void)timeout; /* Suppress unused variable warning */

	/* Check device ID */
	if (data_buffer[0] != 0x31) { /* Only device D1: supported for now */
		SIO_last_result = SIO_ERROR;
		return SIO_last_result;
	}

	if (SIO_FrameCounter == 0) {
		/* Init */
		SIO_FrameCounter = 1;
	}

	SIO_FrameCounter++;
	atari_frame_num = data_buffer[0];
	if (atari_frame_num == last_atari_frame_num) {
		/* OS is resending the same frame */
		frame_resent = TRUE;
	}
	last_atari_frame_num = atari_frame_num;

	/* Get data length */
	data_len = data_buffer[1];
	if (data_len > SIO_BUFFER_SIZE - 2) {
		SIO_last_result = SIO_CHECKSUM_ERROR;
		goto SendResult;
	}

#ifdef USE_FUJINET
	/* Check if FujiNet is enabled and should handle this command */
	if (FujiNet_IsEnabled()) {
		/* Try to process the command with FujiNet */
		if (FujiNet_ProcessCommand(data_buffer, response_frame)) {
			/* Command was processed by FujiNet */
			/* Map FujiNet response to SIO result codes */
			switch (response_frame[0]) {
			case 'A': /* ACK */
				SIO_last_result = SIO_OK;
				break;
			case 'C': /* COMPLETE */
				SIO_last_result = SIO_COMPLETE;
				break;
			case 'E': /* ERROR */
				SIO_last_result = SIO_ERROR;
				break;
			case 'N': /* NAK */
				SIO_last_result = SIO_CHECKSUM_ERROR;
				break;
			default:
				SIO_last_result = SIO_ERROR;
				break;
			}
			goto SendResult;
		}
		/* If FujiNet didn't handle it, continue with regular SIO processing */
	}
#endif

	/* Verify checksum */
	checksum = 0;
	/* C89: Use pre-declared i */
	for (i = 0; i < data_len; i++) {
		checksum += data_buffer[i + 2];
	}

	if (checksum != data_buffer[data_len + 2]) {
		/* Checksum error */
		SIO_last_result = SIO_CHECKSUM_ERROR;
		goto SendResult;
	}

	/* Command frame looks ok. Process it */
	memcpy(DataBuffer, data_buffer + 2, data_len);

	/* Call SIO_Handler (which operates on global DataBuffer) */
	/* SIO_Handler is void, but sets the global SIO_last_result */
	SIO_Handler();

SendResult:
	/* Now send the result frame back */
	if (SIO_last_result == SIO_OK) {
		/* Send Acknowledge */
		SIO_PutByte(SIO_ACK);
	}
	else if (SIO_last_result == SIO_ERROR) {
		/* Send Error */
		SIO_PutByte(SIO_ERROR_FRAME);
	}
	else if (SIO_last_result == SIO_COMPLETE) {
		/* Send Complete */
		SIO_PutByte(SIO_COMPLETE_FRAME);
	}
	else if (SIO_last_result == SIO_CHECKSUM_ERROR) {
		/* Send NAK */
		SIO_PutByte(SIO_NAK);
	}
	else {
		/* Should not happen */
		SIO_PutByte(SIO_ERROR_FRAME);
	}
	return SIO_last_result;
}

/* Enable/disable the command frame */
void SIO_SwitchCommandFrame(int onoff)
{
	if (onoff)
		TransferStatus = SIO_CommandFrame;
	else
		TransferStatus = SIO_NoFrame;
}

# ifndef NO_SECTOR_DELAY
/* A hack for the "Overmind" demo.  This demo verifies if sectors aren't read
   faster than with a typical disk drive.  We introduce a delay
   of SECTOR_DELAY scanlines between successive reads of sector 1. */

# define SECTOR_DELAY 3
static int delay_counter = 0;
static int last_ypos = 0;

# endif

/* SIO patch emulation routine */
void SIO_Handler(void)
{
	int sector = MEMORY_dGetWordAligned(0x30a);
	int unit = MEMORY_dGetByte(0x300) + MEMORY_dGetByte(0x301) + 0xff;
	UBYTE result = 0x00;
	UWORD data = MEMORY_dGetWordAligned(0x304);
	int length = MEMORY_dGetWordAligned(0x308);
	int realsize = 0;
	int cmd = MEMORY_dGetByte(0x302);

	if ((unsigned int)MEMORY_dGetByte(0x300) + (unsigned int)MEMORY_dGetByte(0x301) > 0xff) {
		/* carry */
		unit++;
	}
	/* A real atari just adds the bytes and 0xff. The result could wrap.*/
	/* XL OS: E99D: LDA $0300 ADC $0301 ADC #$FF STA 023A */

	/* The OS SIO routine copies decide ID do CDEVIC, command ID to CCOMND etc.
	   This operation is not needed with the SIO patch enabled, but we perform
	   it anyway, since some programs rely on that. (E.g. the E.T Phone Home!
	   cartridge would crash with SIO patch enabled.)
	   Note: While on a real XL OS the copying is done only for SIO devices
	   (not for PBI ones), here we copy the values for all types of devices -
	   it's probably harmless. */
	MEMORY_dPutByte(0x023a, unit); /* sta CDEVIC */
	MEMORY_dPutByte(0x023b, cmd); /* sta CCOMND */
	MEMORY_dPutWordAligned(0x023c, sector); /* sta CAUX1; sta CAUX2 */

	/* Disk 1 is ASCII '1' = 0x31 etc */
	/* Disk 1 -> unit = 0 */
	unit -= 0x31;

	if (MEMORY_dGetByte(0x300) != 0x60 && unit < SIO_MAX_DRIVES && (SIO_drive_status[unit] != SIO_OFF || BINLOAD_start_binloading)) {	/* UBYTE range ! */

# ifdef DEBUG
Log_print("SIO disk command is %02x %02x %02x %02x %02x   %02x %02x %02x %02x %02x %02x",
			cmd, MEMORY_dGetByte(0x303), MEMORY_dGetByte(0x304), MEMORY_dGetByte(0x305), MEMORY_dGetByte(0x306),
			MEMORY_dGetByte(0x308), MEMORY_dGetByte(0x309), MEMORY_dGetByte(0x30a), MEMORY_dGetByte(0x30b),
			MEMORY_dGetByte(0x30c), MEMORY_dGetByte(0x30d));

# endif
switch (cmd) {
		case 0x4e:				/* Read Status Block */
			if (12 == length) {
				result = SIO_ReadStatusBlock(unit, DataBuffer);
				if (result == 'C')
					MEMORY_CopyToMem(DataBuffer, data, 12);
			}
			else
				result = 'E';
			break;
		case 0x4f:				/* Write Status Block */
			if (12 == length) {
				MEMORY_CopyFromMem(data, DataBuffer, 12);
				result = SIO_WriteStatusBlock(unit, DataBuffer);
			}
			else
				result = 'E';
			break;
		case 0x50:				/* Write */
		case 0x57:
		case 0xD0:				/* xf551 hispeed */
		case 0xD7:
			SIO_SizeOfSector((UBYTE)unit, sector, &realsize, NULL);
			if (realsize == length) {
				MEMORY_CopyFromMem(data, DataBuffer, realsize);
				result = SIO_WriteSector(unit, sector, DataBuffer);
			}
			else
				result = 'E';
			break;
		case 0x52:				/* Read */
		case 0xD2:				/* xf551 hispeed */

# ifndef NO_SECTOR_DELAY
if (sector == 1) {
				if (delay_counter > 0) {
					if (last_ypos != ANTIC_ypos) {
						last_ypos = ANTIC_ypos;
						delay_counter--;
					}
					CPU_regPC = 0xe459;	/* stay at SIO patch */
					return;
				}
				delay_counter = SECTOR_DELAY;
			}
			else {
				delay_counter = 0;
			}
# endif
SIO_SizeOfSector((UBYTE)unit, sector, &realsize, NULL);
			if (realsize == length) {
				result = SIO_ReadSector(unit, sector, DataBuffer);
				if (result == 'C')
					MEMORY_CopyToMem(DataBuffer, data, realsize);
			}
			else
				result = 'E';
			break;
		case 0x53:				/* Status */
		case 0xD3:				/* xf551 hispeed */
			if (4 == length) {
				result = SIO_DriveStatus(unit, DataBuffer);
				if (result == 'C') {
					MEMORY_CopyToMem(DataBuffer, data, 4);
				}
			}
			else
				result = 'E';
			break;
		/*case 0x66:*/			/* US Doubler Format - I think! */
		case 0x21:				/* Format Disk */
		case 0xA1:				/* xf551 hispeed */
			realsize = SIO_format_sectorsize[unit];
			if (realsize == length) {
				result = SIO_FormatDisk(unit, DataBuffer, realsize, SIO_format_sectorcount[unit]);
				if (result == 'C')
					MEMORY_CopyToMem(DataBuffer, data, realsize);
			}
			else {
				/* there are programs which send the format-command but don't wait for the result (eg xf-tools) */
				SIO_FormatDisk(unit, DataBuffer, realsize, SIO_format_sectorcount[unit]);
				result = 'E';
			}
			break;
		case 0x22:				/* Enhanced Density Format */
		case 0xA2:				/* xf551 hispeed */
			realsize = 128;
			if (realsize == length) {
				result = SIO_FormatDisk(unit, DataBuffer, 128, 1040);
				if (result == 'C')
					MEMORY_CopyToMem(DataBuffer, data, realsize);
			}
			else {
				SIO_FormatDisk(unit, DataBuffer, 128, 1040);
				result = 'E';
			}
			break;
		default:
			result = 'N';
		}
	}
	/* cassette i/o */
	else if (MEMORY_dGetByte(0x300) == 0x60) {
		UBYTE gaps = MEMORY_dGetByte(0x30b);
		switch (cmd){
		case 0x52:	/* read */
			/* set expected Gap */
			CASSETTE_AddGap(gaps == 0 ? 2000 : 160);
			/* get record from storage medium */
			if (CASSETTE_ReadToMemory(data, length))
				result = 'C';
			else
				result = 'E';
			break;
		case 0x50:	/* put (used by AltirraOS) */
		case 0x57:	/* write (used by Atari OS) */
			/* add pregap length */
			CASSETTE_AddGap(gaps == 0 ? 3000 : 260);
			/* write full record to storage medium */
			if (CASSETTE_WriteFromMemory(data, length))
				result = 'C';
			else
				result = 'E';
			break;
		default:
			result = 'N';
		}
	}

	switch (result) {
	case 0x00:					/* Device disabled, generate timeout */
		CPU_regY = 138; /* TIMOUT: peripheral device timeout error */
		CPU_SetN;
		break;
	case 'A':					/* Device acknowledge */
	case 'C':					/* Operation complete */
		CPU_regY = 1; /* SUCCES: successful operation */
		CPU_ClrN;
		break;
	case 'N':					/* Device NAK */
		CPU_regY = 139; /* DNACK: device does not acknowledge command error */
		CPU_SetN;
		break;
	case 'E':					/* Device error */
		CPU_regY = 144; /* DERROR: device done (operation incomplete) error */
		CPU_SetN;
		break;
	default:
		CPU_regY = 146; /* FNCNOT: function not implemented in handler error */
		CPU_SetN;
		break;
	}
	CPU_regA = 0;	/* MMM */
	MEMORY_dPutByte(0x0303, CPU_regY);
	MEMORY_dPutByte(0x42,0);
	CPU_SetC;

	/* After each SIO operation a routine called SENDDS ($EC5F in OSB) is
	   invoked, which, among other functions, silences the sound
	   generators. With SIO patch we don't call SIOV and in effect SENDDS
	   is not called either, but this causes a problem with tape saving.
	   During tape saving sound generators are enabled before calling
	   SIOV, but are not disabled later (no call to SENDDS). The effect is
	   that after saving to tape the unwanted SIO sounds are left audible.
	   To avoid the problem, we silence the sound registers by hand. */
	POKEY_PutByte(POKEY_OFFSET_AUDC1, 0);
	POKEY_PutByte(POKEY_OFFSET_AUDC2, 0);
	POKEY_PutByte(POKEY_OFFSET_AUDC3, 0);
	POKEY_PutByte(POKEY_OFFSET_AUDC4, 0);
}

UBYTE SIO_ChkSum(const UBYTE *buffer, int length)
{

# if 0
/* old, less efficient version */
	int i;
	int checksum = 0;
	for (i = 0; i < length; i++, buffer++) {
		checksum += *buffer;
		while (checksum > 255)
			checksum -= 255;
	}

# else
int checksum = 0;
	while (--length >= 0)
		checksum += *buffer++;
	do
		checksum = (checksum & 0xff) + (checksum >> 8);
	while (checksum > 255);
# endif
	return checksum;
}


static UBYTE WriteSectorBack(void)
{
	UWORD sector;
	int unit;

	sector = CommandFrame[2] + (CommandFrame[3] << 8);
	unit = CommandFrame[0] - '1';
	if (unit >= SIO_MAX_DRIVES)		/* UBYTE range ! */
		return 0;
	switch (CommandFrame[1]) {
	case 0x4f:				/* Write Status Block */
		return SIO_WriteStatusBlock(unit, DataBuffer);
	case 0x50:				/* Write */
	case 0x57:
	case 0xD0:				/* xf551 hispeed */
	case 0xD7:
		return SIO_WriteSector(unit, sector, DataBuffer);
	default:
		return 'E';
	}
}

/* Put a byte that comes out of POKEY. So get it here... */
void SIO_PutByte(int byte)
{
	switch (TransferStatus) {
	case SIO_CommandFrame:
		if (CommandIndex < ExpectedBytes) {
			CommandFrame[CommandIndex++] = byte;
			if (CommandIndex >= ExpectedBytes) {
				if (CommandFrame[0] >= 0x31 && CommandFrame[0] <= 0x38 && (SIO_drive_status[CommandFrame[0]-0x31] != SIO_OFF || BINLOAD_start_binloading)) {
					TransferStatus = SIO_StatusRead;
					POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
				}
				else
					TransferStatus = SIO_NoFrame;
			}
		}
		else {
			Log_print("Invalid command frame!");
			TransferStatus = SIO_NoFrame;
		}
		break;
	case SIO_WriteFrame:		/* Expect data */
		if (DataIndex < ExpectedBytes) {
			DataBuffer[DataIndex++] = byte;
			if (DataIndex >= ExpectedBytes) {
				UBYTE sum = SIO_ChkSum(DataBuffer, ExpectedBytes - 1);
				if (sum == DataBuffer[ExpectedBytes - 1]) {
					UBYTE result = WriteSectorBack();
					if (result != 0) {
						DataBuffer[0] = 'A';
						DataBuffer[1] = result;
						DataIndex = 0;
						ExpectedBytes = 2;
						POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
						TransferStatus = SIO_FinalStatus;
					}
					else
						TransferStatus = SIO_NoFrame;
				}
				else {
					DataBuffer[0] = 'E';
					DataIndex = 0;
					ExpectedBytes = 1;
					POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
					TransferStatus = SIO_FinalStatus;
				}
			}
		}
		else {
			Log_print("Invalid data frame!");
		}
		break;
	}
	CASSETTE_PutByte(byte);
	/* POKEY_DELAYED_SEROUT_IRQ = SIO_SEROUT_INTERVAL; */ /* already set in pokey.c */
}

/* Get a byte from the floppy to the pokey. */
int SIO_GetByte(void)
{
	int byte = 0;

	switch (TransferStatus) {
	case SIO_StatusRead:
		byte = Command_Frame(DataBuffer);		/* Handle now the command */
		break;
	case SIO_FormatFrame:
		TransferStatus = SIO_ReadFrame;
		POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL << 3;
		/* FALL THROUGH */
	case SIO_ReadFrame:
		if (DataIndex < ExpectedBytes) {
			byte = DataBuffer[DataIndex++];
			if (DataIndex >= ExpectedBytes) {
				TransferStatus = SIO_NoFrame;
			}
			else {
				/* set delay using the expected transfer speed */
				POKEY_DELAYED_SERIN_IRQ = (DataIndex == 1) ? SIO_SERIN_INTERVAL
					: ((SIO_SERIN_INTERVAL * POKEY_AUDF[POKEY_CHAN3] - 1) / 0x28 + 1);
			}
		}
		else {
			Log_print("Invalid read frame!");
			TransferStatus = SIO_NoFrame;
		}
		break;
	case SIO_FinalStatus:
		if (DataIndex < ExpectedBytes) {
			byte = DataBuffer[DataIndex++];
			if (DataIndex >= ExpectedBytes) {
				TransferStatus = SIO_NoFrame;
			}
			else {
				if (DataIndex == 0)
					POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
				else
					POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
			}
		}
		else {
			Log_print("Invalid read frame!");
			TransferStatus = SIO_NoFrame;
		}
		break;
	default:
		byte = CASSETTE_GetByte();
		break;
	}
	return byte;
}

#if !defined(BASIC) && !defined(__PLUS)
int SIO_RotateDisks(void)
{
	char tmp_filenames[SIO_MAX_DRIVES][FILENAME_MAX];
	int i;
	int bSuccess = TRUE;

	for (i = 0; i < SIO_MAX_DRIVES; i++) {
		strcpy(tmp_filenames[i], SIO_filename[i]);
		SIO_Dismount(i + 1);
	}

	for (i = 1; i < SIO_MAX_DRIVES; i++) {
		if (strcmp(tmp_filenames[i], "None") && strcmp(tmp_filenames[i], "Off") && strcmp(tmp_filenames[i], "Empty") ) {
			if (!SIO_Mount(i, tmp_filenames[i], FALSE)) /* Note that this is NOT i-1 because SIO_Mount is 1 indexed */
				bSuccess = FALSE;
		}
	}

	i = SIO_MAX_DRIVES - 1;
	while (i > -1 && (!strcmp(tmp_filenames[i], "None") || !strcmp(tmp_filenames[i], "Off") || !strcmp(tmp_filenames[i], "Empty")) ) {
		i--;
	}

	if (i > -1)	{
		if (!SIO_Mount(i + 1, tmp_filenames[0], FALSE))
			bSuccess = FALSE;
	}

	return bSuccess;
}
#endif /* !defined(BASIC) && !defined(__PLUS) */

#ifndef BASIC

void SIO_StateSave(void)
{
	int i;

	for (i = 0; i < 8; i++) {
		StateSav_SaveINT((int *) &SIO_drive_status[i], 1);
		StateSav_SaveFNAME(SIO_filename[i]);
	}
}

void SIO_StateRead(void)
{
	int i;

	for (i = 0; i < 8; i++) {
		int saved_drive_status;
		char filename[FILENAME_MAX];

		StateSav_ReadINT(&saved_drive_status, 1);
		SIO_drive_status[i] = (SIO_UnitStatus)saved_drive_status;

		StateSav_ReadFNAME(filename);
		if (filename[0] == 0)
			continue;

		/* If the disk drive wasn't empty or off when saved,
		   mount the disk */
		switch (saved_drive_status) {
		case SIO_READ_ONLY:
			SIO_Mount(i + 1, filename, TRUE);
			break;
		case SIO_READ_WRITE:
			SIO_Mount(i + 1, filename, FALSE);
			break;
		default:
			break;
		}
	}
}

#endif /* BASIC */
/*
vim:ts=4:sw=4:
*/
