/* loopyCluster - Cluster membership and discovery
 *
 * Implementation of dynamic cluster membership management.
 */

#include "loopyCluster.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

#define MAX_MEMBERS 128
#define DEFAULT_GOSSIP_INTERVAL 1000 /* ms */
#define DEFAULT_JOIN_TIMEOUT 5000    /* ms */
#define DEFAULT_CONFIG_TIMEOUT 10000 /* ms */
#define CONFIG_FILENAME "cluster.conf"

/* Cluster message magic bytes for identification */
#define CLUSTER_MSG_MAGIC 0x434C5354 /* "CLST" */

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

struct loopyCluster {
    /* Configuration */
    loopyClusterConfig config;

    /* State */
    loopyClusterState state;

    /* Local member */
    loopyMember localMember;

    /* Membership */
    loopyMember members[MAX_MEMBERS];
    size_t memberCount;

    /* Event loop integration */
    loopyLoop *loop;
    loopyTimer *gossipTimer;
    loopyTimer *joinTimer;

    /* Join state */
    char seedAddr[64];
    int seedPort;

    /* Statistics */
    loopyClusterStats stats;
};

/* ============================================================================
 * Message Encoding
 *
 * All cluster messages have a common header:
 *   [4 bytes] Magic (CLUSTER_MSG_MAGIC)
 *   [1 byte]  Message type
 *   [4 bytes] Payload length
 *   [N bytes] Payload
 *
 * Member encoding (used in payloads):
 *   [8 bytes] nodeId
 *   [8 bytes] runId
 *   [1 byte]  addr length
 *   [N bytes] addr
 *   [2 bytes] port
 *   [1 byte]  flags (isVoter | isLearner << 1)
 * ============================================================================
 */

/* Minimum header size: magic(4) + type(1) + length(4) = 9 */
#define CLUSTER_MSG_HEADER_SIZE 9

/* Member encoding size: nodeId(8) + runId(8) + addrLen(1) + port(2) + flags(1)
 * = 20 + addr */
#define MEMBER_BASE_SIZE 20

static size_t encodeMember(const loopyMember *m, uint8_t *buf, size_t bufSize) {
    size_t addrLen = strlen(m->addr);
    size_t needed = MEMBER_BASE_SIZE + addrLen;

    if (bufSize < needed) {
        return 0;
    }

    /* nodeId (little-endian) */
    buf[0] = (m->nodeId) & 0xFF;
    buf[1] = (m->nodeId >> 8) & 0xFF;
    buf[2] = (m->nodeId >> 16) & 0xFF;
    buf[3] = (m->nodeId >> 24) & 0xFF;
    buf[4] = (m->nodeId >> 32) & 0xFF;
    buf[5] = (m->nodeId >> 40) & 0xFF;
    buf[6] = (m->nodeId >> 48) & 0xFF;
    buf[7] = (m->nodeId >> 56) & 0xFF;

    /* runId (little-endian) */
    buf[8] = (m->runId) & 0xFF;
    buf[9] = (m->runId >> 8) & 0xFF;
    buf[10] = (m->runId >> 16) & 0xFF;
    buf[11] = (m->runId >> 24) & 0xFF;
    buf[12] = (m->runId >> 32) & 0xFF;
    buf[13] = (m->runId >> 40) & 0xFF;
    buf[14] = (m->runId >> 48) & 0xFF;
    buf[15] = (m->runId >> 56) & 0xFF;

    /* addr length */
    buf[16] = (uint8_t)addrLen;

    /* addr */
    memcpy(buf + 17, m->addr, addrLen);

    /* port (little-endian) */
    buf[17 + addrLen] = m->port & 0xFF;
    buf[18 + addrLen] = (m->port >> 8) & 0xFF;

    /* flags */
    uint8_t flags = 0;
    if (m->isVoter) {
        flags |= 0x01;
    }
    if (m->isLearner) {
        flags |= 0x02;
    }
    buf[19 + addrLen] = flags;

    return needed;
}

