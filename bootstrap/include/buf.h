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
    char    data[]; /**< Flexible array member containing the data */
} ncc_buf_t;

/**
 * @brief Buffer API - module struct for clean function access.
 */
struct buf_api {
    /** Allocate a zero-initialized buffer */
    ncc_buf_t *(*alloc)(int64_t len);

    /** Concatenate data onto a buffer (frees original, returns new) */
    ncc_buf_t *(*concat)(ncc_buf_t *buf, char *data, int64_t len);

    /** Concatenate a null-terminated string onto a buffer */
    ncc_buf_t *(*concat_str)(ncc_buf_t *buf, char *str);

    /** Read entire file into buffer (closes handle) */
    ncc_buf_t *(*read_file)(FILE *f);

    /** Read entire file into buffer by filename */
    ncc_buf_t *(*read_file_by_name)(char *fname);

    /** Read from stream until EOF */
    ncc_buf_t *(*read_stream)(FILE *f);

    /** Write buffer contents to file */
    bool (*write)(ncc_buf_t *buf, FILE *f);

    /** Create buffer from null-terminated string */
    ncc_buf_t *(*from_str)(const char *str);
};

/** @brief Buffer module instance */
extern const struct buf_api buf;

/*
 * Direct function declarations (for internal use or when avoiding indirection)
 */
[[nodiscard]] ncc_buf_t *ncc_buf_alloc(int64_t len);
[[nodiscard]] ncc_buf_t *ncc_buf_concat(ncc_buf_t *buf, char *data, int64_t len);
[[nodiscard]] ncc_buf_t *ncc_buf_concat_str(ncc_buf_t *buf, char *str);
[[nodiscard]] ncc_buf_t *ncc_buf_read_file(FILE *f);
[[nodiscard]] ncc_buf_t *ncc_buf_read_file_by_name(char *fname);
[[nodiscard]] ncc_buf_t *ncc_buf_read_stream(FILE *f);
bool buf_write(ncc_buf_t *buf, FILE *f);
[[nodiscard]] ncc_buf_t *ncc_buf_from_str(const char *str);
