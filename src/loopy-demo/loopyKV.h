/* loopyKV - Demo Key-Value State Machine
 *
 * A simple key-value store demonstrating a practical Raft state machine.
 * This shows how to build application logic on top of the loopy-demo platform.
 *
 * Features:
 *   - In-memory key-value storage with string keys and values
 *   - Full snapshot/restore for log compaction
 *   - Command protocol: SET, GET, DEL, INCR, APPEND
 *   - Live statistics and metrics
 *
 * Command Format (binary):
 *   [1 byte cmd] [2 byte key_len] [key] [2 byte val_len] [value]
 *
 * Response Format:
 *   [1 byte status] [2 byte len] [data]
 *
 * This is part of loopy-demo, demonstrating kraft usage.
 */

#ifndef LOOPY_KV_H
#define LOOPY_KV_H

#include "loopyServer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * KV store handle.
 */
typedef struct loopyKV loopyKV;

/**
 * Command types.
 */
typedef enum loopyKVCmd {
    LOOPY_KV_CMD_SET = 0x01,  /* SET key value */
    LOOPY_KV_CMD_GET = 0x02,  /* GET key */
    LOOPY_KV_CMD_DEL = 0x03,  /* DEL key */
    LOOPY_KV_CMD_INCR = 0x04, /* INCR key [amount] - increment numeric value */
    LOOPY_KV_CMD_APPEND = 0x05, /* APPEND key value - append to existing */
    LOOPY_KV_CMD_KEYS = 0x06,   /* KEYS [pattern] - list keys */
    LOOPY_KV_CMD_STAT = 0x07    /* STAT - get stats */
} loopyKVCmd;

/**
 * Response status codes.
 */
typedef enum loopyKVStatus {
    LOOPY_KV_OK = 0x00,           /* Success */
    LOOPY_KV_ERR_NOTFOUND = 0x01, /* Key not found */
    LOOPY_KV_ERR_INVALID = 0x02,  /* Invalid command */
    LOOPY_KV_ERR_TYPE = 0x03, /* Type mismatch (e.g., INCR on non-numeric) */
    LOOPY_KV_ERR_NOMEM = 0x04 /* Out of memory */
} loopyKVStatus;

/**
 * Statistics.
 */
typedef struct loopyKVStats {
    uint64_t keyCount;          /* Current number of keys */
    uint64_t memoryUsed;        /* Approximate memory usage */
    uint64_t setsTotal;         /* Total SET operations */
    uint64_t getsTotal;         /* Total GET operations */
    uint64_t delsTotal;         /* Total DEL operations */
    uint64_t hitsTotal;         /* GET operations that found the key */
    uint64_t missesTotal;       /* GET operations that didn't find the key */
    uint64_t snapshotsCreated;  /* Snapshots created */
    uint64_t snapshotsRestored; /* Snapshots restored */
    uint64_t lastAppliedIndex;  /* Last applied log index */
} loopyKVStats;

/**
 * Change notification callback.
 *
 * Called when a key is modified. Useful for live display.
 *
 * @param kv       KV store handle
 * @param cmd      Command that caused the change
 * @param key      Key that changed
 * @param keyLen   Key length
 * @param value    New value (NULL for DEL)
 * @param valueLen Value length
 * @param userData User context
 */
typedef void loopyKVChangeCallback(loopyKV *kv, loopyKVCmd cmd, const char *key,
                                   size_t keyLen, const char *value,
                                   size_t valueLen, void *userData);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a KV store.
 *
 * @return KV handle, or NULL on error
 */
loopyKV *loopyKVCreate(void);

/**
 * Destroy a KV store.
 *
 * @param kv KV handle (may be NULL)
 */
void loopyKVDestroy(loopyKV *kv);

/**
 * Get the state machine interface.
 *
 * Returns a loopyStateMachine that can be passed to
 * loopyServerSetStateMachine().
 *
 * @param kv KV handle
 * @return State machine interface (owned by kv, do not free)
 */
loopyStateMachine *loopyKVGetStateMachine(loopyKV *kv);

/**
 * Set change notification callback.
 *
 * @param kv       KV handle
 * @param cb       Callback function (NULL to disable)
 * @param userData User context
 */
void loopyKVSetChangeCallback(loopyKV *kv, loopyKVChangeCallback *cb,
                              void *userData);

