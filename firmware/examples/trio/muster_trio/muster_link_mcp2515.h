/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* muster_link_mcp2515.h - CAN link for boards without on-chip CAN (Arduino
 * Nano/Uno) using an MCP2515 + TJA1050 module over SPI.
 * Library: autowp/arduino-mcp2515 (PlatformIO: autowp/autowp-mcp2515).
 *
 * Same ID scheme as the TWAI link, so both kinds of board share one bus.
 * Most cheap modules carry an 8 MHz crystal; change MUSTER_MCP_CLOCK if
 * yours says 16.000 on the can.
 *
 * Status: written against the library's documented API; compiles with
 * autowp-mcp2515 for the Nano example. Not yet verified on hardware.
 */
#ifndef MUSTER_LINK_MCP2515_H
#define MUSTER_LINK_MCP2515_H
#include "muster.h"
#include <SPI.h>
#include <mcp2515.h>

#ifndef MUSTER_CAN_BASE
#define MUSTER_CAN_BASE 0x1F000000ul
#endif
#define MUSTER_CAN_MASK 0x1FFFFF00ul
#ifndef MUSTER_MCP_CLOCK
#define MUSTER_MCP_CLOCK MCP_8MHZ
#endif

typedef struct { MCP2515 *mcp; uint8_t self; } muster_mcp_ctx;

static int mmcp_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{
    muster_mcp_ctx *c = (muster_mcp_ctx *)l->ctx;
    struct can_frame fr;
    (void)dst;
    fr.can_id = (MUSTER_CAN_BASE | c->self) | CAN_EFF_FLAG;
    fr.can_dlc = (uint8_t)len;
    memcpy(fr.data, f, len);
    return c->mcp->sendMessage(&fr) == MCP2515::ERROR_OK ? 0 : -1;
}

static uint16_t mmcp_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    muster_mcp_ctx *c = (muster_mcp_ctx *)l->ctx;
    struct can_frame fr;
    while (c->mcp->readMessage(&fr) == MCP2515::ERROR_OK) {
        uint32_t id = fr.can_id & CAN_EFF_MASK;
        if (!(fr.can_id & CAN_EFF_FLAG) || (id & MUSTER_CAN_MASK) != MUSTER_CAN_BASE) continue;
        if (fr.can_dlc > cap) continue;
        *src = (uint8_t)(id & 0xFFu);
        memcpy(f, fr.data, fr.can_dlc);
        return fr.can_dlc;
    }
    return 0;
}

static void mmcp_set_id(muster_link *l, uint8_t id) { ((muster_mcp_ctx *)l->ctx)->self = id; }

static int muster_mcp2515_begin(muster_link *link, muster_mcp_ctx *ctx,
                                MCP2515 *mcp, uint8_t node_id)
{
    mcp->reset();
    if (mcp->setBitrate(CAN_500KBPS, MUSTER_MCP_CLOCK) != MCP2515::ERROR_OK) return -1;
    if (mcp->setNormalMode() != MCP2515::ERROR_OK) return -1;
    ctx->mcp = mcp; ctx->self = node_id;
    link->mtu = 8; link->kbps = 500;
    link->send = mmcp_send; link->recv = mmcp_recv; link->ctx = ctx;
    link->set_id = mmcp_set_id;
    return 0;
}
#endif
