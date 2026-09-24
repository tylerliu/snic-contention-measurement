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

#ifndef UTILS_H_
#define UTILS_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_types.h>
#include <doca_dev.h>

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y)) /* Return the minimum value between X and Y */
#endif

#ifndef MAX
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y)) /* Return the maximum value between X and Y */
#endif

/* Function to check if a given device is capable of executing some task */
typedef doca_error_t (*tasks_check)(struct doca_devinfo *);


typedef doca_error_t (*open_dev_cb)(struct doca_devinfo *devinfo, void *usr_ctx, struct doca_dev **dev);

/*
 * Open a DOCA device according to a given PCI address
 *
 * @pci_addr [in]: PCI address
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_with_pci(const char *pci_addr, tasks_check func, struct doca_dev **retval);

/*
 * Open a DOCA device according to a given PCI address and a callback function
 *
 * @pci_addr [in]: PCI address
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @open_dev_cb [in]: pointer to a function that opens the device
 * @usr_ctx [in]: user context
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_with_pci_and_callback(const char *pci_addr,
						    tasks_check func,
						    open_dev_cb open_dev_cb,
						    void *usr_ctx,
						    struct doca_dev **retval);

/*
 * Open a DOCA device according to a given IB device name
 *
 * @value [in]: IB device name
 * @val_size [in]: input length, in bytes
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_with_ibdev_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval);

/*
 * Open a DOCA device according to a given interface name
 *
 * @value [in]: interface name
 * @val_size [in]: input length, in bytes
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_with_iface_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval);

/*
 * Open a DOCA device representor according to a given interface name
 *
 * @value [in]: interface name
 * @val_size [in]: input length, in bytes
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @retval [out]: pointer to doca_dev_rep struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_rep_with_iface_name(const uint8_t *value, 
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval,
					      struct doca_dev_rep **rep_retval);

/*
 * Open a DOCA device according to a given SF index
 *
 * @sf_index [in]: SF index
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_with_sf_index(uint32_t sf_index, tasks_check func, struct doca_dev **retval);

/*
 * Open a DOCA device according to a given SF index
 *
 * @sf_index [in]: SF index
 * @func [in]: pointer to a function that checks if the device have some task capabilities (Ignored if set to NULL)
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @rep_retval [out]: pointer to doca_dev_rep struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_rep_with_sf_index(uint32_t sf_index, tasks_check func, struct doca_dev **retval, struct doca_dev_rep **rep_retval);

/*
 * Open a DOCA device with a custom set of capabilities
 *
 * @func [in]: pointer to a function that checks if the device have some task capabilities
 * @retval [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_with_capabilities(tasks_check func, struct doca_dev **retval);

/*
 * Open a DOCA device representor according to a given VUID string
 *
 * @local [in]: queries representors of the given local doca device
 * @filter [in]: bitflags filter to narrow the representors in the search
 * @value [in]: IB device name
 * @val_size [in]: input length, in bytes
 * @retval [out]: pointer to doca_dev_rep struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_rep_with_vuid(struct doca_dev *local,
					    enum doca_devinfo_rep_filter filter,
					    const uint8_t *value,
					    size_t val_size,
					    struct doca_dev_rep **retval);

/*
 * Open a DOCA device according to a given PCI address
 *
 * @local [in]: queries representors of the given local doca device
 * @filter [in]: bitflags filter to narrow the representors in the search
 * @pci_addr [in]: PCI address
 * @retval [out]: pointer to doca_dev_rep struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t open_doca_device_rep_with_pci(struct doca_dev *local,
					   enum doca_devinfo_rep_filter filter,
					   const char *pci_addr,
					   struct doca_dev_rep **retval);

/*
 * Prints DOCA SDK and runtime versions
 *
 * @param [in]: unused
 * @doca_config [in]: unused
 * @return: the function exit with EXIT_SUCCESS
 */
doca_error_t sdk_version_callback(void *param, void *doca_config);

/*
 * Read the entire content of a file into a buffer
 *
 * @path [in]: file path
 * @out_bytes [out]: file data buffer
 * @out_bytes_len [out]: file length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t read_file(char const *path, uint8_t **out_bytes, size_t *out_bytes_len);

/*
 * Init a uint16_t array with linear number start from zero
 *
 * @array [in]: pointer to array to init
 * @n [in]: number of element to init
 */
void linear_array_init_u16(uint16_t *array, uint16_t n);

#ifdef DOCA_USE_LIBBSD

#include <bsd/string.h>

#else

#ifndef strlcpy

/*
 * This method wraps our implementation of strlcpy when libbsd is missing
 * @dst [in]: destination string
 * @src [in]: source string
 * @size [in]: size, in bytes, of the destination buffer
 * @return: total length of the string (src) we tried to create
 */
size_t strlcpy(char *dst, const char *src, size_t size);

#endif /* strlcpy */

#ifndef strlcat

/*
 * This method wraps our implementation of strlcat when libbsd is missing
 * @dst [in]: destination string
 * @src [in]: source string
 * @size [in]: size, in bytes, of the destination buffer
 * @return: total length of the string (src) we tried to create
 */
size_t strlcat(char *dst, const char *src, size_t size);

#endif /* strlcat */

#endif /* DOCA_USE_LIBBSD */

#endif /* COMMON_UTILS_H_ */