/* ============================================================================
 * Direct API (for local reads - use commands for writes!)
 * ============================================================================
 */

/**
 * Get a value by key.
 *
 * This is a local read - for linearizable reads, use the command protocol
 * through loopyServerSubmit() or loopyServerReadLinearizable().
 *
 * @param kv       KV handle
 * @param key      Key to look up
 * @param keyLen   Key length
 * @param value    OUT: Value (caller must free)
 * @param valueLen OUT: Value length
 * @return true if found
 */
bool loopyKVGet(loopyKV *kv, const char *key, size_t keyLen, char **value,
                size_t *valueLen);

/**
 * Get key count.
 */
size_t loopyKVCount(const loopyKV *kv);

/**
 * Get statistics.
 */
void loopyKVGetStats(const loopyKV *kv, loopyKVStats *stats);

/* ============================================================================
 * Command Encoding/Decoding
 *
 * Helper functions for creating commands to submit via loopyServerSubmit()
 * ============================================================================
 */

/**
 * Encode a SET command.
 *
 * @param key      Key
 * @param keyLen   Key length
 * @param value    Value
 * @param valueLen Value length
 * @param data     OUT: Encoded command (caller must free)
 * @param dataLen  OUT: Command length
 * @return true on success
 */
bool loopyKVEncodeSet(const char *key, size_t keyLen, const char *value,
                      size_t valueLen, void **data, size_t *dataLen);

/**
 * Encode a GET command.
 *
 * Note: GET through Raft ensures linearizability but has overhead.
 * Use loopyKVGet() for local reads if stale data is acceptable.
 *
 * @param key     Key
 * @param keyLen  Key length
 * @param data    OUT: Encoded command (caller must free)
 * @param dataLen OUT: Command length
 * @return true on success
 */
bool loopyKVEncodeGet(const char *key, size_t keyLen, void **data,
                      size_t *dataLen);

/**
 * Encode a DEL command.
 *
 * @param key     Key
 * @param keyLen  Key length
 * @param data    OUT: Encoded command (caller must free)
 * @param dataLen OUT: Command length
 * @return true on success
 */
bool loopyKVEncodeDel(const char *key, size_t keyLen, void **data,
                      size_t *dataLen);

/**
 * Encode an INCR command.
 *
 * @param key     Key
 * @param keyLen  Key length
 * @param amount  Amount to increment (can be negative)
 * @param data    OUT: Encoded command (caller must free)
 * @param dataLen OUT: Command length
 * @return true on success
 */
bool loopyKVEncodeIncr(const char *key, size_t keyLen, int64_t amount,
                       void **data, size_t *dataLen);

/**
 * Decode a response.
 *
 * @param data      Response data
 * @param dataLen   Response length
 * @param status    OUT: Status code
 * @param value     OUT: Value (caller must free, may be NULL)
 * @param valueLen  OUT: Value length
 * @return true on success
 */
bool loopyKVDecodeResponse(const void *data, size_t dataLen,
                           loopyKVStatus *status, char **value,
                           size_t *valueLen);

/* ============================================================================
 * Text Command Parsing (for CLI)
 * ============================================================================
 */

/**
 * Parse a text command.
 *
 * Parses standard text commands:
 *   SET key value
 *   GET key
 *   DEL key
 *   INCR key [amount]
 *   KEYS [pattern]
 *   STAT
 *
 * @param text     Command text
 * @param textLen  Text length
 * @param data     OUT: Encoded command (caller must free)
 * @param dataLen  OUT: Command length
 * @return true on success
 */
bool loopyKVParseCommand(const char *text, size_t textLen, void **data,
                         size_t *dataLen);

/**
 * Format a response as text.
 *
 * @param data     Response data
 * @param dataLen  Response length
 * @param text     OUT: Formatted text (caller must free)
 * @return true on success
 */
bool loopyKVFormatResponse(const void *data, size_t dataLen, char **text);

/* ============================================================================
 * Debugging
 * ============================================================================
 */

/**
 * Dump all keys to stdout.
 *
 * @param kv KV handle
 */
void loopyKVDump(const loopyKV *kv);

/**
 * Get command name string.
 */
const char *loopyKVCmdName(loopyKVCmd cmd);

/**
 * Get status name string.
 */
const char *loopyKVStatusName(loopyKVStatus status);

#endif /* LOOPY_KV_H */
