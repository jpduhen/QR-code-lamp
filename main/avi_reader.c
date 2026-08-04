#include "avi_reader.h"

#include <string.h>

#define AVI_DEFAULT_FRAME_INTERVAL_US 100000U

typedef struct {
    uint32_t frame_interval_us;
    uint32_t width;
    uint32_t height;
    char compressor[5];
    bool pending_video_strf;
    long movi_start;
    long movi_end;
    long file_end;
} avi_parse_state_t;

static bool read_exact(FILE *file, void *buffer, size_t length)
{
    return fread(buffer, 1, length, file) == length;
}

static uint32_t le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool read_le32(FILE *file, uint32_t *value)
{
    uint8_t bytes[4];
    if (!read_exact(file, bytes, sizeof(bytes))) {
        return false;
    }
    *value = le32(bytes);
    return true;
}

static bool chunk_id_is(const uint8_t *id, const char *text)
{
    return memcmp(id, text, 4) == 0;
}

static long aligned_chunk_end(long payload_start, uint32_t size)
{
    long next = payload_start + (long)size;
    if ((size & 1U) != 0) {
        ++next;
    }
    return next;
}

static void parse_avih(FILE *file, uint32_t size, avi_parse_state_t *state)
{
    uint8_t header[16];
    if (size < sizeof(header) || !read_exact(file, header, sizeof(header))) {
        return;
    }
    const uint32_t interval = le32(&header[0]);
    if (interval >= 20000U && interval <= 200000U) {
        state->frame_interval_us = interval;
    }
    const uint32_t width = le32(&header[8]);
    const uint32_t height = le32(&header[12]);
    if (width > 0 && height > 0) {
        state->width = width;
        state->height = height;
    }
}

static void parse_strh(FILE *file, uint32_t size, avi_parse_state_t *state)
{
    uint8_t header[8];
    state->pending_video_strf = false;
    if (size < sizeof(header) || !read_exact(file, header, sizeof(header))) {
        return;
    }
    if (chunk_id_is(header, "vids")) {
        state->pending_video_strf = true;
        memcpy(state->compressor, &header[4], 4);
        state->compressor[4] = '\0';
    }
}

static void parse_strf(FILE *file, uint32_t size, avi_parse_state_t *state)
{
    uint8_t header[20];
    if (!state->pending_video_strf || size < sizeof(header) ||
        !read_exact(file, header, sizeof(header))) {
        return;
    }
    const int32_t width = (int32_t)le32(&header[4]);
    const int32_t height = (int32_t)le32(&header[8]);
    if (width > 0 && height != 0) {
        state->width = (uint32_t)width;
        state->height = height < 0 ? (uint32_t)-height : (uint32_t)height;
    }
    memcpy(state->compressor, &header[16], 4);
    state->compressor[4] = '\0';
    state->pending_video_strf = false;
}

static void scan_avi_range(FILE *file, long start, long end, int depth,
                           avi_parse_state_t *state)
{
    if (depth > 4 || start < 0 || end > state->file_end || start >= end) {
        return;
    }
    long cursor = start;
    while (cursor + 8 <= end) {
        if (fseek(file, cursor, SEEK_SET) != 0) {
            return;
        }
        uint8_t id[4];
        uint32_t size = 0;
        if (!read_exact(file, id, sizeof(id)) || !read_le32(file, &size)) {
            return;
        }
        const long payload = cursor + 8;
        const long next = aligned_chunk_end(payload, size);
        if (next < payload || next > end + 1 || next > state->file_end + 1) {
            return;
        }
        if (chunk_id_is(id, "LIST")) {
            uint8_t type[4];
            if (size >= 4 && fseek(file, payload, SEEK_SET) == 0 &&
                read_exact(file, type, sizeof(type))) {
                if (chunk_id_is(type, "movi")) {
                    state->movi_start = payload + 4;
                    state->movi_end = payload + (long)size;
                } else {
                    scan_avi_range(file, payload + 4, payload + (long)size,
                                   depth + 1, state);
                }
            }
        } else if (chunk_id_is(id, "avih")) {
            if (fseek(file, payload, SEEK_SET) == 0) {
                parse_avih(file, size, state);
            }
        } else if (chunk_id_is(id, "strh")) {
            if (fseek(file, payload, SEEK_SET) == 0) {
                parse_strh(file, size, state);
            }
        } else if (chunk_id_is(id, "strf")) {
            if (fseek(file, payload, SEEK_SET) == 0) {
                parse_strf(file, size, state);
            }
        }
        cursor = next;
    }
}

