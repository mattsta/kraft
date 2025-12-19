# Custom Protocols Guide

This guide explains how to implement custom wire format encodings for Kraft's pluggable protocol system.

## Overview

Kraft separates message handling from serialization, allowing you to:

- Use different wire formats (Binary, Protobuf, MessagePack, JSON)
- Optimize for your specific use case (speed vs. debugging)
- Integrate with existing systems using their native protocols
- Support cross-language clients

---

## Protocol Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        kraftProtocol                             │
│                                                                  │
│  ┌─────────────────┐    ┌──────────────────────────────────┐   │
│  │ kraftProtocolOps│───►│ Your encode/decode implementation │   │
│  └─────────────────┘    └──────────────────────────────────┘   │
│                                                                  │
│  Message Types:                                                  │
│  • AppendEntries / Response                                      │
│  • RequestVote / Response                                        │
│  • InstallSnapshot / Response                                    │
│  • ClientRequest / Response                                      │
│  • ReadIndex / Response                                          │
│  • TimeoutNow                                                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## The Protocol Interface

### Core Operations

```c
typedef struct kraftProtocolOps {
    // === Encoding ===

    // Leader → Follower: Log replication
    bool (*encodeAppendEntries)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftAppendEntriesArgs *args
    );

    bool (*encodeAppendEntriesResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftAppendEntriesResponse *resp
    );

    // Candidate → All: Vote requests
    bool (*encodeRequestVote)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftRequestVoteArgs *args
    );

    bool (*encodeRequestVoteResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftRequestVoteResponse *resp
    );

    // Leader → Follower: Snapshot transfer
    bool (*encodeInstallSnapshot)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftInstallSnapshotArgs *args
    );

    bool (*encodeInstallSnapshotResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftInstallSnapshotResponse *resp
    );

    // Client → Server: Commands
    bool (*encodeClientRequest)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftClientRequestArgs *args
    );

    bool (*encodeClientResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftClientResponseArgs *resp
    );

    // Client → Leader: Linearizable reads
    bool (*encodeReadIndex)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftReadIndexArgs *args
    );

    bool (*encodeReadIndexResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftReadIndexResponse *resp
    );

    // Leader → Target: Leadership transfer
    bool (*encodeTimeoutNow)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftTimeoutNowArgs *args
    );

    // === Decoding ===

    // Unified decoder - determines message type and decodes
    kraftDecodeStatus (*decode)(
        kraftProtocol *proto,
        const uint8_t *data,
        size_t len,
        kraftMessage *out,      // Decoded message
        size_t *consumed        // Bytes consumed
    );

    // === Lifecycle ===

    void (*destroy)(kraftProtocol *proto);

} kraftProtocolOps;
```

### Message Structures

```c
// AppendEntries RPC
typedef struct {
    uint64_t term;              // Leader's term
    uint64_t leaderId;          // For redirects
    uint64_t prevLogIndex;      // Index of preceding entry
    uint64_t prevLogTerm;       // Term of preceding entry
    uint64_t leaderCommit;      // Leader's commit index

    // Log entries to replicate
    kraftLogEntry *entries;
    uint32_t entriesCount;
} kraftAppendEntriesArgs;

typedef struct {
    uint64_t term;              // Current term for leader update
    bool success;               // True if matched prevLogIndex/Term
    uint64_t matchIndex;        // Follower's match index (optimization)
    uint64_t conflictIndex;     // Conflict optimization
    uint64_t conflictTerm;      // Term of conflicting entry
} kraftAppendEntriesResponse;

// RequestVote RPC
typedef struct {
    uint64_t term;              // Candidate's term
    uint64_t candidateId;       // Candidate ID
    uint64_t lastLogIndex;      // Index of candidate's last log
    uint64_t lastLogTerm;       // Term of candidate's last log
    bool preVote;               // Is this a pre-vote?
} kraftRequestVoteArgs;

typedef struct {
    uint64_t term;              // Current term
    bool voteGranted;           // True if vote granted
    bool preVote;               // Is this a pre-vote response?
} kraftRequestVoteResponse;

// Log entries
typedef struct {
    uint64_t index;
    uint64_t term;
    uint16_t command;           // Command type
    uint8_t *data;
    uint32_t dataLen;
} kraftLogEntry;
```

