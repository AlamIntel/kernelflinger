/*
 * Copyright (c) 2018, Intel Corporation
 * All rights reserved.
 *
 * Author: Ming Tan <ming.tan@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file defines bootlogic data structures, try to keep it without
 * any external definitions in order to ease export of it.
 */

#include <lib.h>
#include "storage.h"
#include "UsbIo.h"
#include "protocol/DevicePath.h"
#include "protocol/ufs.h"
#include "usb_mass_bot.h"

#define EFI_SCSI_OP_WRITE_10      0x2A

typedef struct {
	UINT8            OpCode;
	UINT8            Lun;
	INT8             Lba[4];
	INT8             Reserved0;
	UINT8            TransferLen[2];
	UINT8            Reserverd1;
	UINT8            Pad[2];
} USB_BOOT_WRITE10_CMD;

typedef struct {
	UINT8             OpCode;
	UINT8             Lun;            ///< Lun (High 3 bits)
	UINT8             Reserved0[2];
	UINT8             AllocLen;       ///< Allocation length
	UINT8             Reserved1;
	UINT8             Pad[6];
} USB_BOOT_REQUEST_SENSE_CMD;

typedef struct {
	UINT8             ErrorCode;
	UINT8             Reserved0;
	UINT8             SenseKey;       ///< Sense key (low 4 bits)
	UINT8             Infor[4];
	UINT8             AddLen;         ///< Additional Sense length, 10
	UINT8             Reserved1[4];
	UINT8             Asc;            ///< Additional Sense Code
	UINT8             Ascq;           ///< Additional Sense Code Qualifier
	UINT8             Reserverd2[4];
} USB_BOOT_REQUEST_SENSE_DATA;
#define USB_REQUEST_SENSE_OPCODE (0x03)
#define USB_WRITE_SAME16_OPCODE (0x93)

static USB_DEVICE_PATH *get_usb_device_path(EFI_DEVICE_PATH *p)
{
	for (; !IsDevicePathEndType(p); p = NextDevicePathNode(p))
		if (DevicePathType(p) == MESSAGING_DEVICE_PATH
				&& DevicePathSubType(p) == MSG_USB_DP)
			return (USB_DEVICE_PATH *)p;

	return NULL;
}

static EFI_STATUS scsi_request_sense(USB_MASS_DEVICE *usb_mass)
{
	USB_BOOT_REQUEST_SENSE_CMD  SenseCmd;
	USB_BOOT_REQUEST_SENSE_DATA SenseData;
	UINT32 cmd_status;
	UINT32 timeout = USB_BOOT_GENERAL_CMD_TIMEOUT;

	ZeroMem(&SenseCmd, sizeof (USB_BOOT_REQUEST_SENSE_CMD));
	ZeroMem(&SenseData, sizeof (USB_BOOT_REQUEST_SENSE_DATA));

	SenseCmd.OpCode   = USB_REQUEST_SENSE_OPCODE;
	SenseCmd.Lun      = (UINT8) ((usb_mass->Lun) << 5);
	SenseCmd.AllocLen = (UINT8) sizeof (USB_BOOT_REQUEST_SENSE_DATA);
	usb_command_with_retry(usb_mass->Context,
									&SenseCmd,
									sizeof(USB_BOOT_REQUEST_SENSE_CMD),
									EfiUsbDataIn,
									&SenseData,
									sizeof(USB_BOOT_REQUEST_SENSE_DATA),
									usb_mass->Lun,
									timeout,
									&cmd_status);

	if (SenseData.SenseKey) {
		debug(L"the last command failed");
		debug(L"SenseKey: 0x%x, Asc: 0x%x, Ascq: 0x%x",
			  SenseData.SenseKey, SenseData.Asc, SenseData.Ascq);
		return EFI_UNSUPPORTED;
	}

	return EFI_SUCCESS;
}