static size_t decodeMember(const uint8_t *buf, size_t bufSize, loopyMember *m) {
    if (bufSize < MEMBER_BASE_SIZE) {
        return 0;
    }

    memset(m, 0, sizeof(*m));

    /* nodeId */
    m->nodeId = (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
                ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
                ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
                ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);

    /* runId */
    m->runId = (uint64_t)buf[8] | ((uint64_t)buf[9] << 8) |
               ((uint64_t)buf[10] << 16) | ((uint64_t)buf[11] << 24) |
               ((uint64_t)buf[12] << 32) | ((uint64_t)buf[13] << 40) |
               ((uint64_t)buf[14] << 48) | ((uint64_t)buf[15] << 56);

    /* addr length */
    size_t addrLen = buf[16];
    if (addrLen >= sizeof(m->addr) || bufSize < MEMBER_BASE_SIZE + addrLen) {
        return 0;
    }

    /* addr */
    memcpy(m->addr, buf + 17, addrLen);
    m->addr[addrLen] = '\0';

    /* port */
    m->port = buf[17 + addrLen] | (buf[18 + addrLen] << 8);

    /* flags */
    uint8_t flags = buf[19 + addrLen];
    m->isVoter = (flags & 0x01) != 0;
    m->isLearner = (flags & 0x02) != 0;

    return MEMBER_BASE_SIZE + addrLen;
}

static size_t encodeHeader(uint8_t *buf, loopyClusterMsgType type,
                           uint32_t payloadLen) {
    /* Magic */
    buf[0] = (CLUSTER_MSG_MAGIC) & 0xFF;
    buf[1] = (CLUSTER_MSG_MAGIC >> 8) & 0xFF;
    buf[2] = (CLUSTER_MSG_MAGIC >> 16) & 0xFF;
    buf[3] = (CLUSTER_MSG_MAGIC >> 24) & 0xFF;

    /* Type */
    buf[4] = (uint8_t)type;

    /* Payload length */
    buf[5] = payloadLen & 0xFF;
    buf[6] = (payloadLen >> 8) & 0xFF;
    buf[7] = (payloadLen >> 16) & 0xFF;
    buf[8] = (payloadLen >> 24) & 0xFF;

    return CLUSTER_MSG_HEADER_SIZE;
}

static bool decodeHeader(const uint8_t *buf, size_t bufSize,
                         loopyClusterMsgType *type, uint32_t *payloadLen) {
    if (bufSize < CLUSTER_MSG_HEADER_SIZE) {
        return false;
    }

    /* Check magic */
    uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    if (magic != CLUSTER_MSG_MAGIC) {
        return false;
    }

    *type = (loopyClusterMsgType)buf[4];
    *payloadLen = buf[5] | (buf[6] << 8) | (buf[7] << 16) | (buf[8] << 24);

    return true;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static loopyMember *findMember(loopyCluster *c, uint64_t nodeId) {
    for (size_t i = 0; i < c->memberCount; i++) {
        if (c->members[i].nodeId == nodeId) {
            return &c->members[i];
        }
    }
    return NULL;
}

static bool addMemberInternal(loopyCluster *c, const loopyMember *m) {
    if (c->memberCount >= MAX_MEMBERS) {
        return false;
    }

    /* Check for duplicate */
    if (findMember(c, m->nodeId)) {
        return false;
    }

    c->members[c->memberCount++] = *m;
    return true;
}

static bool removeMemberInternal(loopyCluster *c, uint64_t nodeId) {
    for (size_t i = 0; i < c->memberCount; i++) {
        if (c->members[i].nodeId == nodeId) {
            /* Shift remaining members */
            for (size_t j = i; j < c->memberCount - 1; j++) {
                c->members[j] = c->members[j + 1];
            }
            c->memberCount--;
            return true;
        }
    }
    return false;
}

static void notifyCallback(loopyCluster *c, loopyClusterEvent event,
                           const loopyMember *member) {
    if (c->config.callback) {
        c->config.callback(c, event, member, c->config.userData);
    }
}

/* ============================================================================
 * Timer Callbacks
 * ============================================================================
 */

static void gossipTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                void *userData) {
    (void)loop;
    (void)timer;
    loopyCluster *c = (loopyCluster *)userData;

    /* Create and broadcast gossip message */
    void *gossipData = NULL;
    size_t gossipLen = 0;

    if (loopyClusterCreateGossip(c, &gossipData, &gossipLen)) {
        /* Broadcast via send handler if provided */
        if (c->config.sendHandler) {
            c->config.sendHandler(c, gossipData, gossipLen, c->config.userData);
        }
        free(gossipData);
        c->stats.gossipSent++;
    }
}