---

## Implementing a Protocol

### Step 1: Create Protocol State

```c
typedef struct {
    kraftProtocolOps ops;
    void *userData;

    // Protocol-specific state
    // For example, for protobuf:
    // ProtobufArena *arena;
} myProtocol;
```

### Step 2: Implement Encode Functions

Example: Simple binary encoding

```c
static bool myEncodeAppendEntries(
    kraftProtocol *proto,
    kraftEncodeBuffer *buf,
    const kraftAppendEntriesArgs *args
) {
    // Message type tag
    uint8_t msgType = KRAFT_MSG_APPEND_ENTRIES;
    kraftEncodeAppend(buf, &msgType, 1);

    // Fixed fields
    kraftEncodeU64(buf, args->term);
    kraftEncodeU64(buf, args->leaderId);
    kraftEncodeU64(buf, args->prevLogIndex);
    kraftEncodeU64(buf, args->prevLogTerm);
    kraftEncodeU64(buf, args->leaderCommit);

    // Entry count
    kraftEncodeU32(buf, args->entriesCount);

    // Entries
    for (uint32_t i = 0; i < args->entriesCount; i++) {
        kraftLogEntry *e = &args->entries[i];
        kraftEncodeU64(buf, e->index);
        kraftEncodeU64(buf, e->term);
        kraftEncodeU16(buf, e->command);
        kraftEncodeU32(buf, e->dataLen);
        kraftEncodeAppend(buf, e->data, e->dataLen);
    }

    return true;
}

static bool myEncodeRequestVote(
    kraftProtocol *proto,
    kraftEncodeBuffer *buf,
    const kraftRequestVoteArgs *args
) {
    uint8_t msgType = args->preVote ?
        KRAFT_MSG_PRE_VOTE : KRAFT_MSG_REQUEST_VOTE;
    kraftEncodeAppend(buf, &msgType, 1);

    kraftEncodeU64(buf, args->term);
    kraftEncodeU64(buf, args->candidateId);
    kraftEncodeU64(buf, args->lastLogIndex);
    kraftEncodeU64(buf, args->lastLogTerm);

    return true;
}
```

### Step 3: Implement the Decoder