bool avi_reader_open(FILE *file, avi_reader_t *reader)
{
    memset(reader, 0, sizeof(*reader));
    if (fseek(file, 0, SEEK_END) != 0) {
        return false;
    }
    const long file_end = ftell(file);
    if (file_end < 12 || fseek(file, 0, SEEK_SET) != 0) {
        return false;
    }
    uint8_t riff[12];
    if (!read_exact(file, riff, sizeof(riff)) ||
        !chunk_id_is(riff, "RIFF") || !chunk_id_is(&riff[8], "AVI ")) {
        return false;
    }

    avi_parse_state_t state = {
        .frame_interval_us = AVI_DEFAULT_FRAME_INTERVAL_US,
        .file_end = file_end,
    };
    scan_avi_range(file, 12, file_end, 0, &state);
    if (state.movi_start <= 0 || state.movi_end <= state.movi_start ||
        state.width == 0 || state.height == 0 || state.compressor[0] == '\0') {
        return false;
    }
    reader->file = file;
    reader->movi_start = state.movi_start;
    reader->movi_end = state.movi_end > file_end ? file_end : state.movi_end;
    reader->cursor = reader->movi_start;
    reader->frame_interval_us = state.frame_interval_us;
    reader->width = state.width;
    reader->height = state.height;
    memcpy(reader->compressor, state.compressor, sizeof(reader->compressor));
    return true;
}

bool avi_reader_next_video_packet(avi_reader_t *reader, uint8_t *buffer,
                                  size_t capacity, size_t *packet_size)
{
    while (true) {
        long current_end = reader->range_depth > 0
            ? reader->range_end[reader->range_depth - 1] : reader->movi_end;
        while (reader->cursor >= current_end) {
            if (reader->range_depth == 0) {
                return false;
            }
            --reader->range_depth;
            current_end = reader->range_depth > 0
                ? reader->range_end[reader->range_depth - 1] : reader->movi_end;
        }
        if (reader->cursor + 8 > current_end ||
            fseek(reader->file, reader->cursor, SEEK_SET) != 0) {
            return false;
        }
        uint8_t id[4];
        uint32_t size = 0;
        if (!read_exact(reader->file, id, sizeof(id)) ||
            !read_le32(reader->file, &size)) {
            return false;
        }
        const long payload = reader->cursor + 8;
        const long next = aligned_chunk_end(payload, size);
        if (next < payload || next > current_end + 1) {
            return false;
        }
        reader->cursor = next;
        if (chunk_id_is(id, "LIST")) {
            uint8_t type[4];
            if (size >= 4 && fseek(reader->file, payload, SEEK_SET) == 0 &&
                read_exact(reader->file, type, sizeof(type)) &&
                chunk_id_is(type, "rec ") && reader->range_depth < 4) {
                reader->range_end[reader->range_depth++] = next;
                reader->cursor = payload + 4;
            }
            continue;
        }
        const bool video_chunk = id[0] >= '0' && id[0] <= '9' &&
                                 id[1] >= '0' && id[1] <= '9' &&
                                 id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
        if (!video_chunk) {
            continue;
        }
        if (size > capacity || fseek(reader->file, payload, SEEK_SET) != 0 ||
            !read_exact(reader->file, buffer, size)) {
            return false;
        }
        *packet_size = size;
        return true;
    }
}

int avi_reader_progress_width(const avi_reader_t *reader, int max_width)
{
    if (reader->movi_end <= reader->movi_start || max_width <= 0) {
        return 0;
    }
    const long consumed = reader->cursor > reader->movi_start
        ? reader->cursor - reader->movi_start : 0;
    const long total = reader->movi_end - reader->movi_start;
    int width = (int)(((int64_t)consumed * max_width) / total);
    if (width < 0) {
        width = 0;
    } else if (width > max_width) {
        width = max_width;
    }
    return width;
}
