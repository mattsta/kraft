/* kraftLinearizability.c - Linearizability Testing Core Implementation
 *
 * Implements operation history tracking with thread-safe recording.
 */

#include "kraftLinearizability.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * History Management
 * ============================================================================
 */

kraftLinHistory *kraftLinHistoryCreate(uint32_t initialCapacity) {
    if (initialCapacity == 0) {
        initialCapacity = 10000; /* Default capacity */
    }

    kraftLinHistory *history = calloc(1, sizeof(*history));
    if (!history) {
        return NULL;
    }

    history->ops = calloc(initialCapacity, sizeof(kraftLinOp));
    if (!history->ops) {
        free(history);
        return NULL;
    }

    history->capacity = initialCapacity;
    history->count = 0;
    history->nextGlobalSeq = 1;

    if (pthread_mutex_init(&history->lock, NULL) != 0) {
        free(history->ops);
        free(history);
        return NULL;
    }

    return history;
}

void kraftLinHistoryDestroy(kraftLinHistory *history) {
    if (!history) {
        return;
    }

    pthread_mutex_destroy(&history->lock);
    free(history->ops);
    free(history);
}

/**
 * historyGrow - Grow history capacity
 *
 * Assumes lock is held by caller.
 */
static bool historyGrow(kraftLinHistory *history) {
    uint32_t newCapacity = history->capacity * 2;
    kraftLinOp *newOps =
        realloc(history->ops, newCapacity * sizeof(kraftLinOp));
    if (!newOps) {
        return false;
    }

    history->ops = newOps;
    history->capacity = newCapacity;
    return true;
}

kraftLinOpId kraftLinHistoryRecordInvoke(kraftLinHistory *history,
                                         uint64_t clientId, uint64_t clientSeq,
                                         kraftLinOpType type, const char *key,
                                         const char *value) {
    kraftLinOpId opId = {0};

    pthread_mutex_lock(&history->lock);

    /* Ensure capacity */
    if (history->count >= history->capacity) {
        if (!historyGrow(history)) {
            pthread_mutex_unlock(&history->lock);
            return opId; /* Return invalid ID (all zeros) */
        }
    }

    /* Assign operation ID */
    opId.clientId = clientId;
    opId.seq = clientSeq;
    opId.global = history->nextGlobalSeq++;

    /* Initialize operation */
    kraftLinOp *op = &history->ops[history->count++];
    memset(op, 0, sizeof(*op));

    op->id = opId;
    op->type = type;
    strncpy(op->key, key, sizeof(op->key) - 1);
    if (value) {
        strncpy(op->value, value, sizeof(op->value) - 1);
    }
    op->invokeUs = kraftLinGetMonotonicUs();
    op->completed = false;
    op->pending = true;

    history->totalInvocations++;

    pthread_mutex_unlock(&history->lock);

    return opId;
}

void kraftLinHistoryRecordReturn(kraftLinHistory *history, kraftLinOpId opId,
                                 bool success, const char *value) {
    pthread_mutex_lock(&history->lock);

    /* Find operation (linear search for now; optimize with hash map later) */
    kraftLinOp *op = NULL;
    for (uint32_t i = 0; i < history->count; i++) {
        if (history->ops[i].id.global == opId.global &&
            history->ops[i].id.clientId == opId.clientId &&
            history->ops[i].id.seq == opId.seq) {
            op = &history->ops[i];
            break;
        }
    }

    if (op && op->pending) {
        op->returnUs = kraftLinGetMonotonicUs();
        op->completed = true;
        op->pending = false;
        op->success = success;

        /* For reads, record observed value */
        if (op->type == KRAFT_LIN_OP_GET && value) {
            strncpy(op->value, value, sizeof(op->value) - 1);
        }

        history->totalCompletions++;
    }

    pthread_mutex_unlock(&history->lock);
}

void kraftLinHistoryRecordTimeout(kraftLinHistory *history, kraftLinOpId opId) {
    pthread_mutex_lock(&history->lock);

    /* Find operation */
    kraftLinOp *op = NULL;
    for (uint32_t i = 0; i < history->count; i++) {
        if (history->ops[i].id.global == opId.global) {
            op = &history->ops[i];
            break;
        }
    }

    if (op && op->pending) {
        op->returnUs = kraftLinGetMonotonicUs();
        op->completed = false;
        op->pending = false;
        op->timedOut = true;

        history->totalTimeouts++;
    }

    pthread_mutex_unlock(&history->lock);
}

kraftLinOp *kraftLinHistoryGetOp(kraftLinHistory *history, kraftLinOpId opId) {
    if (!history) {
        return NULL;
    }

    /* Linear search (this is called during verification, not hot path) */
    for (uint32_t i = 0; i < history->count; i++) {
        if (history->ops[i].id.global == opId.global) {
            return &history->ops[i];
        }
    }

    return NULL;
}