static void joinTimerCallback(loopyLoop *loop, loopyTimer *timer,
                              void *userData) {
    (void)loop;
    (void)timer;
    loopyCluster *c = (loopyCluster *)userData;

    if (c->state == LOOPY_CLUSTER_JOINING) {
        /* Join timed out */
        c->state = LOOPY_CLUSTER_INIT;
        notifyCallback(c, LOOPY_CLUSTER_EVENT_JOIN_FAILED, NULL);
    }

    c->joinTimer = NULL;
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void loopyClusterConfigInit(loopyClusterConfig *cfg) {
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->clusterId = 1;
    cfg->gossipIntervalMs = DEFAULT_GOSSIP_INTERVAL;
    cfg->joinTimeoutMs = DEFAULT_JOIN_TIMEOUT;
    cfg->configChangeTimeoutMs = DEFAULT_CONFIG_TIMEOUT;
}

void loopyClusterConfigSetLocal(loopyClusterConfig *cfg, uint64_t nodeId,
                                const char *addr, int port) {
    if (!cfg) {
        return;
    }

    cfg->localNodeId = nodeId;
    if (addr) {
        strncpy(cfg->localAddr, addr, sizeof(cfg->localAddr) - 1);
    }
    cfg->localPort = port;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyCluster *loopyClusterCreate(const loopyClusterConfig *cfg) {
    if (!cfg || cfg->localNodeId == 0) {
        return NULL;
    }

    loopyCluster *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }

    c->config = *cfg;
    c->state = LOOPY_CLUSTER_INIT;

    /* Initialize local member */
    c->localMember.nodeId = cfg->localNodeId;
    strncpy(c->localMember.addr, cfg->localAddr,
            sizeof(c->localMember.addr) - 1);
    c->localMember.port = cfg->localPort;
    c->localMember.isVoter = true;

    /* Generate unique run ID */
    c->localMember.runId = ((uint64_t)time(NULL) << 20) ^
                           ((uint64_t)rand() << 10) ^ (uint64_t)rand();

    return c;
}

void loopyClusterDestroy(loopyCluster *cluster) {
    if (!cluster) {
        return;
    }

    /* Cancel timers */
    if (cluster->gossipTimer) {
        loopyTimerCancel(cluster->gossipTimer);
    }
    if (cluster->joinTimer) {
        loopyTimerCancel(cluster->joinTimer);
    }

    free(cluster);
}

bool loopyClusterBootstrap(loopyCluster *cluster) {
    if (!cluster || cluster->state != LOOPY_CLUSTER_INIT) {
        return false;
    }

    cluster->state = LOOPY_CLUSTER_BOOTSTRAPPING;

    /* Add ourselves as the only member */
    if (!addMemberInternal(cluster, &cluster->localMember)) {
        cluster->state = LOOPY_CLUSTER_INIT;
        return false;
    }

    cluster->state = LOOPY_CLUSTER_ACTIVE;
    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_BOOTSTRAPPED,
                   &cluster->localMember);

    return true;
}

bool loopyClusterJoin(loopyCluster *cluster, const char *seedAddr,
                      int seedPort) {
    if (!cluster || !seedAddr || seedPort <= 0) {
        return false;
    }

    if (cluster->state != LOOPY_CLUSTER_INIT) {
        return false;
    }

    cluster->state = LOOPY_CLUSTER_JOINING;
    strncpy(cluster->seedAddr, seedAddr, sizeof(cluster->seedAddr) - 1);
    cluster->seedPort = seedPort;

    /* The actual join request will be sent when start() is called
     * and the platform is integrated */

    cluster->stats.joinRequestsSent++;
    return true;
}

bool loopyClusterStart(loopyCluster *cluster, loopyLoop *loop) {
    if (!cluster || !loop) {
        return false;
    }

    cluster->loop = loop;

    /* Start gossip timer if we're active */
    if (cluster->state == LOOPY_CLUSTER_ACTIVE) {
        cluster->gossipTimer =
            loopyTimerPeriodicMs(loop, cluster->config.gossipIntervalMs,
                                 gossipTimerCallback, cluster);
    }

    /* Start join timeout timer if we're joining */
    if (cluster->state == LOOPY_CLUSTER_JOINING) {
        cluster->joinTimer = loopyTimerOneShotMs(
            loop, cluster->config.joinTimeoutMs, joinTimerCallback, cluster);
    }

    return true;
}

