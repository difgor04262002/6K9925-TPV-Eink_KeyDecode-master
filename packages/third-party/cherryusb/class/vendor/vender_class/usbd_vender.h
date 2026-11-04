/*
 * Copyright (c) 2023-2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef _USBD_VENDER_H_
#define _USBD_VENDER_H_

#include "usb_vender.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct usbd_interface *usbd_vender_init_intf(uint8_t busid, struct usbd_interface *intf);

int usbd_vender_send_ack_bytes(const uint8_t *p, uint32_t len);
void usbd_vender_kick_pending_ack(void);
void usbd_vender_clear_pending_ack(void);
uint8_t usbd_vender_get_in_ep(void);
uint8_t usbd_vender_get_out_ep(void);

#ifdef __cplusplus
}
#endif

#endif /* _USBD_VENDER_H_ */
