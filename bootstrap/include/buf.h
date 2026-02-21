/**
 * @file buf.h
 * @brief Simple buffer type for file I/O and string manipulation.
 *
 * Provides a length-prefixed buffer with flexible array member for
 * efficient memory management and file operations.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/** @brief Default chunk size for buffer I/O operations */
#define NCC_BUF_CHUNK_SIZE 4096

/**
 * @brief Length-prefixed buffer with flexible array data.
 *
 * The data immediately follows the length field in memory,
 * allowing single-allocation buffers of any size.
 */
typedef struct {
    int64_t len;    /**< Length of data in bytes */
    int64_t cap;    /**< Allocated capacity of data[] (0 = exact allocation, len==cap) */
    char    data[]; /**< Flexible array member containing the data */
} ncc_buf_t;

[[nodiscard]] ncc_buf_t *ncc_buf_alloc(int64_t len);
[[nodiscard]] ncc_buf_t *ncc_buf_concat(ncc_buf_t *buf, char *data, int64_t len);
[[nodiscard]] ncc_buf_t *ncc_buf_concat_str(ncc_buf_t *buf, char *str);
[[nodiscard]] ncc_buf_t *ncc_buf_read_file(FILE *f);
[[nodiscard]] ncc_buf_t *ncc_buf_read_file_by_name(char *fname);
[[nodiscard]] ncc_buf_t *ncc_buf_read_stream(FILE *f);
[[nodiscard]] ncc_buf_t *ncc_buf_from_str(const char *str);
