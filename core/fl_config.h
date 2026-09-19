/*
 * Freshness Lab — compile-time capacities and limits.
 * All arrays in the core are sized by these constants. Override with -D.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_CONFIG_H
#define FL_CONFIG_H

#ifndef FL_MAX_STREAMS
#define FL_MAX_STREAMS 8u
#endif
#ifndef FL_EVENT_CAPACITY
#define FL_EVENT_CAPACITY 8u
#endif
#ifndef FL_STATE_PAYLOAD_LEN
#define FL_STATE_PAYLOAD_LEN 8u
#endif
#ifndef FL_EVENT_PAYLOAD_LEN
#define FL_EVENT_PAYLOAD_LEN 8u
#endif
#ifndef FL_RX_DEDUP_WINDOW
#define FL_RX_DEDUP_WINDOW 64u
#endif
#ifndef FL_RX_ACK_QUEUE
#define FL_RX_ACK_QUEUE 4u
#endif

#define FL_MAX_FRAME_LEN 32u

/* Time: uint32 milliseconds; see docs/DESIGN.md §3. */
#define FL_TIME_HORIZON_MS 0x7FFFFFFFu /* 2^31 - 1 */
#define FL_MAX_REL_MS      0x40000000u /* 2^30     */
#define FL_TIME_NONE       0xFFFFFFFFu

/* Wire sizes (docs/DESIGN.md §5). */
#define FL_FRAME_HDR_LEN      5u
#define FL_FRAME_STATE_LEN    (FL_FRAME_HDR_LEN + 1u + 4u + 4u + FL_STATE_PAYLOAD_LEN)
#define FL_FRAME_EVENT_LEN    (FL_FRAME_HDR_LEN + 4u + 1u + 1u + 4u + 4u + FL_EVENT_PAYLOAD_LEN)
#define FL_FRAME_ACK_STATE_LEN (FL_FRAME_HDR_LEN + 1u + 4u + 4u)
#define FL_FRAME_ACK_EVENT_LEN (FL_FRAME_HDR_LEN + 4u + 1u)
#define FL_FRAME_MAGIC 0xF1u

#if FL_MAX_STREAMS > 255u || FL_MAX_STREAMS < 1u
#error "FL_MAX_STREAMS must be 1..255"
#endif
#if FL_EVENT_CAPACITY > 255u || FL_EVENT_CAPACITY < 1u
#error "FL_EVENT_CAPACITY must be 1..255"
#endif
#if FL_RX_DEDUP_WINDOW > 64u || FL_RX_DEDUP_WINDOW < 1u
#error "FL_RX_DEDUP_WINDOW must be 1..64 (single uint64_t bitmap)"
#endif
#if FL_RX_ACK_QUEUE > 255u || FL_RX_ACK_QUEUE < 1u
#error "FL_RX_ACK_QUEUE must be 1..255"
#endif
#if FL_FRAME_STATE_LEN > FL_MAX_FRAME_LEN || FL_FRAME_EVENT_LEN > FL_MAX_FRAME_LEN
#error "payload lengths exceed FL_MAX_FRAME_LEN"
#endif

#endif /* FL_CONFIG_H */
