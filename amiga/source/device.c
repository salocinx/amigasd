/*
 *  SPI SD device driver for K1208/Amiga 1200
 *
 *  Copyright (C) 2018 Mike Stirling
 *  Modified in 2020 by Niklas Ekström to work with parallel port to SPI adapter
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <exec/io.h>
#include <exec/devices.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/errors.h>
#include <exec/lists.h>

#include <dos/dos.h>
#include <dos/dostags.h>

#include <libraries/expansion.h>

#include <hardware/cia.h>
#include <hardware/custom.h>
#include <hardware/intbits.h>
//#include <clib/cia_protos.h>
#include <pragmas/cia_pragmas.h>
//#include <clib/misc_protos.h>

#include <devices/trackdisk.h>
#include <devices/scsidisk.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/alib.h>
#include <proto/disk.h>
#include <proto/expansion.h>

#include <stabs.h>

#include "common.h"
#include "sd.h"
#include "spi-par.h"

/* These must be globals and the variable names are important */

const char DevName[] = "spisd.device";
const char DevIdString[] = "spisd 0.4a (10 Jan 2021)";

const UWORD DevVersion = 0;
const UWORD DevRevision = 4;

typedef struct {
	struct Device		*device;
	struct Unit			unit;
} device_ctx_t;

/* Global device context allocated on device init */

static device_ctx_t *ctx;
struct ExecBase *SysBase;

/* Disk change interrupt handling */

#define CLEARINT        SetICR(ciabase, CIAICRF_FLG)
#define DISABLEINT      AbleICR(ciabase, CIAICRF_FLG)
#define ENABLEINT       AbleICR(ciabase, CIAICRF_FLG | CIAICRF_SETCLR)

extern void DiskInt();  							// Prototype for asm interrupt server.
struct Library *ciabase;							// Base library for handling Amiga CIA chips.
struct Interrupt *hw_int;							// Hardware interrupt to detect changes at ACK/FLG line.
struct Interrupt *sw_int = NULL;					// Software interrupt to be triggered when ACK/FLG hardware interrupt was triggered (notifies DOS to ask for TD_CHANGESTATE).
volatile ULONG disk_state = 0;						// Current disk state {0 = disk present, 1 = disk not present}

static void hw_isr() 
{
	SERIAL("Hardware ISR ...\n");
	if(sw_int) {
		SERIAL("    -> Trigger software interrupt.\n");
		Cause(sw_int);
		SERIAL("    -> Change disk state: %ld.\n", disk_state);
		disk_state = disk_state == 0 ? 1 : 0;
	} else {
		SERIAL("    -> No software interrupt stored.\n");
	}
}

static void int_init() 
{
	//BOOL rc = FALSE;
	if ((hw_int = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC|MEMF_CLEAR))) {
		if ((ciabase = OpenResource("ciaa.resource"))) {

			/* Add interrupt to handle CIAICRB_FLG.
			*  This is also cia.resource means of granting exclusive
			*  access to the related registers in the CIAs.
			*/
			hw_int->is_Node.ln_Type = NT_INTERRUPT;
			hw_int->is_Node.ln_Pri  = 127;
			hw_int->is_Node.ln_Name = "CIA_FLG_INT";
			/* ASM interrupt handler */
			//hw_int->is_Data         = (APTR)sw_int;
			//hw_int->is_Code         = DiskInt;
			/* C interrupt handler*/
			hw_int->is_Code         = hw_isr;
			hw_int->is_Data         = (APTR)&disk_state;

			AddICRVector(ciabase, CIAICRB_FLG, hw_int);

	    }
	}
}

static uint32_t device_get_geometry(struct IOStdReq *iostd)
{
	struct DriveGeometry *geom = (struct DriveGeometry*)iostd->io_Data;
	const sd_card_info_t *ci = sd_get_card_info();

	if (ci->type != sdCardType_None) {
		geom->dg_SectorSize = 1 << ci->block_size;
		geom->dg_TotalSectors = ci->capacity >> ci->block_size;
		geom->dg_Cylinders = geom->dg_TotalSectors;
		geom->dg_CylSectors = 1;
		geom->dg_Heads = 1;
		geom->dg_TrackSectors = 1;
		geom->dg_BufMemType = MEMF_PUBLIC;
		geom->dg_DeviceType = DG_DIRECT_ACCESS;
		geom->dg_Flags = DGF_REMOVABLE;
		return 0;
	} else {
		return TDERR_DiskChanged;
	}
}

int __UserDevInit(struct Device *device)
{

	//SERIAL("Device init: spisd.device rev 0.4b (2020)\n");

	/* Open libraries */
	SysBase = *(struct ExecBase**)4l;

	/* Allocate driver context */
	ctx = AllocMem(sizeof(device_ctx_t), MEMF_PUBLIC | MEMF_CLEAR);
	if (ctx == NULL) {
		ERROR("Memory allocation failed\n");
		goto error;
	}
	ctx->device = device;

	/* Initialise hardware */
	spi_init();

	/* Initialize hardware interrupt (CIA/FLG/ACK) */
	int_init();

	/* Return success */
	return 1;

error:
	/* Clean up after failed open */
	return 0;

}

void __UserDevCleanup(void)
{

	SERIAL("Device cleanup ...\n");

	if (ctx) {
		spi_shutdown();

		/* Free context memory */
		FreeMem(ctx, sizeof(device_ctx_t));
		ctx = NULL;
	}

	/* Clean up libs */
}