void loopyClusterStop(loopyCluster *cluster) {
    if (!cluster) {
        return;
    }

    /* Cancel timers */
    if (cluster->gossipTimer) {
        loopyTimerCancel(cluster->gossipTimer);
        cluster->gossipTimer = NULL;
    }
    if (cluster->joinTimer) {
        loopyTimerCancel(cluster->joinTimer);
        cluster->joinTimer = NULL;
    }

    cluster->state = LOOPY_CLUSTER_STOPPED;
}

/* ============================================================================
 * Membership Management
 * ============================================================================
 */

bool loopyClusterAddMember(loopyCluster *cluster, uint64_t nodeId,
                           const char *addr, int port, bool isVoter) {
    if (!cluster || nodeId == 0 || !addr || port <= 0) {
        return false;
    }

    if (cluster->state != LOOPY_CLUSTER_ACTIVE) {
        return false;
    }

    /* Check if already a member */
    if (findMember(cluster, nodeId)) {
        return false;
    }

    loopyMember m = {0};
    m.nodeId = nodeId;
    strncpy(m.addr, addr, sizeof(m.addr) - 1);
    m.port = port;
    m.isVoter = isVoter;
    m.isLearner = !isVoter;

    if (!addMemberInternal(cluster, &m)) {
        return false;
    }

    cluster->stats.memberAdded++;
    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_MEMBER_ADDED, &m);

    return true;
}

bool loopyClusterRemoveMember(loopyCluster *cluster, uint64_t nodeId) {
    if (!cluster || nodeId == 0) {
        return false;
    }

    if (cluster->state != LOOPY_CLUSTER_ACTIVE) {
        return false;
    }

    /* Can't remove ourselves */
    if (nodeId == cluster->localMember.nodeId) {
        return false;
    }

    loopyMember *m = findMember(cluster, nodeId);
    if (!m) {
        return false;
    }

    loopyMember removed = *m;
    if (!removeMemberInternal(cluster, nodeId)) {
        return false;
    }

    cluster->stats.memberRemoved++;
    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_MEMBER_REMOVED, &removed);

    return true;
}

bool loopyClusterPromoteMember(loopyCluster *cluster, uint64_t nodeId) {
    if (!cluster || nodeId == 0) {
        return false;
    }

    loopyMember *m = findMember(cluster, nodeId);
    if (!m || m->isVoter) {
        return false;
    }

    m->isVoter = true;
    m->isLearner = false;

    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_MEMBER_UPDATED, m);
    return true;
}

bool loopyClusterDemoteMember(loopyCluster *cluster, uint64_t nodeId) {
    if (!cluster || nodeId == 0) {
        return false;
    }

    loopyMember *m = findMember(cluster, nodeId);
    if (!m || !m->isVoter) {
        return false;
    }

    m->isVoter = false;
    m->isLearner = true;

    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_MEMBER_UPDATED, m);
    return true;
}

/* ============================================================================
 * Membership Queries
 * ============================================================================
 */

loopyClusterState loopyClusterGetState(const loopyCluster *cluster) {
    if (!cluster) {
        return LOOPY_CLUSTER_STOPPED;
    }
    return cluster->state;
}