void kraftLinHistoryGetStats(const kraftLinHistory *history,
                             uint64_t *invocations, uint64_t *completions,
                             uint64_t *timeouts) {
    if (!history) {
        if (invocations) {
            *invocations = 0;
        }
        if (completions) {
            *completions = 0;
        }
        if (timeouts) {
            *timeouts = 0;
        }
        return;
    }

    if (invocations) {
        *invocations = history->totalInvocations;
    }
    if (completions) {
        *completions = history->totalCompletions;
    }
    if (timeouts) {
        *timeouts = history->totalTimeouts;
    }
}

/* ============================================================================
 * Verification Result Management
 * ============================================================================
 */

void kraftLinResultDestroy(kraftLinResult *result) {
    if (!result) {
        return;
    }

    free(result->violation.opIndices);
    memset(result, 0, sizeof(*result));
}

void kraftLinResultPrint(const kraftLinResult *result,
                         const kraftLinHistory *history) {
    if (!result) {
        return;
    }

    if (result->isLinearizable) {
        printf("=== LINEARIZABILITY: PASS ===\n");
        printf("All operations are linearizable.\n");
        printf("\n");
        printf("Verification statistics:\n");
        printf("  Time:          %" PRIu64 " us\n", result->verificationTimeUs);
        printf("  Graph edges:   %" PRIu64 "\n", result->graphEdgeCount);
        printf("  States explored: %" PRIu64 "\n", result->statesExplored);
        printf("\n");
    } else {
        printf("=== LINEARIZABILITY: VIOLATION DETECTED ===\n");
        printf("\n");
        printf("%s\n", result->violation.explanation);
        printf("\n");
        printf("Minimal failing history (%u operations):\n",
               result->violation.opCount);

        for (uint32_t i = 0; i < result->violation.opCount && history; i++) {
            uint32_t idx = result->violation.opIndices[i];
            if (idx >= history->count) {
                continue;
            }

            const kraftLinOp *op = &history->ops[idx];
            printf("  [%3u] Client %2lu: ", idx, op->id.clientId);
            printf("%-4s key=%-10s ", kraftLinOpTypeString(op->type), op->key);

            if (op->type == KRAFT_LIN_OP_SET || op->type == KRAFT_LIN_OP_INCR) {
                printf("value=%-10s ", op->value);
            } else if (op->type == KRAFT_LIN_OP_GET) {
                printf("observed=%-10s ", op->value);
            }

            printf("[%" PRIu64 " us - %" PRIu64 " us]", op->invokeUs,
                   op->returnUs);

            if (!op->completed) {
                printf(" TIMEOUT");
            } else if (!op->success) {
                printf(" FAILED");
            }

            printf("\n");
        }
        printf("\n");
    }
}

void kraftLinResultPrintJSON(const kraftLinResult *result,
                             const kraftLinHistory *history, FILE *out) {
    if (!out || !result) {
        return;
    }

    fprintf(out, "{\n");
    fprintf(out, "  \"linearizable\": %s,\n",
            result->isLinearizable ? "true" : "false");

    if (!result->isLinearizable) {
        fprintf(out, "  \"violation\": {\n");
        fprintf(out, "    \"explanation\": \"%s\",\n",
                result->violation.explanation);
        fprintf(out, "    \"operations\": [\n");

        for (uint32_t i = 0; i < result->violation.opCount && history; i++) {
            uint32_t idx = result->violation.opIndices[i];
            if (idx >= history->count) {
                continue;
            }

            const kraftLinOp *op = &history->ops[idx];
            fprintf(out, "      {\n");
            fprintf(out, "        \"index\": %u,\n", idx);
            fprintf(out, "        \"client\": %" PRIu64 ",\n", op->id.clientId);
            fprintf(out, "        \"seq\": %" PRIu64 ",\n", op->id.seq);
            fprintf(out, "        \"type\": \"%s\",\n",
                    kraftLinOpTypeString(op->type));
            fprintf(out, "        \"key\": \"%s\",\n", op->key);
            fprintf(out, "        \"value\": \"%s\",\n", op->value);
            fprintf(out, "        \"invoke_us\": %" PRIu64 ",\n", op->invokeUs);
            fprintf(out, "        \"return_us\": %" PRIu64 ",\n", op->returnUs);
            fprintf(out, "        \"success\": %s\n",
                    op->success ? "true" : "false");
            fprintf(out, "      }%s\n",
                    (i < result->violation.opCount - 1) ? "," : "");
        }

        fprintf(out, "    ]\n");
        fprintf(out, "  },\n");
    }

    fprintf(out, "  \"verification_time_us\": %" PRIu64 ",\n",
            result->verificationTimeUs);
    fprintf(out, "  \"graph_edges\": %" PRIu64 ",\n", result->graphEdgeCount);
    fprintf(out, "  \"states_explored\": %" PRIu64 "\n",
            result->statesExplored);
    fprintf(out, "}\n");
}