int __UserDevOpen(struct IORequest *ioreq, uint32_t unit, uint32_t flags)
{

	struct IOStdReq *iostd = (struct IOStdReq*)ioreq;
	int err = IOERR_OPENFAIL;

	SERIAL("Device open ...\n");

	if (iostd && unit == 0) {
		if (sd_open() == 0) {
			/* Device is open */
			iostd->io_Unit = &ctx->unit;
			ctx->unit.unit_flags = UNITF_ACTIVE;
			ctx->unit.unit_OpenCnt = 1;
			err = 0;
		}
	}

	iostd->io_Error = err;
	return err;
}

int __UserDevClose(struct IORequest *ioreq)
{

	SERIAL("Device close ...\n");

	return 0;
}

ADDTABL_1(__BeginIO,a1);

void __BeginIO(struct IORequest *ioreq)
{

	struct IOStdReq *iostd = (struct IOStdReq*)ioreq;
 
	if (ctx == NULL || ioreq == NULL) {
		/* Driver not initialised */
		return;
	}

	iostd->io_Error = 0;

	SERIAL("Device begin IO ...\n");

	switch (iostd->io_Command) {
		case CMD_RESET:
			SERIAL("  CMD_RESET: CMD=%ld\n", iostd->io_Command);
			break;
		case CMD_CLEAR:
			SERIAL("  CMD_CLEAR: CMD=%ld\n", iostd->io_Command);
			break;
		case CMD_UPDATE:
			SERIAL("  CMD_UPDATE: CMD=%ld\n", iostd->io_Command);
			break;
		case TD_MOTOR:
			SERIAL("  TD_MOTOR: CMD=%ld\n", iostd->io_Command);
			break;
		case TD_PROTSTATUS:
			SERIAL("  TD_PROTSTATUS: CMD=%ld\n", iostd->io_Command);
			/* Should return a non-zero value if the card is write protected */
			iostd->io_Actual = 0;
			break;
		case TD_ADDCHANGEINT:
			SERIAL("  TD_ADDCHANGEINT: CMD=%ld\n", iostd->io_Command);
			/* Called when disk-change software interrupt is added to this device */
			if(iostd->io_Data) {
				SERIAL("    -> Storing software interrupt.\n");
				sw_int = (struct Interrupt *)iostd->io_Data;
			} else {
				SERIAL("    -> No software interrupt passed.\n");
				sw_int = NULL;
			}
			break;
		case TD_CHANGENUM:
			SERIAL("  TD_CHANGENUM: CMD=%ld\n", iostd->io_Command);
			/* This should increment each time a disk is inserted */
			break;
		case TD_CHANGESTATE:
			SERIAL("  TD_CHANGESTATE: CMD=%ld\n", iostd->io_Command);
			/* Called after a software interrupt has been caused indicating a disk change */
			/* Should return a non-zero value if the card is invalid or not inserted */
			iostd->io_Actual = disk_state;
			break;
		case TD_REMOVE:
			SERIAL("  TD_REMOVE: CMD=%ld\n", iostd->io_Command);
			/* NULL commands */
			iostd->io_Actual = 0;
			break;
		case TD_REMCHANGEINT:
			SERIAL("  TD_REMCHANGEINT: CMD=%ld\n", iostd->io_Command);
			/* Called when disk-change software interrupt is removed from this device */
			break;
		case TD_GETDRIVETYPE:
			SERIAL("  TD_GETDRIVETYPE: CMD=%ld\n", iostd->io_Command);
			iostd->io_Actual = DG_DIRECT_ACCESS;
			break;
		case TD_GETGEOMETRY:
			SERIAL("  TD_GETGEOMETRY: CMD=%ld\n", iostd->io_Command);
			iostd->io_Actual = 0;
			iostd->io_Error = device_get_geometry(iostd);
			break;
		case TD_FORMAT:
			SERIAL("  TD_FORMAT: CMD=%ld\n", iostd->io_Command);
			break;
		case CMD_WRITE:
			/* FIXME: Should be deferred to task but this did not work reliably - investigate */
			if (sd_write(iostd->io_Data, iostd->io_Offset >> SD_SECTOR_SHIFT, iostd->io_Length >> SD_SECTOR_SHIFT) == 0) {
				iostd->io_Actual = iostd->io_Length;
				iostd->io_Error = 0;
			} else {
				iostd->io_Actual = 0;
				iostd->io_Error = TDERR_NotSpecified;
			}
			SERIAL("  CMD_WRITE: CMD=%ld\n", iostd->io_Command);
			break;
		case CMD_READ:
			if (sd_read(iostd->io_Data, iostd->io_Offset >> SD_SECTOR_SHIFT, iostd->io_Length >> SD_SECTOR_SHIFT) == 0) {
				iostd->io_Actual = iostd->io_Length;
				iostd->io_Error = 0;
			} else {
				iostd->io_Actual = 0;
				iostd->io_Error = TDERR_NotSpecified;
			}
			SERIAL("  CMD_READ: CMD=%ld\n", iostd->io_Command);
			break;
		default:
			SERIAL("  CMD_???: CMD=%ld\n", iostd->io_Command);
			iostd->io_Error = IOERR_NOCMD;
	}

	if (iostd && !(iostd->io_Flags & IOF_QUICK)) {
		/* Reply to message now unless it was deferred to the task or is IOF_QUICK */
		ReplyMsg(&iostd->io_Message);
	}
	
}

ADDTABL_1(__AbortIO,a1);

void __AbortIO(struct IORequest *ioreq)
{

	SERIAL("Device abort io ...\n");

	if (ioreq == NULL) {
		return;
	}

	ioreq->io_Error = IOERR_ABORTED;
	return;
}

ADDTABL_END();
