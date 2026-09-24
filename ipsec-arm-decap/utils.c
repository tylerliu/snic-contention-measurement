/*
 * Copyright (c) 2021-2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdnoreturn.h>

#include <doca_version.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_error.h>

#include "utils.h"

DOCA_LOG_REGISTER(UTILS);

doca_error_t open_doca_device_with_pci_and_callback(const char *pci_addr,
						    tasks_check func,
						    open_dev_cb open_dev_cb,
						    void *usr_ctx,
						    struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	uint8_t is_addr_equal = 0;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_addr_equal);
		if (res == DOCA_SUCCESS && is_addr_equal) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			if (open_dev_cb != NULL) {
				res = open_dev_cb(dev_list[i], usr_ctx, retval);
				if (res == DOCA_SUCCESS) {
					doca_devinfo_destroy_list(dev_list);
					return res;
				}
			}
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_pci(const char *pci_addr, tasks_check func, struct doca_dev **retval)
{
	return open_doca_device_with_pci_and_callback(pci_addr, func, NULL, NULL, retval);
}

doca_error_t open_doca_device_with_ibdev_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	char buf[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {};
	char val_copy[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {};
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	/* Setup */
	if (val_size > DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Value size too large. Failed to locate device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(val_copy, value, val_size);

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_ibdev_name(dev_list[i], buf, DOCA_DEVINFO_IBDEV_NAME_SIZE);
		if (res == DOCA_SUCCESS && strncmp(buf, val_copy, val_size) == 0) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_iface_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	char buf[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
	char val_copy[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	/* Setup */
	if (val_size > DOCA_DEVINFO_IFACE_NAME_SIZE) {
		DOCA_LOG_ERR("Value size too large. Failed to locate device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(val_copy, value, val_size);

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_iface_name(dev_list[i], buf, DOCA_DEVINFO_IFACE_NAME_SIZE);
		if (res == DOCA_SUCCESS && strncmp(buf, val_copy, val_size) == 0) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_rep_with_iface_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval,
					      struct doca_dev_rep **rep_retval)
{
	enum doca_pci_func_type pci_func_type;
	struct doca_devinfo **dev_list;
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_devs;
	uint32_t nb_rdevs;
	doca_error_t res;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (size_t i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_pci_func_type(dev_list[i], &pci_func_type);
		if (pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
			continue;
		}

		res = doca_dev_open(dev_list[i], retval);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed open device when searching for interface name %s: %s",
						  (char *)value,
						  doca_error_get_descr(res));
			goto end;
		}

		res = doca_devinfo_rep_create_list(*retval, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_rdevs);
		if (res == DOCA_SUCCESS) {
			for (size_t j = 0; j < nb_rdevs; j++) {
				char rep_iface_name[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
				doca_devinfo_rep_get_iface_name(rep_list[j], rep_iface_name, sizeof(rep_iface_name));
				if (strncmp(rep_iface_name, (char *)value, val_size) != 0) {
					continue;
				}
				if (func != NULL) {
					res = func(dev_list[i]);
					if (res != DOCA_SUCCESS)
						goto end;
				}

				/* if device can be opened */
				res = doca_dev_rep_open(rep_list[j], rep_retval);
				if (res != DOCA_SUCCESS) {
					DOCA_LOG_WARN("Failed open representor with interface name %s: %s",
						(char *)value,
						doca_error_get_descr(res));
				}
				goto end;
			}
			doca_devinfo_rep_destroy_list(rep_list);
		}

		doca_dev_close(*retval);
		*retval = NULL;
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

end:
	doca_devinfo_rep_destroy_list(rep_list);
	doca_dev_close(*retval);
	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_sf_index(uint32_t sf_index, tasks_check func, struct doca_dev **retval)
{
	enum doca_pci_func_type pci_func_type;
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	uint32_t sf_idx;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_pci_func_type(dev_list[i], &pci_func_type);
		if (res == DOCA_SUCCESS && pci_func_type != DOCA_PCI_FUNC_TYPE_SF)
			continue;

		res = doca_devinfo_get_sf_index(dev_list[i], &sf_idx);
		if (res == DOCA_SUCCESS && sf_idx == sf_index) {
			/* If any special capabilities are needed */
			if (func != NULL) {
				res = func(dev_list[i]);
				if (res != DOCA_SUCCESS)
					goto end;
			}

			/* if device can be opened */
			res = doca_dev_open(dev_list[i], retval);
			if (res != DOCA_SUCCESS)
				DOCA_LOG_WARN("Failed open device with SF index %u: %s",
					      sf_index,
					      doca_error_get_descr(res));
			goto end;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

end:
	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_rep_with_sf_index(uint32_t sf_index, tasks_check func, struct doca_dev **retval, struct doca_dev_rep **rep_retval)
{
	enum doca_pci_func_type pci_func_type;
	struct doca_devinfo **dev_list;
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_devs;
	uint32_t nb_rdevs;
	uint32_t sf_idx;
	doca_error_t res;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (size_t i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_pci_func_type(dev_list[i], &pci_func_type);
		if (pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
			continue;
		}

		res = doca_dev_open(dev_list[i], retval);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed open device when searching for SF index %u: %s",
						  sf_index,
						  doca_error_get_descr(res));
			goto end;
		}

		res = doca_devinfo_rep_create_list(*retval, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_rdevs);
		if (res == DOCA_SUCCESS) {
			for (size_t j = 0; j < nb_rdevs; j++) {
				enum doca_pci_func_type rep_pci_func_type;
				doca_devinfo_rep_get_pci_func_type(rep_list[j], &rep_pci_func_type);
				if (rep_pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
					res = doca_devinfo_rep_get_sf_index(rep_list[j], &sf_idx);
					if (res == DOCA_SUCCESS && sf_idx == sf_index) {
						/* If any special capabilities are needed */
						if (func != NULL) {
							res = func(dev_list[i]);
							if (res != DOCA_SUCCESS)
								goto end;
						}

						/* if device can be opened */
						res = doca_dev_rep_open(rep_list[j], rep_retval);
						if (res != DOCA_SUCCESS) {
							DOCA_LOG_WARN("Failed open representor with SF index %u: %s",
								sf_index,
								doca_error_get_descr(res));
						}
						goto end;
					}
				}
			}
			doca_devinfo_rep_destroy_list(rep_list);
		}

		doca_dev_close(*retval);
		*retval = NULL;
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

end:
	doca_devinfo_rep_destroy_list(rep_list);
	doca_dev_close(*retval);
	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_capabilities(tasks_check func, struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t result;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(result));
		return result;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		/* If any special capabilities are needed */
		if (func(dev_list[i]) != DOCA_SUCCESS)
			continue;

		/* If device can be opened */
		if (doca_dev_open(dev_list[i], retval) == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t open_doca_device_rep_with_vuid(struct doca_dev *local,
					    enum doca_devinfo_rep_filter filter,
					    const uint8_t *value,
					    size_t val_size,
					    struct doca_dev_rep **retval)
{
	uint32_t nb_rdevs = 0;
	struct doca_devinfo_rep **rep_dev_list = NULL;
	char val_copy[DOCA_DEVINFO_REP_VUID_SIZE] = {};
	char buf[DOCA_DEVINFO_REP_VUID_SIZE] = {};
	doca_error_t result;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	/* Setup */
	if (val_size > DOCA_DEVINFO_REP_VUID_SIZE) {
		DOCA_LOG_ERR("Value size too large. Ignored");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(val_copy, value, val_size);

	/* Search */
	result = doca_devinfo_rep_create_list(local, filter, &rep_dev_list, &nb_rdevs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create devinfo representor list. Representor devices are available only on DPU, do not run on Host");
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (i = 0; i < nb_rdevs; i++) {
		result = doca_devinfo_rep_get_vuid(rep_dev_list[i], buf, DOCA_DEVINFO_REP_VUID_SIZE);
		if (result == DOCA_SUCCESS && strncmp(buf, val_copy, DOCA_DEVINFO_REP_VUID_SIZE) == 0 &&
		    doca_dev_rep_open(rep_dev_list[i], retval) == DOCA_SUCCESS) {
			doca_devinfo_rep_destroy_list(rep_dev_list);
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_rep_destroy_list(rep_dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t open_doca_device_rep_with_pci(struct doca_dev *local,
					   enum doca_devinfo_rep_filter filter,
					   const char *pci_addr,
					   struct doca_dev_rep **retval)
{
	uint32_t nb_rdevs = 0;
	struct doca_devinfo_rep **rep_dev_list = NULL;
	uint8_t is_addr_equal = 0;
	doca_error_t result;
	size_t i;

	*retval = NULL;

	/* Search */
	result = doca_devinfo_rep_create_list(local, filter, &rep_dev_list, &nb_rdevs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create devinfo representors list. Representor devices are available only on DPU, do not run on Host");
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (i = 0; i < nb_rdevs; i++) {
		result = doca_devinfo_rep_is_equal_pci_addr(rep_dev_list[i], pci_addr, &is_addr_equal);
		if (result == DOCA_SUCCESS && is_addr_equal &&
		    doca_dev_rep_open(rep_dev_list[i], retval) == DOCA_SUCCESS) {
			doca_devinfo_rep_destroy_list(rep_dev_list);
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_rep_destroy_list(rep_dev_list);
	return DOCA_ERROR_NOT_FOUND;
}
noreturn doca_error_t sdk_version_callback(void *param, void *doca_config)
{
	(void)(param);
	(void)(doca_config);

	printf("DOCA SDK     Version (Compilation): %s\n", doca_version());
	printf("DOCA Runtime Version (Runtime):     %s\n", doca_version_runtime());

	/* We assume that when printing DOCA's versions there is no need to continue the program's execution */
	exit(EXIT_SUCCESS);
}

doca_error_t read_file(char const *path, uint8_t **out_bytes, size_t *out_bytes_len)
{
	FILE *file;
	uint8_t *bytes;

	file = fopen(path, "rb");
	if (file == NULL)
		return DOCA_ERROR_NOT_FOUND;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

	long const nb_file_bytes = ftell(file);

	if (nb_file_bytes == -1) {
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

	if (nb_file_bytes == 0) {
		fclose(file);
		return DOCA_ERROR_INVALID_VALUE;
	}

	bytes = malloc(nb_file_bytes);
	if (bytes == NULL) {
		fclose(file);
		return DOCA_ERROR_NO_MEMORY;
	}

	if (fseek(file, 0, SEEK_SET) != 0) {
		free(bytes);
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

	size_t const read_byte_count = fread(bytes, 1, nb_file_bytes, file);

	fclose(file);

	if (read_byte_count != (size_t)nb_file_bytes) {
		free(bytes);
		return DOCA_ERROR_IO_FAILED;
	}

	*out_bytes = bytes;
	*out_bytes_len = read_byte_count;

	return DOCA_SUCCESS;
}

void linear_array_init_u16(uint16_t *array, uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n; i++)
		array[i] = i;
}

#ifndef DOCA_USE_LIBBSD

#ifndef strlcpy

#include <string.h>

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t trimmed_size;
	size_t src_len = strlen(src);

	if (size > 0) {
		trimmed_size = MIN(src_len, (size - 1));

		memcpy(dst, src, trimmed_size);
		dst[trimmed_size] = '\0';
	}

	return src_len;
}

#endif /* strlcpy */

#ifndef strlcat

#include <string.h>

size_t strlcat(char *dst, const char *src, size_t size)
{
	size_t dst_len = strnlen(dst, size);

	if (dst_len >= size)
		return size;

	return dst_len + strlcpy(dst + dst_len, src, size - dst_len);
}

#endif /* strlcat */

#endif /* ! DOCA_USE_LIBBSD */
