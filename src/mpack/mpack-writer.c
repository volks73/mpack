/*
 * Copyright (c) 2015 Nicholas Fraser
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define MPACK_INTERNAL 1

#include "mpack-writer.h"

#if MPACK_WRITER

#if MPACK_WRITE_TRACKING
#define MPACK_WRITER_TRACK(writer, error_expr) \
    (((writer)->error == mpack_ok) ? mpack_writer_flag_if_error((writer), (error_expr)) : ((void)0))

MPACK_STATIC_INLINE_SPEED void mpack_writer_flag_if_error(mpack_writer_t* writer, mpack_error_t error) {
    if (error != mpack_ok)
        mpack_writer_flag_error(writer, error);
}
#else
#define MPACK_WRITER_TRACK(writer, error_expr) MPACK_UNUSED(writer)
#endif

MPACK_STATIC_INLINE_SPEED void mpack_writer_track_element(mpack_writer_t* writer) {
    MPACK_WRITER_TRACK(writer, mpack_track_element(&writer->track, false));
}

void mpack_writer_init(mpack_writer_t* writer, char* buffer, size_t size) {
    mpack_assert(buffer != NULL, "cannot initialize writer with empty buffer");
    mpack_memset(writer, 0, sizeof(*writer));
    writer->buffer = buffer;
    writer->size = size;
    MPACK_WRITER_TRACK(writer, mpack_track_init(&writer->track));
}

void mpack_writer_init_error(mpack_writer_t* writer, mpack_error_t error) {
    mpack_memset(writer, 0, sizeof(*writer));
    writer->error = error;
}

#ifdef MPACK_MALLOC
typedef struct mpack_growable_writer_t {
    char** target_data;
    size_t* target_size;
} mpack_growable_writer_t;

static void mpack_growable_writer_flush(mpack_writer_t* writer, const char* data, size_t count) {

    // This is an intrusive flush function which modifies the writer's buffer
    // in response to a flush instead of emptying it in order to add more
    // capacity for data. This removes the need to copy data from a fixed buffer
    // into a growable one, improving performance.
    //
    // There are three ways flush can be called:
    //   - flushing the buffer during writing (used is zero, count is all data, data is buffer)
    //   - flushing extra data during writing (used is all flushed data, count is extra data, data is not buffer)
    //   - flushing during teardown (used and count are both all flushed data, data is buffer)
    //
    // We handle these here, making sure used is the total count in all three cases.
    mpack_log("flush size %i used %i data %p buffer %p\n", (int)writer->size, (int)writer->used, data, writer->buffer);

    // if the given data is not the old buffer, we'll need to actually copy it into the buffer
    bool is_extra_data = (data != writer->buffer);

    // if we're flushing all data (used is zero), we should actually grow
    size_t new_size = writer->size;
    if (writer->used == 0 && count != 0)
        new_size *= 2;
    while (new_size < (is_extra_data ? writer->used + count : count))
        new_size *= 2;

    if (new_size > writer->size) {
        mpack_log("flush growing from %i to %i\n", (int)writer->size, (int)new_size);

        char* new_buffer = (char*)mpack_realloc(writer->buffer, count, new_size);
        if (new_buffer == NULL) {
            mpack_writer_flag_error(writer, mpack_error_memory);
            return;
        }

        writer->buffer = new_buffer;
        writer->size = new_size;
    }

    if (is_extra_data) {
        mpack_memcpy(writer->buffer + writer->used, data, count);
        // add our extra data to count
        writer->used += count;
    } else {
        // used is either zero or count; set it to count
        writer->used = count;
    }
}

static void mpack_growable_writer_teardown(mpack_writer_t* writer) {
    mpack_growable_writer_t* growable_writer = (mpack_growable_writer_t*)writer->context;

    if (mpack_writer_error(writer) == mpack_ok) {

        // shrink the buffer to an appropriate size if the data is
        // much smaller than the buffer
        if (writer->used < writer->size / 2) {
            char* buffer = (char*)mpack_realloc(writer->buffer, writer->used, writer->used);
            if (!buffer) {
                MPACK_FREE(writer->buffer);
                mpack_writer_flag_error(writer, mpack_error_memory);
                return;
            }
            writer->buffer = buffer;
            writer->size = writer->used;
        }

        *growable_writer->target_data = writer->buffer;
        *growable_writer->target_size = writer->used;
        writer->buffer = NULL;

    } else if (writer->buffer) {
        MPACK_FREE(writer->buffer);
        writer->buffer = NULL;
    }

    MPACK_FREE(growable_writer);
    writer->context = NULL;
}

void mpack_writer_init_growable(mpack_writer_t* writer, char** target_data, size_t* target_size) {
    *target_data = NULL;
    *target_size = 0;

    mpack_growable_writer_t* growable_writer = (mpack_growable_writer_t*) MPACK_MALLOC(sizeof(mpack_growable_writer_t));
    if (growable_writer == NULL) {
        mpack_writer_init_error(writer, mpack_error_memory);
        return;
    }
    mpack_memset(growable_writer, 0, sizeof(*growable_writer));

    growable_writer->target_data = target_data;
    growable_writer->target_size = target_size;

    size_t capacity = MPACK_BUFFER_SIZE;
    char* buffer = (char*)MPACK_MALLOC(capacity);
    if (buffer == NULL) {
        MPACK_FREE(growable_writer);
        mpack_writer_init_error(writer, mpack_error_memory);
        return;
    }

    mpack_writer_init(writer, buffer, capacity);
    mpack_writer_set_context(writer, growable_writer);
    mpack_writer_set_flush(writer, mpack_growable_writer_flush);
    mpack_writer_set_teardown(writer, mpack_growable_writer_teardown);
}
#endif

#if MPACK_STDIO
typedef struct mpack_file_writer_t {
    FILE* file;
    char buffer[MPACK_BUFFER_SIZE];
} mpack_file_writer_t;

static void mpack_file_writer_flush(mpack_writer_t* writer, const char* buffer, size_t count) {
    mpack_file_writer_t* file_writer = (mpack_file_writer_t*)writer->context;
    size_t written = fwrite((const void*)buffer, 1, count, file_writer->file);
    if (written != count)
        mpack_writer_flag_error(writer, mpack_error_io);
}

static void mpack_file_writer_teardown(mpack_writer_t* writer) {
    mpack_file_writer_t* file_writer = (mpack_file_writer_t*)writer->context;

    if (file_writer->file) {
        int ret = fclose(file_writer->file);
        file_writer->file = NULL;
        if (ret != 0)
            mpack_writer_flag_error(writer, mpack_error_io);
    }

    MPACK_FREE(file_writer);
}

void mpack_writer_init_file(mpack_writer_t* writer, const char* filename) {
    mpack_file_writer_t* file_writer = (mpack_file_writer_t*) MPACK_MALLOC(sizeof(mpack_file_writer_t));
    if (file_writer == NULL) {
        mpack_writer_init_error(writer, mpack_error_memory);
        return;
    }

    file_writer->file = fopen(filename, "wb");
    if (file_writer->file == NULL) {
        mpack_writer_init_error(writer, mpack_error_io);
        MPACK_FREE(file_writer);
        return;
    }

    mpack_writer_init(writer, file_writer->buffer, sizeof(file_writer->buffer));
    mpack_writer_set_context(writer, file_writer);
    mpack_writer_set_flush(writer, mpack_file_writer_flush);
    mpack_writer_set_teardown(writer, mpack_file_writer_teardown);
}
#endif

void mpack_writer_flag_error(mpack_writer_t* writer, mpack_error_t error) {
    mpack_log("writer %p setting error %i: %s\n", writer, (int)error, mpack_error_to_string(error));

    if (writer->error == mpack_ok) {
        writer->error = error;
        if (writer->error_fn)
            writer->error_fn(writer, writer->error);
    }
}

static void mpack_write_native_big(mpack_writer_t* writer, const char* p, size_t count) {
    if (mpack_writer_error(writer) != mpack_ok)
        return;
    mpack_log("big write for %i bytes from %p, %i space left in buffer\n",
            (int)count, p, (int)(writer->size - writer->used));
    mpack_assert(count > writer->size - writer->used,
            "big write requested for %i bytes, but there is %i available "
            "space in buffer. should have called mpack_write_native() instead",
            (int)count, (int)(writer->size - writer->used));

    // we'll need a flush function
    if (!writer->flush) {
        mpack_writer_flag_error(writer, mpack_error_io);
        return;
    }

    // we assume that the flush function is orders of magnitude slower
    // than memcpy(), so we fill the buffer up first to try to flush as
    // infrequently as possible.
    
    // fill the remaining space in the buffer
    size_t n = writer->size - writer->used;
    if (count < n)
        n = count;
    mpack_memcpy(writer->buffer + writer->used, p, n);
    writer->used += n;
    p += n;
    count -= n;
    if (count == 0)
        return;

    // flush the buffer
    size_t used = writer->used;
    writer->used = 0;
    writer->flush(writer, writer->buffer, used);
    if (mpack_writer_error(writer) != mpack_ok)
        return;

    // note that an intrusive flush function (such as mpack_growable_writer_flush())
    // may have changed size and/or reset used to a non-zero value. we treat both as
    // though they may have changed, and there may still be data in the buffer.

    // flush the extra data directly if it doesn't fit in the buffer
    if (count > writer->size - writer->used) {
        writer->flush(writer, p, count);
        if (mpack_writer_error(writer) != mpack_ok)
            return;
    } else {
        mpack_memcpy(writer->buffer + writer->used, p, count);
        writer->used += count;
    }
}

MPACK_STATIC_INLINE_SPEED void mpack_write_native(mpack_writer_t* writer, const char* p, size_t count) {
    if (writer->size - writer->used < count) {
        mpack_write_native_big(writer, p, count);
    } else {
        mpack_memcpy(writer->buffer + writer->used, p, count);
        writer->used += count;
    }
}

MPACK_STATIC_ALWAYS_INLINE void mpack_write_native_u8(mpack_writer_t* writer, uint8_t value) {
    mpack_write_native(writer, (char*)&value, sizeof(value));
}

MPACK_STATIC_ALWAYS_INLINE size_t mpack_store_native_u8(char* p, uint8_t val) {
    uint8_t* u = (uint8_t*)p;
    u[0] = val;
    return sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_u16(char* p, uint16_t val) {
    uint8_t* u = (uint8_t*)p;
    u[0] = (uint8_t)((val >> 8) & 0xFF);
    u[1] = (uint8_t)( val       & 0xFF);
    return sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_u32(char* p, uint32_t val) {
    uint8_t* u = (uint8_t*)p;
    u[0] = (uint8_t)((val >> 24) & 0xFF);
    u[1] = (uint8_t)((val >> 16) & 0xFF);
    u[2] = (uint8_t)((val >>  8) & 0xFF);
    u[3] = (uint8_t)( val        & 0xFF);
    return sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_u64(char* p, uint64_t val) {
    uint8_t* u = (uint8_t*)p;
    u[0] = (uint8_t)((val >> 56) & 0xFF);
    u[1] = (uint8_t)((val >> 48) & 0xFF);
    u[2] = (uint8_t)((val >> 40) & 0xFF);
    u[3] = (uint8_t)((val >> 32) & 0xFF);
    u[4] = (uint8_t)((val >> 24) & 0xFF);
    u[5] = (uint8_t)((val >> 16) & 0xFF);
    u[6] = (uint8_t)((val >>  8) & 0xFF);
    u[7] = (uint8_t)( val        & 0xFF);
    return sizeof(val);
}

MPACK_STATIC_ALWAYS_INLINE size_t mpack_store_native_i8(char* p, int8_t val) {return mpack_store_native_u8(p, (uint8_t)val);}
MPACK_STATIC_ALWAYS_INLINE size_t mpack_store_native_i16(char* p, int16_t val) {return mpack_store_native_u16(p, (uint16_t)val);}
MPACK_STATIC_ALWAYS_INLINE size_t mpack_store_native_i32(char* p, int32_t val) {return mpack_store_native_u32(p, (uint32_t)val);}
MPACK_STATIC_ALWAYS_INLINE size_t mpack_store_native_i64(char* p, int64_t val) {return mpack_store_native_u64(p, (uint64_t)val);}

MPACK_STATIC_INLINE size_t mpack_store_native_float(char* p, float value) {
    union {
        float f;
        uint32_t i;
    } u;
    u.f = value;
    mpack_store_native_u32(p, u.i);
    return sizeof(value);
}

MPACK_STATIC_INLINE size_t mpack_store_native_double(char* p, double value) {
    union {
        double d;
        uint64_t i;
    } u;
    u.d = value;
    mpack_store_native_u64(p, u.i);
    return sizeof(value);
}

MPACK_STATIC_INLINE size_t mpack_store_native_pair_u8(char* p, uint8_t header, uint8_t val) {
    mpack_store_native_u8(p, header);
    mpack_store_native_u8(p + sizeof(header), val);
    return sizeof(header) + sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_pair_u16(char* p, uint8_t header, uint16_t val) {
    mpack_store_native_u8(p, header);
    mpack_store_native_u16(p + sizeof(header), val);
    return sizeof(header) + sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_pair_u32(char* p, uint8_t header, uint32_t val) {
    mpack_store_native_u8(p, header);
    mpack_store_native_u32(p + sizeof(header), val);
    return sizeof(header) + sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_pair_u64(char* p, uint8_t header, uint64_t val) {
    mpack_store_native_u8(p, header);
    mpack_store_native_u64(p + sizeof(header), val);
    return sizeof(header) + sizeof(val);
}

MPACK_STATIC_INLINE size_t mpack_store_native_pair_i8(char* p, uint8_t header, int8_t val) {return mpack_store_native_pair_u8(p, header, (uint8_t)val);}
MPACK_STATIC_INLINE size_t mpack_store_native_pair_i16(char* p, uint8_t header, int16_t val) {return mpack_store_native_pair_u16(p, header, (uint16_t)val);}
MPACK_STATIC_INLINE size_t mpack_store_native_pair_i32(char* p, uint8_t header, int32_t val) {return mpack_store_native_pair_u32(p, header, (uint32_t)val);}
MPACK_STATIC_INLINE size_t mpack_store_native_pair_i64(char* p, uint8_t header, int64_t val) {return mpack_store_native_pair_u64(p, header, (uint64_t)val);}

mpack_error_t mpack_writer_destroy(mpack_writer_t* writer) {

    // clean up tracking, asserting if we're not already in an error state
    #if MPACK_WRITE_TRACKING
    mpack_track_destroy(&writer->track, writer->error != mpack_ok);
    #endif

    // flush any outstanding data
    if (mpack_writer_error(writer) == mpack_ok && writer->used != 0 && writer->flush != NULL) {
        writer->flush(writer, writer->buffer, writer->used);
        writer->flush = NULL;
    }

    if (writer->teardown) {
        writer->teardown(writer);
        writer->teardown = NULL;
    }

    return writer->error;
}

void mpack_write_tag(mpack_writer_t* writer, mpack_tag_t value) {
    switch (value.type) {
        case mpack_type_nil:    mpack_writer_track_element(writer); mpack_write_nil   (writer);            break;
        case mpack_type_bool:   mpack_writer_track_element(writer); mpack_write_bool  (writer, value.v.b); break;
        case mpack_type_float:  mpack_writer_track_element(writer); mpack_write_float (writer, value.v.f); break;
        case mpack_type_double: mpack_writer_track_element(writer); mpack_write_double(writer, value.v.d); break;
        case mpack_type_int:    mpack_writer_track_element(writer); mpack_write_int   (writer, value.v.i); break;
        case mpack_type_uint:   mpack_writer_track_element(writer); mpack_write_uint  (writer, value.v.u); break;

        case mpack_type_str: mpack_start_str(writer, value.v.l); break;
        case mpack_type_bin: mpack_start_bin(writer, value.v.l); break;
        case mpack_type_ext: mpack_start_ext(writer, value.exttype, value.v.l); break;

        case mpack_type_array: mpack_start_array(writer, value.v.n); break;
        case mpack_type_map:   mpack_start_map(writer, value.v.n);   break;

        default:
            mpack_assert(0, "unrecognized type %i", (int)value.type);
            break;
    }
}

void mpack_write_u8(mpack_writer_t* writer, uint8_t value) {
    mpack_writer_track_element(writer);

    char encoded[2];
    size_t size;

    if (value <= 0x7f) {
        size = mpack_store_native_u8(encoded, value);
    } else {
        size = mpack_store_native_pair_u8(encoded, 0xcc, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_u16(mpack_writer_t* writer, uint16_t value) {
    mpack_writer_track_element(writer);

    char encoded[3];
    size_t size;

    if (value <= 0x7f) {
        size = mpack_store_native_u8(encoded, (uint8_t)value);
    } else if (value <= UINT8_MAX) {
        size = mpack_store_native_pair_u8(encoded, 0xcc, (uint8_t)value);
    } else {
        size = mpack_store_native_pair_u16(encoded, 0xcd, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_u32(mpack_writer_t* writer, uint32_t value) {
    mpack_writer_track_element(writer);

    char encoded[5];
    size_t size;

    if (value <= 0x7f) {
        size = mpack_store_native_u8(encoded, (uint8_t)value);
    } else if (value <= UINT8_MAX) {
        size = mpack_store_native_pair_u8(encoded, 0xcc, (uint8_t)value);
    } else if (value <= UINT16_MAX) {
        size = mpack_store_native_pair_u16(encoded, 0xcd, (uint16_t)value);
    } else {
        size = mpack_store_native_pair_u32(encoded, 0xce, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_u64(mpack_writer_t* writer, uint64_t value) {
    mpack_writer_track_element(writer);

    char encoded[9];
    size_t size;

    if (value <= 0x7f) {
        size = mpack_store_native_u8(encoded, (uint8_t)value);
    } else if (value <= UINT8_MAX) {
        size = mpack_store_native_pair_u8(encoded, 0xcc, (uint8_t)value);
    } else if (value <= UINT16_MAX) {
        size = mpack_store_native_pair_u16(encoded, 0xcd, (uint16_t)value);
    } else if (value <= UINT32_MAX) {
        size = mpack_store_native_pair_u32(encoded, 0xce, (uint32_t)value);
    } else {
        size = mpack_store_native_pair_u64(encoded, 0xcf, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_i8(mpack_writer_t* writer, int8_t value) {

    // write any non-negative number as a uint
    if (value >= 0) {
        mpack_write_u8(writer, (uint8_t)value);
        return;
    }

    mpack_writer_track_element(writer);

    char encoded[2];
    size_t size;

    if (value >= -32) {
        size = mpack_store_native_i8(encoded, value);
    } else {
        size = mpack_store_native_pair_i8(encoded, 0xd0, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_i16(mpack_writer_t* writer, int16_t value) {

    // write any non-negative number as a uint
    if (value >= 0) {
        mpack_write_u16(writer, (uint16_t)value);
        return;
    }

    mpack_writer_track_element(writer);

    char encoded[3];
    size_t size;

    if (value >= -32) {
        size = mpack_store_native_i8(encoded, (int8_t)value);
    } else if (value >= INT8_MIN) {
        size = mpack_store_native_pair_i8(encoded, 0xd0, (int8_t)value);
    } else {
        size = mpack_store_native_pair_i16(encoded, 0xd1, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_i32(mpack_writer_t* writer, int32_t value) {

    // write any non-negative number as a uint
    if (value >= 0) {
        mpack_write_u32(writer, (uint32_t)value);
        return;
    }

    mpack_writer_track_element(writer);

    char encoded[5];
    size_t size;

    if (value >= -32) {
        size = mpack_store_native_i8(encoded, (int8_t)value);
    } else if (value >= INT8_MIN) {
        size = mpack_store_native_pair_i8(encoded, 0xd0, (int8_t)value);
    } else if (value >= INT16_MIN) {
        size = mpack_store_native_pair_i16(encoded, 0xd1, (int16_t)value);
    } else {
        size = mpack_store_native_pair_i32(encoded, 0xd2, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_i64(mpack_writer_t* writer, int64_t value) {

    // write any non-negative number as a uint
    if (value >= 0) {
        mpack_write_u64(writer, (uint64_t)value);
        return;
    }

    mpack_writer_track_element(writer);

    char encoded[9];
    size_t size;

    if (value >= -32) {
        size = mpack_store_native_i8(encoded, (int8_t)value);
    } else if (value >= INT8_MIN) {
        size = mpack_store_native_pair_i8(encoded, 0xd0, (int8_t)value);
    } else if (value >= INT16_MIN) {
        size = mpack_store_native_pair_i16(encoded, 0xd1, (int16_t)value);
    } else if (value >= INT32_MIN) {
        size = mpack_store_native_pair_i32(encoded, 0xd2, (int32_t)value);
    } else {
        size = mpack_store_native_pair_i64(encoded, 0xd3, value);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_bool(mpack_writer_t* writer, bool value) {
    mpack_writer_track_element(writer);
    mpack_write_native_u8(writer, (uint8_t)(0xc2 | (value ? 1 : 0)));
}

void mpack_write_true(mpack_writer_t* writer) {
    mpack_writer_track_element(writer);
    mpack_write_native_u8(writer, (uint8_t)0xc3);
}

void mpack_write_false(mpack_writer_t* writer) {
    mpack_writer_track_element(writer);
    mpack_write_native_u8(writer, (uint8_t)0xc2);
}

void mpack_write_nil(mpack_writer_t* writer) {
    mpack_writer_track_element(writer);
    mpack_write_native_u8(writer, 0xc0);
}

void mpack_write_float(mpack_writer_t* writer, float value) {
    mpack_writer_track_element(writer);
    char encoded[5];
    mpack_store_native_u8(encoded, 0xca);
    mpack_store_native_float(encoded + 1, value);
    mpack_write_native(writer, encoded, sizeof(encoded));
}

void mpack_write_double(mpack_writer_t* writer, double value) {
    mpack_writer_track_element(writer);
    char encoded[9];
    mpack_store_native_u8(encoded, 0xcb);
    mpack_store_native_double(encoded + 1, value);
    mpack_write_native(writer, encoded, sizeof(encoded));
}

#if MPACK_WRITE_TRACKING
void mpack_finish_array(mpack_writer_t* writer) {
    MPACK_WRITER_TRACK(writer, mpack_track_pop(&writer->track, mpack_type_array));
}

void mpack_finish_map(mpack_writer_t* writer) {
    MPACK_WRITER_TRACK(writer, mpack_track_pop(&writer->track, mpack_type_map));
}

void mpack_finish_str(mpack_writer_t* writer) {
    MPACK_WRITER_TRACK(writer, mpack_track_pop(&writer->track, mpack_type_str));
}

void mpack_finish_bin(mpack_writer_t* writer) {
    MPACK_WRITER_TRACK(writer, mpack_track_pop(&writer->track, mpack_type_bin));
}

void mpack_finish_ext(mpack_writer_t* writer) {
    MPACK_WRITER_TRACK(writer, mpack_track_pop(&writer->track, mpack_type_ext));
}

void mpack_finish_type(mpack_writer_t* writer, mpack_type_t type) {
    MPACK_WRITER_TRACK(writer, mpack_track_pop(&writer->track, type));
}
#endif

void mpack_start_array(mpack_writer_t* writer, uint32_t count) {
    if (mpack_writer_error(writer) != mpack_ok)
        return;

    mpack_writer_track_element(writer);
    MPACK_WRITER_TRACK(writer, mpack_track_push(&writer->track, mpack_type_array, count));

    char encoded[5];
    size_t size;

    if (count <= 15) {
        size = mpack_store_native_u8(encoded, (uint8_t)(0x90 | count));
    } else if (count <= UINT16_MAX) {
        size = mpack_store_native_pair_u16(encoded, 0xdc, (uint16_t)count);
    } else {
        size = mpack_store_native_pair_u32(encoded, 0xdd, count);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_start_map(mpack_writer_t* writer, uint32_t count) {
    if (mpack_writer_error(writer) != mpack_ok)
        return;

    mpack_writer_track_element(writer);
    MPACK_WRITER_TRACK(writer, mpack_track_push(&writer->track, mpack_type_map, count));

    char encoded[5];
    size_t size;

    if (count <= 15) {
        size = mpack_store_native_u8(encoded, (uint8_t)(0x80 | count));
    } else if (count <= UINT16_MAX) {
        size = mpack_store_native_pair_u16(encoded, 0xde, (uint16_t)count);
    } else {
        size = mpack_store_native_pair_u32(encoded, 0xdf, count);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_start_str(mpack_writer_t* writer, uint32_t count) {
    if (mpack_writer_error(writer) != mpack_ok)
        return;

    mpack_writer_track_element(writer);
    MPACK_WRITER_TRACK(writer, mpack_track_push(&writer->track, mpack_type_str, count));

    char encoded[5];
    size_t size;

    if (count <= 31) {
        size = mpack_store_native_u8(encoded, (uint8_t)(0xa0 | count));
    } else if (count <= UINT8_MAX) {
        // TODO: str8 had no counterpart in MessagePack 1.0; there was only
        // raw16 and raw32. This should not be used in compatibility mode.
        size = mpack_store_native_pair_u8(encoded, 0xd9, (uint8_t)count);
    } else if (count <= UINT16_MAX) {
        size = mpack_store_native_pair_u16(encoded, 0xda, (uint16_t)count);
    } else {
        size = mpack_store_native_pair_u32(encoded, 0xdb, count);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_start_bin(mpack_writer_t* writer, uint32_t count) {
    if (mpack_writer_error(writer) != mpack_ok)
        return;

    // TODO: use str (raw) in compatibility mode

    mpack_writer_track_element(writer);
    MPACK_WRITER_TRACK(writer, mpack_track_push(&writer->track, mpack_type_bin, count));

    char encoded[5];
    size_t size;

    if (count <= UINT8_MAX) {
        size = mpack_store_native_pair_u8(encoded, 0xc4, (uint8_t)count);
    } else if (count <= UINT16_MAX) {
        size = mpack_store_native_pair_u16(encoded, 0xc5, (uint16_t)count);
    } else {
        size = mpack_store_native_pair_u32(encoded, 0xc6, count);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_start_ext(mpack_writer_t* writer, int8_t exttype, uint32_t count) {
    if (mpack_writer_error(writer) != mpack_ok)
        return;

    // TODO: fail if compatibility mode

    mpack_writer_track_element(writer);
    MPACK_WRITER_TRACK(writer, mpack_track_push(&writer->track, mpack_type_ext, count));

    char encoded[6];
    size_t size = 0;

    if (count == 1) {
        size = mpack_store_native_pair_i8(encoded, 0xd4, exttype);
    } else if (count == 2) {
        size = mpack_store_native_pair_i8(encoded, 0xd5, exttype);
    } else if (count == 4) {
        size = mpack_store_native_pair_i8(encoded, 0xd6, exttype);
    } else if (count == 8) {
        size = mpack_store_native_pair_i8(encoded, 0xd7, exttype);
    } else if (count == 16) {
        size = mpack_store_native_pair_i8(encoded, 0xd8, exttype);
    } else if (count <= UINT8_MAX) {
        size += mpack_store_native_u8(encoded + size, 0xc7);
        size += mpack_store_native_u8(encoded + size, (uint8_t)count);
        size += mpack_store_native_i8(encoded + size, exttype);
    } else if (count <= UINT16_MAX) {
        size += mpack_store_native_u8(encoded + size, 0xc8);
        size += mpack_store_native_u16(encoded + size, (uint16_t)count);
        size += mpack_store_native_i8(encoded + size, exttype);
    } else {
        size += mpack_store_native_u8(encoded + size, 0xc9);
        size += mpack_store_native_u32(encoded + size, count);
        size += mpack_store_native_i8(encoded + size, exttype);
    }

    mpack_write_native(writer, encoded, size);
}

void mpack_write_str(mpack_writer_t* writer, const char* data, uint32_t count) {
    mpack_start_str(writer, count);
    mpack_write_bytes(writer, data, count);
    mpack_finish_str(writer);
}

void mpack_write_bin(mpack_writer_t* writer, const char* data, uint32_t count) {
    mpack_start_bin(writer, count);
    mpack_write_bytes(writer, data, count);
    mpack_finish_bin(writer);
}

void mpack_write_ext(mpack_writer_t* writer, int8_t exttype, const char* data, uint32_t count) {
    mpack_start_ext(writer, exttype, count);
    mpack_write_bytes(writer, data, count);
    mpack_finish_ext(writer);
}

void mpack_write_bytes(mpack_writer_t* writer, const char* data, size_t count) {
    MPACK_WRITER_TRACK(writer, mpack_track_bytes(&writer->track, false, count));
    if (mpack_writer_error(writer) != mpack_ok)
        return;
    mpack_write_native(writer, data, count);
}

void mpack_write_cstr(mpack_writer_t* writer, const char* str) {
    size_t len = mpack_strlen(str);
    if (len > UINT32_MAX)
        mpack_writer_flag_error(writer, mpack_error_invalid);
    mpack_write_str(writer, str, (uint32_t)len);
}

#endif