static EFI_STATUS scsi_unmap(EFI_BLOCK_IO *bio,
							 EFI_LBA start,
							 EFI_LBA end)
{
	EFI_STATUS status;
	USB_MASS_DEVICE 		*usb_mass;
	struct command_descriptor_block_unmap cdb;
	struct unmap_parameter unmap;

	usb_mass = (USB_MASS_DEVICE *)(USB_MASS_DEVICE_FROM_BLOCK_IO (bio));
	ZeroMem(&cdb, sizeof(cdb));
	cdb.op_code = UFS_UNMAP;
	cdb.param_length = htobe16(sizeof(unmap));

	ZeroMem(&unmap, sizeof(unmap));
	unmap.data_length = htobe16(sizeof(unmap) - sizeof(unmap.data_length));
	unmap.block_desc_length = htobe16(sizeof(unmap.block_desc));
	unmap.block_desc.lba = htobe64(start);
	unmap.block_desc.count = htobe32(end - start + 1);

	UINT32 timeout = USB_BOOT_GENERAL_CMD_TIMEOUT;
	UINT32 cmd_status;
	status = usb_command_with_retry(usb_mass->Context,
									&cdb,
									sizeof(cdb),
									EfiUsbDataOut,
									&unmap,
									sizeof(unmap),
									usb_mass->Lun,
									timeout,
									&cmd_status);
	if (EFI_ERROR (status))
		return status;

	if (cmd_status) {
		return scsi_request_sense(usb_mass);
	}
	return EFI_SUCCESS;
}

static EFI_STATUS scsi_write_same16(EFI_BLOCK_IO *bio,
									EFI_LBA start,
									EFI_LBA end,
									UINTN block_size,
									BOOLEAN unmap)
{
	EFI_STATUS              status;
	UINT32 cmd_status;
	USB_MASS_DEVICE 		*usb_mass;
	UINT8 write_same[16];
	UINT32 timeout = USB_BOOT_GENERAL_CMD_TIMEOUT;
	VOID *emptyblock;
	VOID *aligned_emptyblock;

	status = alloc_aligned(&emptyblock,
						   &aligned_emptyblock,
						   bio->Media->BlockSize,
						   bio->Media->IoAlign);
	if (EFI_ERROR(status)) {
		debug(L"Can not alloc enough buffer");
		return status;
	}

	usb_mass = (USB_MASS_DEVICE *)(USB_MASS_DEVICE_FROM_BLOCK_IO (bio));
	ZeroMem(write_same, sizeof(write_same));
	write_same[0] = USB_WRITE_SAME16_OPCODE;
	if (unmap)
		write_same[1] = 0x1 << 3; //set UNMAP bit to perform an unmap operation
	*((UINT64 *)&(write_same[2])) = htobe64(start);
	*((UINT32 *)&(write_same[10])) = htobe32(end - start + 1);
	status = usb_command_with_retry(usb_mass->Context,
									write_same,
									sizeof(write_same),
									EfiUsbDataOut,
									aligned_emptyblock,
									block_size,
									usb_mass->Lun,
									timeout,
									&cmd_status);
	if (EFI_ERROR (status)) {
		FreePool(emptyblock);
		return status;
	}

	if (cmd_status) {
		FreePool(emptyblock);
		return scsi_request_sense(usb_mass);
	}

	FreePool(emptyblock);
	return  EFI_SUCCESS;
}