```c
static kraftDecodeStatus myDecode(
    kraftProtocol *proto,
    const uint8_t *data,
    size_t len,
    kraftMessage *out,
    size_t *consumed
) {
    if (len < 1) {
        return KRAFT_DECODE_NEED_MORE;
    }

    uint8_t msgType = data[0];
    const uint8_t *ptr = data + 1;
    size_t remaining = len - 1;

    out->type = msgType;

    switch (msgType) {
        case KRAFT_MSG_APPEND_ENTRIES: {
            // Need at least: 5 × u64 + 1 × u32 = 44 bytes
            if (remaining < 44) return KRAFT_DECODE_NEED_MORE;

            kraftAppendEntriesArgs *args = &out->appendEntries;
            args->term = decodeU64(&ptr);
            args->leaderId = decodeU64(&ptr);
            args->prevLogIndex = decodeU64(&ptr);
            args->prevLogTerm = decodeU64(&ptr);
            args->leaderCommit = decodeU64(&ptr);
            args->entriesCount = decodeU32(&ptr);

            remaining -= 44;

            // Decode entries
            for (uint32_t i = 0; i < args->entriesCount; i++) {
                if (remaining < 22) return KRAFT_DECODE_NEED_MORE;

                kraftLogEntry *e = &args->entries[i];
                e->index = decodeU64(&ptr);
                e->term = decodeU64(&ptr);
                e->command = decodeU16(&ptr);
                e->dataLen = decodeU32(&ptr);

                remaining -= 22;

                if (remaining < e->dataLen) return KRAFT_DECODE_NEED_MORE;

                e->data = (uint8_t *)ptr;  // Zero-copy reference
                ptr += e->dataLen;
                remaining -= e->dataLen;
            }

            *consumed = ptr - data;
            return KRAFT_DECODE_OK;
        }

        case KRAFT_MSG_APPEND_ENTRIES_RESPONSE: {
            if (remaining < 26) return KRAFT_DECODE_NEED_MORE;

            kraftAppendEntriesResponse *resp = &out->appendEntriesResponse;
            resp->term = decodeU64(&ptr);
            resp->success = *ptr++;
            resp->matchIndex = decodeU64(&ptr);
            resp->conflictIndex = decodeU64(&ptr);
            resp->conflictTerm = decodeU64(&ptr);

            *consumed = ptr - data;
            return KRAFT_DECODE_OK;
        }

        case KRAFT_MSG_REQUEST_VOTE:
        case KRAFT_MSG_PRE_VOTE: {
            if (remaining < 32) return KRAFT_DECODE_NEED_MORE;

            kraftRequestVoteArgs *args = &out->requestVote;
            args->term = decodeU64(&ptr);
            args->candidateId = decodeU64(&ptr);
            args->lastLogIndex = decodeU64(&ptr);
            args->lastLogTerm = decodeU64(&ptr);
            args->preVote = (msgType == KRAFT_MSG_PRE_VOTE);

            *consumed = ptr - data;
            return KRAFT_DECODE_OK;
        }

        // ... implement remaining message types

        default:
            return KRAFT_DECODE_ERROR;
    }
}
```

### Step 4: Create the Protocol Instance

```c
kraftProtocolOps myProtocolOps = {
    .encodeAppendEntries = myEncodeAppendEntries,
    .encodeAppendEntriesResponse = myEncodeAppendEntriesResponse,
    .encodeRequestVote = myEncodeRequestVote,
    .encodeRequestVoteResponse = myEncodeRequestVoteResponse,
    .encodeInstallSnapshot = myEncodeInstallSnapshot,
    .encodeInstallSnapshotResponse = myEncodeInstallSnapshotResponse,
    .encodeClientRequest = myEncodeClientRequest,
    .encodeClientResponse = myEncodeClientResponse,
    .encodeReadIndex = myEncodeReadIndex,
    .encodeReadIndexResponse = myEncodeReadIndexResponse,
    .encodeTimeoutNow = myEncodeTimeoutNow,
    .decode = myDecode,
    .destroy = myDestroy,
};

kraftProtocol *myProtocolCreate(void) {
    myProtocol *proto = calloc(1, sizeof(myProtocol));
    if (!proto) return NULL;

    proto->ops = myProtocolOps;

    return (kraftProtocol *)proto;
}
```

---

## Built-in Protocol: Binary

The default binary protocol optimizes for:

- Zero-copy decoding where possible
- Minimal allocations
- Fast encoding with pre-sized buffers

```c
// Get the built-in binary protocol
kraftProtocol *proto = kraftProtocolCreate(
    kraftProtocolGetBinaryOps(),
    NULL  // No user data needed
);

// Wire format:
// [1 byte: msg type][variable: payload]
//
// Integers: little-endian
// Strings: [4 byte length][data]
// Arrays: [4 byte count][elements...]
```

---

## Example: JSON Protocol

For debugging or cross-language compatibility:

```c
#include <cJSON.h>

static bool jsonEncodeAppendEntries(
    kraftProtocol *proto,
    kraftEncodeBuffer *buf,
    const kraftAppendEntriesArgs *args
) {
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "type", "AppendEntries");
    cJSON_AddNumberToObject(root, "term", args->term);
    cJSON_AddNumberToObject(root, "leaderId", args->leaderId);
    cJSON_AddNumberToObject(root, "prevLogIndex", args->prevLogIndex);
    cJSON_AddNumberToObject(root, "prevLogTerm", args->prevLogTerm);
    cJSON_AddNumberToObject(root, "leaderCommit", args->leaderCommit);

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    for (uint32_t i = 0; i < args->entriesCount; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "index", args->entries[i].index);
        cJSON_AddNumberToObject(entry, "term", args->entries[i].term);
        cJSON_AddNumberToObject(entry, "command", args->entries[i].command);

        // Base64 encode binary data
        char *b64 = base64Encode(args->entries[i].data,
                                 args->entries[i].dataLen);
        cJSON_AddStringToObject(entry, "data", b64);
        free(b64);

        cJSON_AddItemToArray(entries, entry);
    }

    char *json = cJSON_PrintUnformatted(root);
    uint32_t jsonLen = strlen(json);

    // Length-prefixed JSON
    kraftEncodeU32(buf, jsonLen);
    kraftEncodeAppend(buf, (uint8_t *)json, jsonLen);

    free(json);
    cJSON_Delete(root);

    return true;
}

static kraftDecodeStatus jsonDecode(
    kraftProtocol *proto,
    const uint8_t *data,
    size_t len,
    kraftMessage *out,
    size_t *consumed
) {
    if (len < 4) return KRAFT_DECODE_NEED_MORE;

    uint32_t jsonLen = *(uint32_t *)data;
    if (len < 4 + jsonLen) return KRAFT_DECODE_NEED_MORE;

    cJSON *root = cJSON_ParseWithLength((char *)data + 4, jsonLen);
    if (!root) return KRAFT_DECODE_ERROR;

    const char *type = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "type"));

    if (strcmp(type, "AppendEntries") == 0) {
        out->type = KRAFT_MSG_APPEND_ENTRIES;
        kraftAppendEntriesArgs *args = &out->appendEntries;

        args->term = cJSON_GetNumberValue(
            cJSON_GetObjectItem(root, "term"));
        args->leaderId = cJSON_GetNumberValue(
            cJSON_GetObjectItem(root, "leaderId"));
        // ... parse remaining fields
    }
    // ... handle other message types

    cJSON_Delete(root);
    *consumed = 4 + jsonLen;
    return KRAFT_DECODE_OK;
}
```

---

## Example: MessagePack Protocol

Compact binary with schema flexibility:

```c
#include <msgpack.h>

static bool msgpackEncodeAppendEntries(
    kraftProtocol *proto,
    kraftEncodeBuffer *buf,
    const kraftAppendEntriesArgs *args
) {
    msgpack_sbuffer sbuf;
    msgpack_packer pk;

    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    // Map with 7 fields
    msgpack_pack_map(&pk, 7);

    // type
    msgpack_pack_str(&pk, 4);
    msgpack_pack_str_body(&pk, "type", 4);
    msgpack_pack_uint8(&pk, KRAFT_MSG_APPEND_ENTRIES);

    // term
    msgpack_pack_str(&pk, 4);
    msgpack_pack_str_body(&pk, "term", 4);
    msgpack_pack_uint64(&pk, args->term);

    // leaderId
    msgpack_pack_str(&pk, 8);
    msgpack_pack_str_body(&pk, "leaderId", 8);
    msgpack_pack_uint64(&pk, args->leaderId);

    // ... encode remaining fields

    // entries array
    msgpack_pack_str(&pk, 7);
    msgpack_pack_str_body(&pk, "entries", 7);
    msgpack_pack_array(&pk, args->entriesCount);

    for (uint32_t i = 0; i < args->entriesCount; i++) {
        msgpack_pack_map(&pk, 4);
        // ... pack each entry
    }

    // Copy to output buffer
    kraftEncodeAppend(buf, (uint8_t *)sbuf.data, sbuf.size);

    msgpack_sbuffer_destroy(&sbuf);
    return true;
}
```

---

## Encode Buffer API