const char *loopyClusterStateName(loopyClusterState state) {
    switch (state) {
    case LOOPY_CLUSTER_INIT:
        return "INIT";
    case LOOPY_CLUSTER_JOINING:
        return "JOINING";
    case LOOPY_CLUSTER_BOOTSTRAPPING:
        return "BOOTSTRAPPING";
    case LOOPY_CLUSTER_ACTIVE:
        return "ACTIVE";
    case LOOPY_CLUSTER_CONFIG_CHANGE:
        return "CONFIG_CHANGE";
    case LOOPY_CLUSTER_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

size_t loopyClusterGetMemberCount(const loopyCluster *cluster) {
    if (!cluster) {
        return 0;
    }
    return cluster->memberCount;
}

size_t loopyClusterGetVoterCount(const loopyCluster *cluster) {
    if (!cluster) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < cluster->memberCount; i++) {
        if (cluster->members[i].isVoter) {
            count++;
        }
    }
    return count;
}

bool loopyClusterGetMember(const loopyCluster *cluster, uint64_t nodeId,
                           loopyMember *member) {
    if (!cluster || !member) {
        return false;
    }

    /* Cast away const for internal lookup (doesn't modify) */
    loopyMember *m = findMember((loopyCluster *)cluster, nodeId);
    if (!m) {
        return false;
    }

    *member = *m;
    return true;
}

bool loopyClusterIsMember(const loopyCluster *cluster, uint64_t nodeId) {
    if (!cluster) {
        return false;
    }
    return findMember((loopyCluster *)cluster, nodeId) != NULL;
}

bool loopyClusterIsVoter(const loopyCluster *cluster, uint64_t nodeId) {
    if (!cluster) {
        return false;
    }
    loopyMember *m = findMember((loopyCluster *)cluster, nodeId);
    return m && m->isVoter;
}

const loopyMember *loopyClusterGetLocal(const loopyCluster *cluster) {
    if (!cluster) {
        return NULL;
    }
    return &cluster->localMember;
}

void loopyClusterForEachMember(loopyCluster *cluster,
                               loopyClusterMemberIter *iter, void *userData) {
    if (!cluster || !iter) {
        return;
    }

    for (size_t i = 0; i < cluster->memberCount; i++) {
        if (!iter(cluster, &cluster->members[i], userData)) {
            break;
        }
    }
}

void loopyClusterForEachVoter(loopyCluster *cluster,
                              loopyClusterMemberIter *iter, void *userData) {
    if (!cluster || !iter) {
        return;
    }

    for (size_t i = 0; i < cluster->memberCount; i++) {
        if (cluster->members[i].isVoter) {
            if (!iter(cluster, &cluster->members[i], userData)) {
                break;
            }
        }
    }
}

/* ============================================================================
 * Configuration Persistence
 * ============================================================================
 */

bool loopyClusterSave(loopyCluster *cluster) {
    if (!cluster || !cluster->config.dataDir) {
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", cluster->config.dataDir,
             CONFIG_FILENAME);

    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }

    /* Simple binary format:
     *   [8 bytes] clusterId
     *   [4 bytes] memberCount
     *   [N * encoded members]
     */
    uint8_t header[12];
    header[0] = cluster->config.clusterId & 0xFF;
    header[1] = (cluster->config.clusterId >> 8) & 0xFF;
    header[2] = (cluster->config.clusterId >> 16) & 0xFF;
    header[3] = (cluster->config.clusterId >> 24) & 0xFF;
    header[4] = (cluster->config.clusterId >> 32) & 0xFF;
    header[5] = (cluster->config.clusterId >> 40) & 0xFF;
    header[6] = (cluster->config.clusterId >> 48) & 0xFF;
    header[7] = (cluster->config.clusterId >> 56) & 0xFF;
    header[8] = cluster->memberCount & 0xFF;
    header[9] = (cluster->memberCount >> 8) & 0xFF;
    header[10] = (cluster->memberCount >> 16) & 0xFF;
    header[11] = (cluster->memberCount >> 24) & 0xFF;

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    /* Write each member */
    uint8_t buf[128];
    for (size_t i = 0; i < cluster->memberCount; i++) {
        size_t len = encodeMember(&cluster->members[i], buf, sizeof(buf));
        if (len == 0 || fwrite(buf, 1, len, f) != len) {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

bool loopyClusterLoad(loopyCluster *cluster) {
    if (!cluster || !cluster->config.dataDir) {
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", cluster->config.dataDir,
             CONFIG_FILENAME);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    /* Read header */
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    uint64_t clusterId =
        (uint64_t)header[0] | ((uint64_t)header[1] << 8) |
        ((uint64_t)header[2] << 16) | ((uint64_t)header[3] << 24) |
        ((uint64_t)header[4] << 32) | ((uint64_t)header[5] << 40) |
        ((uint64_t)header[6] << 48) | ((uint64_t)header[7] << 56);

    uint32_t memberCount =
        header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);

    /* Validate */
    if (clusterId != cluster->config.clusterId) {
        fclose(f);
        return false;
    }

    if (memberCount > MAX_MEMBERS) {
        fclose(f);
        return false;
    }

    /* Read members */
    cluster->memberCount = 0;
    uint8_t buf[128];

    for (uint32_t i = 0; i < memberCount; i++) {
        /* Read member data - we need to read enough to decode */
        if (fread(buf, 1, MEMBER_BASE_SIZE, f) != MEMBER_BASE_SIZE) {
            break;
        }

        /* Get addr length and read the rest */
        size_t addrLen = buf[16];
        if (addrLen > 0 && addrLen < 64) {
            if (fread(buf + MEMBER_BASE_SIZE, 1, addrLen, f) != addrLen) {
                break;
            }
        }

        loopyMember m;
        if (decodeMember(buf, MEMBER_BASE_SIZE + addrLen, &m) > 0) {
            addMemberInternal(cluster, &m);
        }
    }

    fclose(f);

    if (cluster->memberCount > 0) {
        cluster->state = LOOPY_CLUSTER_ACTIVE;
        return true;
    }

    return false;
}

/* ============================================================================
 * Gossip Protocol
 * ============================================================================
 */

bool loopyClusterProcessGossip(loopyCluster *cluster, const void *data,
                               size_t len) {
    if (!cluster || !data || len < CLUSTER_MSG_HEADER_SIZE) {
        return false;
    }

    const uint8_t *buf = (const uint8_t *)data;
    loopyClusterMsgType type;
    uint32_t payloadLen;

    if (!decodeHeader(buf, len, &type, &payloadLen)) {
        return false;
    }

    if (type != LOOPY_CLUSTER_MSG_GOSSIP) {
        return false;
    }

    if (len < CLUSTER_MSG_HEADER_SIZE + payloadLen) {
        return false;
    }

    const uint8_t *payload = buf + CLUSTER_MSG_HEADER_SIZE;
    size_t offset = 0;

    /* Gossip payload format:
     *   [4 bytes] member count
     *   [N * encoded members]
     */
    if (payloadLen < 4) {
        return false;
    }

    uint32_t count = payload[0] | (payload[1] << 8) | (payload[2] << 16) |
                     (payload[3] << 24);
    offset = 4;

    for (uint32_t i = 0; i < count && offset < payloadLen; i++) {
        loopyMember m;
        size_t decoded =
            decodeMember(payload + offset, payloadLen - offset, &m);
        if (decoded == 0) {
            break;
        }
        offset += decoded;

        /* Update our membership if we don't know about this node */
        if (!loopyClusterIsMember(cluster, m.nodeId)) {
            if (addMemberInternal(cluster, &m)) {
                notifyCallback(cluster, LOOPY_CLUSTER_EVENT_MEMBER_ADDED, &m);
            }
        }
    }

    cluster->stats.gossipReceived++;
    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_GOSSIP_RECEIVED, NULL);

    return true;
}

bool loopyClusterProcessJoinRequest(loopyCluster *cluster, const void *data,
                                    size_t len, void **response,
                                    size_t *responseLen) {
    if (!cluster || !data || !response || !responseLen) {
        return false;
    }

    const uint8_t *buf = (const uint8_t *)data;
    loopyClusterMsgType type;
    uint32_t payloadLen;

    if (!decodeHeader(buf, len, &type, &payloadLen)) {
        return false;
    }

    if (type != LOOPY_CLUSTER_MSG_JOIN_REQUEST) {
        return false;
    }

    /* Decode joining member */
    loopyMember joiner;
    if (decodeMember(buf + CLUSTER_MSG_HEADER_SIZE, payloadLen, &joiner) == 0) {
        return false;
    }

    cluster->stats.joinRequestsReceived++;

    /* Check with join handler if provided */
    if (cluster->config.joinHandler) {
        if (!cluster->config.joinHandler(cluster, &joiner,
                                         cluster->config.userData)) {
            return false;
        }
    }

    /* Add the new member as a learner initially */
    joiner.isVoter = false;
    joiner.isLearner = true;

    if (!addMemberInternal(cluster, &joiner)) {
        return false;
    }

    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_MEMBER_ADDED, &joiner);

    /* Create response with full membership */
    size_t bufSize = CLUSTER_MSG_HEADER_SIZE + 4 +
                     (cluster->memberCount * (MEMBER_BASE_SIZE + 64));
    uint8_t *respBuf = malloc(bufSize);
    if (!respBuf) {
        return false;
    }

    /* Encode members into payload */
    size_t offset = CLUSTER_MSG_HEADER_SIZE + 4;
    for (size_t i = 0; i < cluster->memberCount; i++) {
        size_t encoded = encodeMember(&cluster->members[i], respBuf + offset,
                                      bufSize - offset);
        if (encoded == 0) {
            break;
        }
        offset += encoded;
    }

    /* Member count */
    uint8_t *countPtr = respBuf + CLUSTER_MSG_HEADER_SIZE;
    countPtr[0] = cluster->memberCount & 0xFF;
    countPtr[1] = (cluster->memberCount >> 8) & 0xFF;
    countPtr[2] = (cluster->memberCount >> 16) & 0xFF;
    countPtr[3] = (cluster->memberCount >> 24) & 0xFF;

    /* Header */
    encodeHeader(respBuf, LOOPY_CLUSTER_MSG_JOIN_RESPONSE,
                 (uint32_t)(offset - CLUSTER_MSG_HEADER_SIZE));

    *response = respBuf;
    *responseLen = offset;

    return true;
}

