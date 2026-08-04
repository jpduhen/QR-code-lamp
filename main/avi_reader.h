#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    long movi_start;
    long movi_end;
    long cursor;
    long range_end[4];
    int range_depth;
    uint32_t frame_interval_us;
    uint32_t width;
    uint32_t height;
    char compressor[5];
} avi_reader_t;

bool avi_reader_open(FILE *file, avi_reader_t *reader);
bool avi_reader_next_video_packet(avi_reader_t *reader, uint8_t *buffer,
                                  size_t capacity, size_t *packet_size);
int avi_reader_progress_width(const avi_reader_t *reader, int max_width);