```c
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} kraftEncodeBuffer;

// Initialize with pre-allocated buffer
void kraftEncodeBufferInit(kraftEncodeBuffer *buf, uint8_t *data, size_t cap);

// Initialize with dynamic allocation
void kraftEncodeBufferInitDynamic(kraftEncodeBuffer *buf, size_t initialCap);

// Append raw bytes
bool kraftEncodeAppend(kraftEncodeBuffer *buf, const void *data, size_t len);

// Encode primitives (little-endian)
bool kraftEncodeU8(kraftEncodeBuffer *buf, uint8_t val);
bool kraftEncodeU16(kraftEncodeBuffer *buf, uint16_t val);
bool kraftEncodeU32(kraftEncodeBuffer *buf, uint32_t val);
bool kraftEncodeU64(kraftEncodeBuffer *buf, uint64_t val);

// Encode with length prefix
bool kraftEncodeBytes(kraftEncodeBuffer *buf, const void *data, size_t len);
bool kraftEncodeString(kraftEncodeBuffer *buf, const char *str);

// Reset for reuse
void kraftEncodeBufferReset(kraftEncodeBuffer *buf);

// Cleanup
void kraftEncodeBufferFree(kraftEncodeBuffer *buf);
```

---

## Decode Status

```c
typedef enum {
    KRAFT_DECODE_OK,         // Message decoded successfully
    KRAFT_DECODE_NEED_MORE,  // Incomplete message, need more bytes
    KRAFT_DECODE_ERROR,      // Invalid message, unrecoverable
} kraftDecodeStatus;
```

### Handling Partial Messages

```c
typedef struct {
    uint8_t buffer[65536];
    size_t bufferLen;
} ConnectionState;

void onDataReceived(ConnectionState *conn, const uint8_t *data, size_t len) {
    // Append to buffer
    memcpy(conn->buffer + conn->bufferLen, data, len);
    conn->bufferLen += len;

    // Try to decode
    while (conn->bufferLen > 0) {
        kraftMessage msg;
        size_t consumed;

        kraftDecodeStatus status = proto->ops.decode(
            proto, conn->buffer, conn->bufferLen, &msg, &consumed);

        if (status == KRAFT_DECODE_NEED_MORE) {
            break;  // Wait for more data
        }

        if (status == KRAFT_DECODE_ERROR) {
            // Protocol error - close connection
            closeConnection(conn);
            return;
        }

        // Process message
        handleMessage(&msg);

        // Remove consumed bytes
        memmove(conn->buffer, conn->buffer + consumed,
                conn->bufferLen - consumed);
        conn->bufferLen -= consumed;
    }
}
```

---

## Protocol Selection

### Runtime Selection

```c
kraftProtocol *createProtocol(const char *name) {
    if (strcmp(name, "binary") == 0) {
        return kraftProtocolCreate(kraftProtocolGetBinaryOps(), NULL);
    }
    if (strcmp(name, "json") == 0) {
        return jsonProtocolCreate();
    }
    if (strcmp(name, "msgpack") == 0) {
        return msgpackProtocolCreate();
    }
    if (strcmp(name, "protobuf") == 0) {
        return protobufProtocolCreate();
    }
    return NULL;
}
```

### Protocol Negotiation

```c
// Simple version negotiation at connection start
typedef struct {
    uint8_t magic[4];      // "RAFT"
    uint8_t version;       // Protocol version
    uint8_t encoding;      // 0=binary, 1=json, 2=msgpack, 3=protobuf
    uint16_t reserved;
} kraftProtocolHeader;

bool negotiateProtocol(kraftConnection *conn) {
    kraftProtocolHeader header = {
        .magic = {'R', 'A', 'F', 'T'},
        .version = 1,
        .encoding = PROTOCOL_BINARY,
    };

    // Send our header
    send(conn, &header, sizeof(header));

    // Receive peer's header
    kraftProtocolHeader peerHeader;
    recv(conn, &peerHeader, sizeof(peerHeader));

    // Validate
    if (memcmp(peerHeader.magic, "RAFT", 4) != 0) {
        return false;
    }

    // Use minimum common version
    if (peerHeader.version != header.version) {
        // Handle version mismatch
    }

    // Encoding must match
    if (peerHeader.encoding != header.encoding) {
        return false;  // Or negotiate common encoding
    }

    return true;
}
```