#define BLOCKS (0x2000)
static EFI_STATUS clean_blocks(EFI_BLOCK_IO *bio,
							    EFI_LBA start,
								EFI_LBA end)
{
	USB_MASS_DEVICE 		*usb_mass;
	EFI_STATUS              status;
	VOID *emptyblock;
	VOID *aligned_emptyblock;

	usb_mass = (USB_MASS_DEVICE *)(USB_MASS_DEVICE_FROM_BLOCK_IO (bio));

	status = scsi_write_same16(bio,
							   start,
							   end,
							   bio->Media->BlockSize,
							   FALSE);
	if (!EFI_ERROR(status))
		return status;

	status = alloc_aligned(&emptyblock,
						   &aligned_emptyblock,
						   bio->Media->BlockSize * BLOCKS,
						   bio->Media->IoAlign);
	if (EFI_ERROR(status)) {
		debug(L"Can not alloc enough buffer");
		return status;
	}

	UINT32 cmd_status;
	UINT32 timeout = USB_BOOT_GENERAL_CMD_TIMEOUT;
	USB_BOOT_WRITE10_CMD  WriteCmd;
	EFI_LBA lba;
	UINT32 size;
	UINT32 blocks;

	ZeroMem (&WriteCmd, sizeof (USB_BOOT_WRITE10_CMD));
	WriteCmd.OpCode = EFI_SCSI_OP_WRITE_10;
	WriteCmd.Lun    = (UINT8)(usb_mass->Lun);
	*((UINT16 *) WriteCmd.TransferLen) = htobe16 (BLOCKS);

	lba = start;
	size  =  end  - start + 1;

	for(blocks =  size / BLOCKS; blocks > 0; blocks--) {
		*((UINT32 *) WriteCmd.Lba) = htobe32 (lba);
		status = usb_command_with_retry(usb_mass->Context,
										&WriteCmd,
										sizeof(WriteCmd),
										EfiUsbDataOut,
										aligned_emptyblock,
										bio->Media->BlockSize * BLOCKS,
										usb_mass->Lun,
										timeout,
										&cmd_status);

		if (EFI_ERROR(status)) {
			FreePool(emptyblock);
			return status;
		}
		lba += BLOCKS;
	}

	*((UINT32 *) WriteCmd.Lba) = htobe32 (lba);
	*((UINT16 *) WriteCmd.TransferLen) = htobe16 (size % BLOCKS);
	status = usb_command_with_retry(usb_mass->Context,
									&WriteCmd,
									sizeof(WriteCmd),
									EfiUsbDataOut,
									aligned_emptyblock,
									(bio->Media->BlockSize) * (size % BLOCKS),
									usb_mass->Lun,
									timeout,
									&cmd_status);
	if (EFI_ERROR(status)) {
		FreePool(emptyblock);
		return status;
	}

	return EFI_SUCCESS;
}

static EFI_STATUS usb_erase_blocks(
		__attribute__((unused)) EFI_HANDLE handle,
		EFI_BLOCK_IO *bio,
		EFI_LBA start,
		EFI_LBA end)
{
	EFI_STATUS              status;

	status = scsi_unmap(bio, start, end);
	if (status == EFI_UNSUPPORTED) {
		status = scsi_write_same16(bio,
								   start,
								   end,
								   bio->Media->BlockSize,
								   TRUE);
		if (status == EFI_UNSUPPORTED)
			debug(L"neither unmap nor write same with unmap are supported");
	}

	/*
	 * UNMAP is not a command that forces the SCSI to immediately erase data.
	 * It simply notifies the SCSI which LBAs are no longer needed.
	 * in addition, there are considerable usb mass storage devices don't
	 * support unmap or write_same_with_unmap command, so clean these blocks
	 * even unmap failed, this can be a time-consumming operation.
	 */
	return clean_blocks(bio, start, end);
}

static EFI_STATUS usb_check_logical_unit(__attribute__((unused)) EFI_DEVICE_PATH *p,
					  logical_unit_t log_unit)
{
	return log_unit == LOGICAL_UNIT_USER ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static BOOLEAN is_usb(EFI_DEVICE_PATH *p)
{
	return get_usb_device_path(p) != NULL;
}

struct storage STORAGE(STORAGE_USB) = {
	.erase_blocks = usb_erase_blocks,
	.check_logical_unit = usb_check_logical_unit,
	.probe = is_usb,
	.name = L"USB"
};
