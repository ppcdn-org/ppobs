/*
 * Copyright (c) 2026 Lain Bailey <lain@obsproject.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

// Header-only (no new libobs export surface) so both a C output plugin
// (obs-outputs/rtmp-stream.c) and a C++ one (obs-webrtc/whip-codec-session.cpp)
// can classify the network adapter actually carrying the outbound stream -
// wired/Wi-Fi/cellular - for the OBS log, without duplicating the
// GetBestRoute/GetIfEntry2 lookup in each plugin.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <ntddndis.h>

#include "../platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wired/Wi-Fi/cellular is the distinction that matters for stream quality
// triage; every other NDIS physical medium collapses into "other" rather
// than growing a case for hardware nobody streams over (Bluetooth PAN,
// FireWire, etc.) - see net_adapter_type_name() for the exact strings.
enum net_adapter_type {
	NET_ADAPTER_TYPE_UNKNOWN,
	NET_ADAPTER_TYPE_ETHERNET,
	NET_ADAPTER_TYPE_WIFI,
	NET_ADAPTER_TYPE_CELLULAR,
	NET_ADAPTER_TYPE_OTHER,
};

static inline const char *net_adapter_type_name(enum net_adapter_type type)
{
	switch (type) {
	case NET_ADAPTER_TYPE_ETHERNET:
		return "wired";
	case NET_ADAPTER_TYPE_WIFI:
		return "wifi";
	// Covers both a phone tethered as a mobile hotspot and a built-in/USB
	// LTE modem - Windows reports both as NdisPhysicalMediumWirelessWan;
	// there is no separate NDIS medium for "hotspot" specifically.
	case NET_ADAPTER_TYPE_CELLULAR:
		return "cellular/mobile hotspot";
	case NET_ADAPTER_TYPE_OTHER:
		return "other";
	default:
		return "unknown";
	}
}

// Resolves which network adapter would actually carry traffic to dest_addr
// (IPv4, network byte order) from source_addr (0 = let Windows pick), and
// classifies its physical medium. On any failure (no route, no interface
// info) returns NET_ADAPTER_TYPE_UNKNOWN with *out_description untouched -
// callers must treat that as "could not determine", not "no adapter".
// *out_description is bfree()-owned UTF-8, only set on success.
static inline enum net_adapter_type net_adapter_type_for_route(uint32_t dest_addr, uint32_t source_addr,
								char **out_description)
{
	MIB_IPFORWARDROW route;
	if (GetBestRoute(dest_addr, source_addr, &route) != NO_ERROR)
		return NET_ADAPTER_TYPE_UNKNOWN;

	MIB_IF_ROW2 row;
	memset(&row, 0, sizeof(row));
	row.InterfaceIndex = route.dwForwardIfIndex;
	if (GetIfEntry2(&row) != NO_ERROR)
		return NET_ADAPTER_TYPE_UNKNOWN;

	if (out_description)
		os_wcs_to_utf8_ptr(row.Description, 0, out_description);

	switch (row.PhysicalMediumType) {
	case NdisPhysicalMedium802_3:
		return NET_ADAPTER_TYPE_ETHERNET;
	case NdisPhysicalMediumWirelessLan:
	case NdisPhysicalMediumNative802_11:
		return NET_ADAPTER_TYPE_WIFI;
	case NdisPhysicalMediumWirelessWan:
	case NdisPhysicalMediumWiMax:
		return NET_ADAPTER_TYPE_CELLULAR;
	default:
		return NET_ADAPTER_TYPE_OTHER;
	}
}

#ifdef __cplusplus
}
#endif

#endif // _WIN32
