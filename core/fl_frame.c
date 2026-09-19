/* Freshness Lab — wire codec. SPDX-License-Identifier: MIT */
#include "fl_frame.h"

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t fl_frame_size(uint8_t type)
{
    switch (type) {
    case FL_FT_STATE: return (uint8_t)FL_FRAME_STATE_LEN;
    case FL_FT_EVENT: return (uint8_t)FL_FRAME_EVENT_LEN;
    case FL_FT_ACK_STATE: return (uint8_t)FL_FRAME_ACK_STATE_LEN;
    case FL_FT_ACK_EVENT: return (uint8_t)FL_FRAME_ACK_EVENT_LEN;
    default: return 0;
    }
}

int fl_frame_encode(const fl_frame_t *f, uint8_t *out, uint8_t cap, uint8_t *len_out)
{
    uint8_t n;
    uint8_t i;
    if (f == 0 || out == 0 || len_out == 0) {
        return FL_ERR_ARG;
    }
    n = fl_frame_size(f->type);
    if (n == 0) {
        return FL_ERR_FRAME_TYPE;
    }
    if (cap < n) {
        return FL_ERR_FRAME_LEN;
    }
    out[0] = (uint8_t)FL_FRAME_MAGIC;
    out[1] = f->type;
    put_u16(&out[2], f->session);
    out[4] = n;
    switch (f->type) {
    case FL_FT_STATE:
        if (f->u.state.stream >= FL_MAX_STREAMS) {
            return FL_ERR_FRAME_FIELD;
        }
        out[5] = f->u.state.stream;
        put_u32(&out[6], f->u.state.seq);
        put_u32(&out[10], f->u.state.gen_time);
        for (i = 0; i < (uint8_t)FL_STATE_PAYLOAD_LEN; i++) {
            out[14u + i] = f->u.state.payload[i];
        }
        break;
    case FL_FT_EVENT:
        put_u32(&out[5], f->u.event.id);
        out[9] = f->u.event.kind;
        out[10] = f->u.event.code;
        put_u32(&out[11], f->u.event.gen_time);
        put_u32(&out[15], f->u.event.deadline_abs);
        for (i = 0; i < (uint8_t)FL_EVENT_PAYLOAD_LEN; i++) {
            out[19u + i] = f->u.event.payload[i];
        }
        break;
    case FL_FT_ACK_STATE:
        if (f->u.ack_state.stream >= FL_MAX_STREAMS) {
            return FL_ERR_FRAME_FIELD;
        }
        out[5] = f->u.ack_state.stream;
        put_u32(&out[6], f->u.ack_state.applied_seq);
        put_u32(&out[10], f->u.ack_state.applied_gen);
        break;
    case FL_FT_ACK_EVENT:
        put_u32(&out[5], f->u.ack_event.id);
        out[9] = f->u.ack_event.flags;
        break;
    default:
        return FL_ERR_FRAME_TYPE;
    }
    *len_out = n;
    return FL_OK;
}

int fl_frame_decode(const uint8_t *bytes, uint8_t len, uint16_t expected_session, fl_frame_t *out)
{
    uint8_t type;
    uint8_t n;
    uint8_t i;
    if (bytes == 0 || out == 0) {
        return FL_ERR_ARG;
    }
    if (len < (uint8_t)FL_FRAME_HDR_LEN) {
        return FL_ERR_FRAME_LEN;
    }
    if (bytes[0] != (uint8_t)FL_FRAME_MAGIC) {
        return FL_ERR_FRAME_MAGIC;
    }
    type = bytes[1];
    n = fl_frame_size(type);
    if (n == 0) {
        return FL_ERR_FRAME_TYPE;
    }
    if (bytes[4] != n || len != n) {
        return FL_ERR_FRAME_LEN;
    }
    out->type = type;
    out->session = get_u16(&bytes[2]);
    if (out->session != expected_session) {
        return FL_ERR_FRAME_SESSION;
    }
    switch (type) {
    case FL_FT_STATE:
        out->u.state.stream = bytes[5];
        if (out->u.state.stream >= FL_MAX_STREAMS) {
            return FL_ERR_FRAME_FIELD;
        }
        out->u.state.seq = get_u32(&bytes[6]);
        out->u.state.gen_time = get_u32(&bytes[10]);
        for (i = 0; i < (uint8_t)FL_STATE_PAYLOAD_LEN; i++) {
            out->u.state.payload[i] = bytes[14u + i];
        }
        break;
    case FL_FT_EVENT:
        out->u.event.id = get_u32(&bytes[5]);
        out->u.event.kind = bytes[9];
        out->u.event.code = bytes[10];
        out->u.event.gen_time = get_u32(&bytes[11]);
        out->u.event.deadline_abs = get_u32(&bytes[15]);
        for (i = 0; i < (uint8_t)FL_EVENT_PAYLOAD_LEN; i++) {
            out->u.event.payload[i] = bytes[19u + i];
        }
        if (out->u.event.kind != FL_EV_RAISE && out->u.event.kind != FL_EV_CLEAR) {
            return FL_ERR_FRAME_FIELD;
        }
        break;
    case FL_FT_ACK_STATE:
        out->u.ack_state.stream = bytes[5];
        if (out->u.ack_state.stream >= FL_MAX_STREAMS) {
            return FL_ERR_FRAME_FIELD;
        }
        out->u.ack_state.applied_seq = get_u32(&bytes[6]);
        out->u.ack_state.applied_gen = get_u32(&bytes[10]);
        break;
    case FL_FT_ACK_EVENT:
        out->u.ack_event.id = get_u32(&bytes[5]);
        out->u.ack_event.flags = bytes[9];
        break;
    default:
        return FL_ERR_FRAME_TYPE;
    }
    return FL_OK;
}