bool loopyClusterProcessJoinResponse(loopyCluster *cluster, const void *data,
                                     size_t len) {
    if (!cluster || !data || len < CLUSTER_MSG_HEADER_SIZE) {
        return false;
    }

    if (cluster->state != LOOPY_CLUSTER_JOINING) {
        return false;
    }

    const uint8_t *buf = (const uint8_t *)data;
    loopyClusterMsgType type;
    uint32_t payloadLen;

    if (!decodeHeader(buf, len, &type, &payloadLen)) {
        return false;
    }

    if (type != LOOPY_CLUSTER_MSG_JOIN_RESPONSE) {
        return false;
    }

    /* Parse membership from response */
    const uint8_t *payload = buf + CLUSTER_MSG_HEADER_SIZE;

    if (payloadLen < 4) {
        return false;
    }

    uint32_t count = payload[0] | (payload[1] << 8) | (payload[2] << 16) |
                     (payload[3] << 24);
    size_t offset = 4;

    /* Clear existing membership and rebuild from response */
    cluster->memberCount = 0;

    for (uint32_t i = 0; i < count && offset < payloadLen; i++) {
        loopyMember m;
        size_t decoded =
            decodeMember(payload + offset, payloadLen - offset, &m);
        if (decoded == 0) {
            break;
        }
        offset += decoded;

        addMemberInternal(cluster, &m);
    }

    /* Cancel join timeout */
    if (cluster->joinTimer) {
        loopyTimerCancel(cluster->joinTimer);
        cluster->joinTimer = NULL;
    }

    cluster->state = LOOPY_CLUSTER_ACTIVE;
    notifyCallback(cluster, LOOPY_CLUSTER_EVENT_JOINED, &cluster->localMember);

    return true;
}