---

## Performance Considerations

### Zero-Copy Decoding

```c
// BAD: Copying data
kraftDecodeStatus decode(...) {
    // Allocates and copies
    out->appendEntries.entries[i].data = malloc(dataLen);
    memcpy(out->appendEntries.entries[i].data, ptr, dataLen);
}

// GOOD: Reference into buffer
kraftDecodeStatus decode(...) {
    // Points directly into receive buffer
    out->appendEntries.entries[i].data = (uint8_t *)ptr;
    out->appendEntries.entries[i].dataLen = dataLen;
    // Caller must process before buffer is reused!
}
```

### Pre-sized Buffers

```c
// Estimate encoded size before allocation
size_t estimateAppendEntriesSize(const kraftAppendEntriesArgs *args) {
    size_t size = 1 + 40;  // Header + fixed fields

    for (uint32_t i = 0; i < args->entriesCount; i++) {
        size += 22 + args->entries[i].dataLen;
    }

    return size;
}

// Encode with pre-sized buffer
void sendAppendEntries(kraftConnection *conn, kraftAppendEntriesArgs *args) {
    size_t estimatedSize = estimateAppendEntriesSize(args);

    kraftEncodeBuffer buf;
    kraftEncodeBufferInit(&buf, alloca(estimatedSize), estimatedSize);

    proto->ops.encodeAppendEntries(proto, &buf, args);

    send(conn, buf.data, buf.len);
}
```

### Batching Small Messages

```c
// Batch multiple small messages into one write
typedef struct {
    kraftEncodeBuffer buf;
    size_t messageCount;
} MessageBatch;

void batchAdd(MessageBatch *batch, kraftProtocol *proto,
              kraftMessageType type, void *args) {
    switch (type) {
        case KRAFT_MSG_APPEND_ENTRIES_RESPONSE:
            proto->ops.encodeAppendEntriesResponse(proto, &batch->buf, args);
            break;
        // ... other types
    }
    batch->messageCount++;
}

void batchFlush(MessageBatch *batch, kraftConnection *conn) {
    if (batch->buf.len > 0) {
        send(conn, batch->buf.data, batch->buf.len);
        kraftEncodeBufferReset(&batch->buf);
        batch->messageCount = 0;
    }
}
```

---

## Testing Protocols

```c
void testProtocolRoundTrip(kraftProtocol *proto) {
    // Create test message
    kraftAppendEntriesArgs args = {
        .term = 5,
        .leaderId = 1,
        .prevLogIndex = 100,
        .prevLogTerm = 4,
        .leaderCommit = 99,
        .entriesCount = 2,
        .entries = (kraftLogEntry[]){
            {101, 5, 100, (uint8_t *)"SET x 1", 7},
            {102, 5, 100, (uint8_t *)"SET y 2", 7},
        },
    };

    // Encode
    kraftEncodeBuffer buf;
    kraftEncodeBufferInitDynamic(&buf, 256);
    assert(proto->ops.encodeAppendEntries(proto, &buf, &args));

    // Decode
    kraftMessage msg;
    size_t consumed;
    kraftDecodeStatus status = proto->ops.decode(
        proto, buf.data, buf.len, &msg, &consumed);

    assert(status == KRAFT_DECODE_OK);
    assert(consumed == buf.len);
    assert(msg.type == KRAFT_MSG_APPEND_ENTRIES);
    assert(msg.appendEntries.term == 5);
    assert(msg.appendEntries.entriesCount == 2);

    kraftEncodeBufferFree(&buf);
    printf("Round-trip test passed!\n");
}
```

---

## Next Steps

- [Architecture](../architecture.md) - System design overview
- [Client Development](client-development.md) - Using protocols in clients
- [Server Development](server-development.md) - Using protocols in servers
- [Message Reference](../reference/messages.md) - Complete message specifications
