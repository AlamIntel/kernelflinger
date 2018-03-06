/*
 * Copyright (c) 2017, Intel Corporation
 * All rights reserved.
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
 */
#include <efi.h>
#include <efiapi.h>
#include <efilib.h>
#include "security_interface.h"
#include "rpmb_storage.h"

#define SECURITY_ABL_SEED_LEN 32
#define SECURITY_ABL_SEED_MAX_ENTRIES 4

 /* structure of seed info */
 typedef struct _seed_info {
	 uint8_t svn;
	 uint8_t padding[3];
	 uint8_t seed[SECURITY_ABL_SEED_LEN];
 } __attribute__((packed)) seed_info_t;

 typedef struct device_sec_info{
	 uint32_t size_of_this_struct;
	 /* version info
		 0: baseline structure
		 1: add xx new field
	 */
	/* version of the struct. 0x0001 for this version */
	uint32_t 			Version;
	uint32_t			num_seeds;
	seed_info_t 		seed_list[SECURITY_ABL_SEED_MAX_ENTRIES];
 } __attribute__((packed)) device_sec_info_t;

 EFI_STATUS set_device_security_info(IN VOID *security_data)
 {
 	EFI_STATUS ret;
	device_sec_info_t *dev_sec;
	UINT8 rpmb_key[SECURITY_ABL_SEED_MAX_ENTRIES * RPMB_KEY_SIZE];
	UINT8 i;

	if (!security_data)
		return EFI_INVALID_PARAMETER;

	dev_sec = (device_sec_info_t *)security_data;
	if (dev_sec->size_of_this_struct != sizeof(device_sec_info_t))
		return EFI_INVALID_PARAMETER;

	for (i = 0; i < SECURITY_ABL_SEED_MAX_ENTRIES; i++)
	{
		if (EFI_SUCCESS != derive_rpmb_key_with_seed(dev_sec->seed_list[i].seed, rpmb_key + i * RPMB_KEY_SIZE))
		{
			memset(rpmb_key + i * RPMB_KEY_SIZE, 0, RPMB_KEY_SIZE);
			break;
		}
	}

	if (i > 0)
		ret = set_rpmb_derived_key(rpmb_key, sizeof(rpmb_key), i);
	else
		ret = EFI_NOT_FOUND;

	if (EFI_ERROR(ret))
	{
		efi_perror(ret, L"Failed to generate the rpmb key");
	}

	return ret;
 }