bool loopyClusterCreateGossip(loopyCluster *cluster, void **data, size_t *len) {
    if (!cluster || !data || !len) {
        return false;
    }

    /* Calculate buffer size needed */
    size_t bufSize = CLUSTER_MSG_HEADER_SIZE + 4 +
                     (cluster->memberCount * (MEMBER_BASE_SIZE + 64));
    uint8_t *buf = malloc(bufSize);
    if (!buf) {
        return false;
    }

    /* Encode members */
    size_t offset = CLUSTER_MSG_HEADER_SIZE + 4;
    for (size_t i = 0; i < cluster->memberCount; i++) {
        size_t encoded =
            encodeMember(&cluster->members[i], buf + offset, bufSize - offset);
        if (encoded == 0) {
            break;
        }
        offset += encoded;
    }

    /* Member count */
    uint8_t *countPtr = buf + CLUSTER_MSG_HEADER_SIZE;
    countPtr[0] = cluster->memberCount & 0xFF;
    countPtr[1] = (cluster->memberCount >> 8) & 0xFF;
    countPtr[2] = (cluster->memberCount >> 16) & 0xFF;
    countPtr[3] = (cluster->memberCount >> 24) & 0xFF;

    /* Header */
    encodeHeader(buf, LOOPY_CLUSTER_MSG_GOSSIP,
                 (uint32_t)(offset - CLUSTER_MSG_HEADER_SIZE));

    *data = buf;
    *len = offset;

    return true;
}

/* ============================================================================
 * Statistics
 * ============================================================================
 */

void loopyClusterGetStats(const loopyCluster *cluster,
                          loopyClusterStats *stats) {
    if (!stats) {
        return;
    }

    if (!cluster) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = cluster->stats;
}

/* ============================================================================
 * Message Type Detection
 * ============================================================================
 */

bool loopyClusterIsClusterMessage(const void *data, size_t len) {
    if (!data || len < CLUSTER_MSG_HEADER_SIZE) {
        return false;
    }

    const uint8_t *buf = (const uint8_t *)data;
    uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

    return magic == CLUSTER_MSG_MAGIC;
}

int loopyClusterGetMessageType(const void *data, size_t len) {
    if (!loopyClusterIsClusterMessage(data, len)) {
        return -1;
    }

    const uint8_t *buf = (const uint8_t *)data;
    return buf[4];
}
