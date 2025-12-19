/* main.c - Kraft Loopy Demo CLI entry point
 *
 * Command-line interface for running a Raft node using the loopy-demo platform.
 * Includes interactive key-value store demonstration with live data flows.
 *
 * Usage:
 *   # Start single-node cluster (immediately becomes leader)
 *   kraft-loopy --listen 127.0.0.1:5001
 *
 *   # Start with explicit node ID
 *   kraft-loopy --node 1 --listen 127.0.0.1:5001
 *
 *   # Join existing cluster (dynamic membership)
 *   kraft-loopy --listen 127.0.0.1:5002 --join 127.0.0.1:5001
 *
 *   # Bootstrap static cluster (all nodes know each other)
 *   kraft-loopy --node 1 --listen 127.0.0.1:5001 --cluster
 * 127.0.0.1:5001,127.0.0.1:5002,127.0.0.1:5003
 *
 *   # With data directory for persistence
 *   kraft-loopy --listen 127.0.0.1:5001 --data /var/lib/kraft/node1
 *
 * Interactive Commands (once running):
 *   SET key value     - Store a key-value pair
 *   GET key           - Retrieve a value
 *   DEL key           - Delete a key
 *   INCR key [n]      - Increment a numeric value
 *   STAT              - Show KV store statistics
 *   STATUS            - Show Raft cluster status
 *   PEERS             - Show connected peers
 *   QUIT              - Shutdown the node
 */

#include "loopyClientAPI.h"
#include "loopyKV.h"
#include "loopyPlatform.h"
#include "loopyServer.h"

/* loopy headers for stdin integration */
#include "../../deps/loopy/src/loopyStream.h"
#include "../../deps/loopy/src/loopyTTY.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Configuration
 * ============================================================================
 */

typedef struct {
    /* Node identity */
    uint64_t nodeId; /* 0 = auto-assign */
    uint64_t clusterId;

    /* Networking */
    char listenAddr[64];
    int listenPort;

    /* Cluster configuration */
    char *joinAddr; /* For dynamic join */
    int joinPort;
    char *clusterAddrs; /* Comma-separated list for static bootstrap */

    /* Storage */
    char *dataDir;

    /* Raft tuning */
    uint32_t electionTimeoutMinMs;
    uint32_t electionTimeoutMaxMs;
    uint32_t heartbeatIntervalMs;

    /* Features */
    bool preVoteEnabled;
    bool readIndexEnabled;
    bool verbose;
    bool daemonMode; /* If true, disable stdin reading */

    /* Client API */
    int clientPort; /* 0 = disabled */
} CliConfig;

/* Global handles for signal handler */
static loopyServer *g_server = NULL;
static loopyKV *g_kv = NULL;
static loopyClientAPI *g_clientAPI = NULL;

/* Stdin handling for interactive commands
 *
 * Line buffer is dynamically allocated to handle commands of any length
 * up to LINE_BUFFER_MAX_SIZE. Buffer starts at LINE_BUFFER_INITIAL_SIZE
 * and grows as needed via realloc.
 */
#define LINE_BUFFER_INITIAL_SIZE 4096
#define LINE_BUFFER_MAX_SIZE 65536

static loopyTTY *g_stdinTTY = NULL;
static char *g_lineBuffer = NULL; /* Dynamically allocated */
static size_t g_lineLen = 0;      /* Current data length */
static size_t g_lineCapacity = 0; /* Current buffer capacity */
static CliConfig *g_cfg = NULL;   /* For stdin callback context */

/* ============================================================================
 * Argument Parsing
 * ============================================================================
 */

static void printUsage(const char *progname) {
    fprintf(stderr, "Kraft Loopy Demo - Raft Consensus Server\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s [options]\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Required:\n");
    fprintf(stderr, "  --listen ADDR:PORT     Address and port to listen on\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Node Identity:\n");
    fprintf(stderr,
            "  --node ID              Node ID (default: auto from port)\n");
    fprintf(stderr, "  --cluster-id ID        Cluster ID (default: 1)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Cluster Configuration (choose one):\n");
    fprintf(stderr,
            "  --join ADDR:PORT       Join existing cluster at seed node\n");
    fprintf(stderr, "  --cluster ADDRS        Bootstrap static cluster "
                    "(comma-separated)\n");
    fprintf(
        stderr,
        "                         Example: 127.0.0.1:5001,127.0.0.1:5002\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Storage:\n");
    fprintf(stderr,
            "  --data DIR             Data directory for persistence\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Client API:\n");
    fprintf(stderr, "  --client PORT          Enable client API on PORT\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Raft Tuning:\n");
    fprintf(stderr,
            "  --election-min MS      Min election timeout (default: 150)\n");
    fprintf(stderr,
            "  --election-max MS      Max election timeout (default: 300)\n");
    fprintf(stderr,
            "  --heartbeat MS         Heartbeat interval (default: 50)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Features:\n");
    fprintf(stderr, "  --no-prevote           Disable PreVote extension\n");
    fprintf(stderr,
            "  --no-readindex         Disable ReadIndex optimization\n");
    fprintf(stderr, "  --daemon               Daemon mode (disable stdin)\n");
    fprintf(stderr, "  -v, --verbose          Verbose output\n");
    fprintf(stderr, "  -h, --help             Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  # Single-node development mode\n");
    fprintf(stderr, "  %s --listen 127.0.0.1:5001\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "  # 3-node static cluster\n");
    fprintf(stderr, "  %s --node 1 --listen 127.0.0.1:5001 \\\n", progname);
    fprintf(stderr,
            "      --cluster 127.0.0.1:5001,127.0.0.1:5002,127.0.0.1:5003\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  # Join existing cluster\n");
    fprintf(stderr, "  %s --listen 127.0.0.1:5004 --join 127.0.0.1:5001\n",
            progname);
}

static bool parseAddrPort(const char *str, char *addr, size_t addrLen,
                          int *port) {
    const char *colon = strrchr(str, ':');
    if (!colon) {
        return false;
    }

    size_t hostLen = (size_t)(colon - str);
    if (hostLen >= addrLen) {
        return false;
    }

    memcpy(addr, str, hostLen);
    addr[hostLen] = '\0';

    char *endptr;
    long p = strtol(colon + 1, &endptr, 10);
    if (*endptr != '\0' || p <= 0 || p > 65535) {
        return false;
    }

    *port = (int)p;
    return true;
}

static bool parseArgs(int argc, char **argv, CliConfig *cfg) {
    /* Defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->clusterId = 1;
    cfg->electionTimeoutMinMs = 150;
    cfg->electionTimeoutMaxMs = 300;
    cfg->heartbeatIntervalMs = 50;
    cfg->preVoteEnabled = true;
    cfg->readIndexEnabled = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else if (strcmp(argv[i], "--listen") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --listen requires ADDR:PORT\n");
                return false;
            }
            if (!parseAddrPort(argv[i], cfg->listenAddr,
                               sizeof(cfg->listenAddr), &cfg->listenPort)) {
                fprintf(stderr, "Error: Invalid address format: %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--node") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --node requires ID\n");
                return false;
            }
            char *endptr;
            cfg->nodeId = strtoull(argv[i], &endptr, 10);
            if (*endptr != '\0' || cfg->nodeId == 0) {
                fprintf(stderr, "Error: Invalid node ID: %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--cluster-id") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cluster-id requires ID\n");
                return false;
            }
            char *endptr;
            cfg->clusterId = strtoull(argv[i], &endptr, 10);
            if (*endptr != '\0') {
                fprintf(stderr, "Error: Invalid cluster ID: %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--join") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --join requires ADDR:PORT\n");
                return false;
            }
            cfg->joinAddr = malloc(64);
            if (!parseAddrPort(argv[i], cfg->joinAddr, 64, &cfg->joinPort)) {
                fprintf(stderr, "Error: Invalid join address: %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--cluster") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cluster requires comma-separated "
                                "ADDR:PORT list\n");
                return false;
            }
            cfg->clusterAddrs = strdup(argv[i]);
        } else if (strcmp(argv[i], "--data") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --data requires DIR\n");
                return false;
            }
            cfg->dataDir = strdup(argv[i]);
        } else if (strcmp(argv[i], "--client") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --client requires PORT\n");
                return false;
            }
            char *endptr;
            long p = strtol(argv[i], &endptr, 10);
            if (*endptr != '\0' || p <= 0 || p > 65535) {
                fprintf(stderr, "Error: Invalid client port: %s\n", argv[i]);
                return false;
            }
            cfg->clientPort = (int)p;
        } else if (strcmp(argv[i], "--election-min") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --election-min requires MS\n");
                return false;
            }
            cfg->electionTimeoutMinMs = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--election-max") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --election-max requires MS\n");
                return false;
            }
            cfg->electionTimeoutMaxMs = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--heartbeat") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --heartbeat requires MS\n");
                return false;
            }
            cfg->heartbeatIntervalMs = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--no-prevote") == 0) {
            cfg->preVoteEnabled = false;
        } else if (strcmp(argv[i], "--no-readindex") == 0) {
            cfg->readIndexEnabled = false;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            cfg->daemonMode = true;
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    /* Validate required options */
    if (cfg->listenPort == 0) {
        fprintf(stderr, "Error: --listen is required\n");
        return false;
    }

    /* Auto-assign node ID from port if not specified */
    if (cfg->nodeId == 0) {
        cfg->nodeId = (uint64_t)cfg->listenPort;
    }

    /* Validate timing */
    if (cfg->electionTimeoutMinMs >= cfg->electionTimeoutMaxMs) {
        fprintf(stderr, "Error: election-min must be less than election-max\n");
        return false;
    }
    if (cfg->heartbeatIntervalMs >= cfg->electionTimeoutMinMs) {
        fprintf(stderr,
                "Warning: heartbeat should be less than election-min\n");
    }

    return true;
}

static void freeConfig(CliConfig *cfg) {
    free(cfg->joinAddr);
    free(cfg->clusterAddrs);
    free(cfg->dataDir);
}

/* ============================================================================
 * KV Change Callback (for live data flow display)
 * ============================================================================
 */

static void kvChangeCallback(loopyKV *kv, loopyKVCmd cmd, const char *key,
                             size_t keyLen, const char *value, size_t valueLen,
                             void *userData) {
    CliConfig *cfg = (CliConfig *)userData;
    (void)kv;

    if (!cfg->verbose) {
        return;
    }

    /* Show live data flow */
    printf("\033[36m[DATA]\033[0m %s ", loopyKVCmdName(cmd));
    printf("%.*s", (int)keyLen, key);
    if (value && valueLen > 0) {
        printf(" = %.*s", (int)(valueLen > 50 ? 50 : valueLen), value);
        if (valueLen > 50) {
            printf("...");
        }
    }
    printf("\n");
}

/* ============================================================================
 * Server Callbacks
 * ============================================================================
 */

static void serverCallback(loopyServer *server, loopyServerEvent event,
                           const void *data, void *userData) {
    CliConfig *cfg = (CliConfig *)userData;
    (void)data;

    switch (event) {
    case LOOPY_SERVER_EVENT_STARTED:
        printf("\033[32m[NODE]\033[0m Kraft node %lu started\n",
               (unsigned long)loopyServerGetNodeId(server));
        printf("  Cluster:   %lu\n",
               (unsigned long)loopyServerGetClusterId(server));
        printf("  Listen:    %s:%d\n", cfg->listenAddr, cfg->listenPort);
        printf("  PreVote:   %s\n",
               cfg->preVoteEnabled ? "enabled" : "disabled");
        printf("  ReadIndex: %s\n",
               cfg->readIndexEnabled ? "enabled" : "disabled");
        if (cfg->verbose) {
            printf("  Election timeout: %u-%u ms\n", cfg->electionTimeoutMinMs,
                   cfg->electionTimeoutMaxMs);
            printf("  Heartbeat: %u ms\n", cfg->heartbeatIntervalMs);
        }
        printf("\n");
        printf("Commands: SET key value | GET key | DEL key | INCR key | STAT "
               "| STATUS | QUIT\n");
        printf("\n");
        /* Print initial prompt for interactive mode */
        if (g_stdinTTY) {
            printf("> ");
            fflush(stdout);
        }
        break;

    case LOOPY_SERVER_EVENT_STOPPED:
        printf("\033[32m[NODE]\033[0m Node stopped\n");
        break;

    case LOOPY_SERVER_EVENT_ROLE_CHANGED: {
        const char *roleNames[] = {"FOLLOWER", "CANDIDATE", "LEADER"};
        loopyServerRole role = loopyServerGetRole(server);
        printf("\033[33m[RAFT]\033[0m Role: %s\n", roleNames[role]);
    } break;

    case LOOPY_SERVER_EVENT_LEADER_CHANGED: {
        uint64_t leaderId = loopyServerGetLeaderId(server);
        if (leaderId == loopyServerGetNodeId(server)) {
            printf("\033[33m[RAFT]\033[0m \033[1mBecame LEADER\033[0m (term "
                   "%lu)\n",
                   (unsigned long)loopyServerGetTerm(server));
        } else if (leaderId > 0) {
            printf("\033[33m[RAFT]\033[0m Leader is node %lu\n",
                   (unsigned long)leaderId);
        } else {
            printf("\033[33m[RAFT]\033[0m Leader unknown (election in "
                   "progress)\n");
        }
    } break;

    case LOOPY_SERVER_EVENT_PEER_ADDED:
        printf("\033[35m[PEER]\033[0m Peer connected\n");
        break;

    case LOOPY_SERVER_EVENT_PEER_REMOVED:
        printf("\033[35m[PEER]\033[0m Peer disconnected\n");
        break;

    case LOOPY_SERVER_EVENT_COMMAND_APPLIED:
        if (cfg->verbose) {
            printf("\033[34m[LOG]\033[0m Command applied at index %lu\n",
                   (unsigned long)loopyServerGetLastApplied(server));
        }
        break;
    }
}

/* ============================================================================
 * Signal Handling
 * ============================================================================
 */

static void signalHandler(int sig) {
    const char *signame = "UNKNOWN";
    if (sig == SIGINT) {
        signame = "SIGINT";
    } else if (sig == SIGTERM) {
        signame = "SIGTERM";
    } else if (sig == SIGHUP) {
        signame = "SIGHUP";
    }

    printf("\nShutting down (signal %d: %s)...\n", sig, signame);
    if (g_server) {
        loopyServerStop(g_server);
    }
}

static void setupSignals(void) {
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* In daemon mode, ignore SIGHUP (sent when parent shell exits) */
    if (g_cfg && g_cfg->daemonMode) {
        signal(SIGHUP, SIG_IGN);
    }
}

/* ============================================================================
 * Stdin Integration (loopy event loop)
 * ============================================================================
 */

/* Forward declaration */
static bool processCommand(loopyServer *server, loopyKV *kv, const char *line,
                           size_t lineLen);

/**
 * stdinAllocCallback - Provide buffer space for stdin reads
 *
 * Dynamically grows the line buffer if needed to accommodate more input.
 * The buffer starts at LINE_BUFFER_INITIAL_SIZE and can grow up to
 * LINE_BUFFER_MAX_SIZE.
 */
static void stdinAllocCallback(loopyStream *stream, size_t suggested,
                               void **buf, size_t *bufLen, void *userData) {
    (void)stream;
    (void)suggested;
    (void)userData;

    /* Ensure buffer is allocated */
    if (!g_lineBuffer) {
        g_lineBuffer = malloc(LINE_BUFFER_INITIAL_SIZE);
        if (!g_lineBuffer) {
            *buf = NULL;
            *bufLen = 0;
            return;
        }
        g_lineCapacity = LINE_BUFFER_INITIAL_SIZE;
        g_lineLen = 0;
    }

    /* Check if we need to grow the buffer */
    size_t remaining = g_lineCapacity - g_lineLen - 1; /* -1 for null */
    if (remaining < 256 && g_lineCapacity < LINE_BUFFER_MAX_SIZE) {
        size_t newCapacity = g_lineCapacity * 2;
        if (newCapacity > LINE_BUFFER_MAX_SIZE) {
            newCapacity = LINE_BUFFER_MAX_SIZE;
        }
        char *newBuffer = realloc(g_lineBuffer, newCapacity);
        if (newBuffer) {
            g_lineBuffer = newBuffer;
            g_lineCapacity = newCapacity;
            remaining = newCapacity - g_lineLen - 1;
        }
    }

    /* Provide space in line buffer for new data */
    *buf = g_lineBuffer + g_lineLen;
    *bufLen = remaining;
}

static void stdinReadCallback(loopyStream *stream, ssize_t nread,
                              const void *buf, void *userData) {
    (void)stream;
    (void)buf;
    (void)userData;

    if (nread <= 0) {
        if (nread < 0) {
            /* EOF or error - user closed stdin (e.g., Ctrl-D) */
            printf(
                "\nShutting down (stdin EOF/error, nread=%ld, errno=%d)...\n",
                (long)nread, errno);
            if (g_server) {
                loopyServerStop(g_server);
            }
        } else {
            /* nread == 0, just EOF */
            printf("\n[DEBUG] stdin EOF (nread=0), shutting down...\n");
            if (g_server) {
                loopyServerStop(g_server);
            }
        }
        return;
    }

    /* Data was read directly into g_lineBuffer at offset g_lineLen */
    g_lineLen += (size_t)nread;
    g_lineBuffer[g_lineLen] = '\0';

    /* Process complete lines */
    char *lineStart = g_lineBuffer;
    char *newline;

    while ((newline = strchr(lineStart, '\n')) != NULL) {
        *newline = '\0';
        size_t lineLen = (size_t)(newline - lineStart);

        /* Strip trailing CR if present (Windows line endings) */
        if (lineLen > 0 && lineStart[lineLen - 1] == '\r') {
            lineStart[--lineLen] = '\0';
        }

        /* Process the command */
        if (!processCommand(g_server, g_kv, lineStart, lineLen)) {
            return; /* processCommand returned false = shutdown */
        }

        /* Print prompt */
        printf("> ");
        fflush(stdout);

        lineStart = newline + 1;
    }

    /* Move any remaining partial line to start of buffer */
    if (lineStart != g_lineBuffer) {
        size_t remaining = g_lineLen - (size_t)(lineStart - g_lineBuffer);
        if (remaining > 0) {
            memmove(g_lineBuffer, lineStart, remaining);
        }
        g_lineLen = remaining;
    }

    /* Check for buffer overflow */
    if (g_lineCapacity > 0 && g_lineLen >= g_lineCapacity - 1) {
        printf("\nLine too long (max %zu bytes), clearing buffer\n",
               (size_t)LINE_BUFFER_MAX_SIZE);
        g_lineLen = 0;
        printf("> ");
        fflush(stdout);
    }
}

static bool setupStdinReading(loopyServer *server) {
    /* Get the event loop from the server */
    loopyPlatform *platform = loopyServerGetPlatform(server);
    if (!platform) {
        return false;
    }

    loopyLoop *loop = loopyPlatformGetLoop(platform);
    if (!loop) {
        return false;
    }

    /* Check if stdin is a TTY */
    if (!loopyTTYIsTTY(STDIN_FILENO)) {
        /* Not a TTY - don't set up interactive input */
        return true; /* Not an error, just skip */
    }

    /* Create TTY for stdin */
    g_stdinTTY = loopyTTYStdin(loop);
    if (!g_stdinTTY) {
        fprintf(stderr, "Warning: Could not create stdin TTY\n");
        return true; /* Non-fatal */
    }

    /* Get stream and start reading */
    loopyStream *stdinStream = loopyTTYAsStream(g_stdinTTY);
    if (!stdinStream) {
        loopyTTYFree(g_stdinTTY);
        g_stdinTTY = NULL;
        return true; /* Non-fatal */
    }

    if (!loopyStreamReadStart(stdinStream, stdinAllocCallback,
                              stdinReadCallback, NULL)) {
        fprintf(stderr, "Warning: Could not start stdin reading\n");
        loopyTTYFree(g_stdinTTY);
        g_stdinTTY = NULL;
        return true; /* Non-fatal */
    }

    return true;
}

/**
 * cleanupStdinReading - Clean up stdin resources
 *
 * Frees the dynamic line buffer and TTY resources.
 */
static void cleanupStdinReading(void) {
    if (g_stdinTTY) {
        loopyTTYResetMode(g_stdinTTY);
        loopyTTYFree(g_stdinTTY);
        g_stdinTTY = NULL;
    }

    /* Free dynamic line buffer */
    free(g_lineBuffer);
    g_lineBuffer = NULL;
    g_lineLen = 0;
    g_lineCapacity = 0;
}

/* ============================================================================
 * Cluster Bootstrap
 * ============================================================================
 */

static bool parseClusterPeers(loopyServerConfig *serverCfg, const char *addrs,
                              uint64_t selfNodeId) {
    if (!addrs || !*addrs) {
        return true; /* No peers */
    }

    /* Parse comma-separated list of ADDR:PORT */
    char *copy = strdup(addrs);
    char *saveptr;
    char *token = strtok_r(copy, ",", &saveptr);

    while (token) {
        char addr[64];
        int port;

        if (!parseAddrPort(token, addr, sizeof(addr), &port)) {
            fprintf(stderr, "Error: Invalid peer address: %s\n", token);
            free(copy);
            return false;
        }

        /* Use port as node ID, skip self */
        uint64_t peerNodeId = (uint64_t)port;
        if (peerNodeId != selfNodeId) {
            if (!loopyServerConfigAddPeer(serverCfg, peerNodeId, addr, port)) {
                fprintf(stderr, "Error: Failed to add peer %s:%d\n", addr,
                        port);
                free(copy);
                return false;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return true;
}

/* ============================================================================
 * Interactive Command Processing
 * ============================================================================
 */

static void printStatus(loopyServer *server, loopyKV *kv) {
    const char *roleNames[] = {"FOLLOWER", "CANDIDATE", "LEADER"};

    printf("\n=== Raft Status ===\n");
    printf("  Node ID:      %lu\n",
           (unsigned long)loopyServerGetNodeId(server));
    printf("  Cluster ID:   %lu\n",
           (unsigned long)loopyServerGetClusterId(server));
    printf("  Role:         %s\n", roleNames[loopyServerGetRole(server)]);
    printf("  Term:         %lu\n", (unsigned long)loopyServerGetTerm(server));
    printf("  Leader:       %lu\n",
           (unsigned long)loopyServerGetLeaderId(server));
    printf("  Commit Index: %lu\n",
           (unsigned long)loopyServerGetCommitIndex(server));
    printf("  Last Applied: %lu\n",
           (unsigned long)loopyServerGetLastApplied(server));

    if (kv) {
        loopyKVStats kvStats;
        loopyKVGetStats(kv, &kvStats);
        printf("\n=== KV Store ===\n");
        printf("  Keys:         %lu\n", (unsigned long)kvStats.keyCount);
        printf("  Memory:       %lu bytes\n",
               (unsigned long)kvStats.memoryUsed);
        printf("  Sets:         %lu\n", (unsigned long)kvStats.setsTotal);
        printf("  Gets:         %lu (hits: %lu, misses: %lu)\n",
               (unsigned long)kvStats.getsTotal,
               (unsigned long)kvStats.hitsTotal,
               (unsigned long)kvStats.missesTotal);

        printf("\n=== Snapshots ===\n");
        printf("  Created:      %lu\n",
               (unsigned long)kvStats.snapshotsCreated);
        printf("  Restored:     %lu\n",
               (unsigned long)kvStats.snapshotsRestored);
    }
    printf("\n");
}

static void commandResultCallback(loopyServer *server, uint64_t index,
                                  bool success, const void *result,
                                  size_t resultLen, void *userData) {
    (void)server;
    (void)index;
    (void)userData;

    if (!success) {
        printf("\033[31mERROR\033[0m: Not leader (redirect to leader)\n");
        return;
    }

    if (result && resultLen > 0) {
        char *text = NULL;
        if (loopyKVFormatResponse(result, resultLen, &text)) {
            printf("%s\n", text);
            free(text);
        }
    } else {
        printf("OK\n");
    }
}

static bool processCommand(loopyServer *server, loopyKV *kv, const char *line,
                           size_t lineLen) {
    /* Skip empty lines */
    while (lineLen > 0 && (line[0] == ' ' || line[0] == '\t')) {
        line++;
        lineLen--;
    }
    if (lineLen == 0 || line[0] == '#') {
        return true; /* Continue */
    }

    /* Parse command name */
    char cmd[16] = {0};
    size_t i = 0;
    while (i < lineLen && i < sizeof(cmd) - 1 && line[i] != ' ' &&
           line[i] != '\t') {
        cmd[i] = (char)toupper((unsigned char)line[i]);
        i++;
    }

    /* Built-in commands */
    if (strcmp(cmd, "QUIT") == 0 || strcmp(cmd, "EXIT") == 0) {
        printf("Shutting down...\n");
        loopyServerStop(server);
        return false;
    }

    if (strcmp(cmd, "STATUS") == 0) {
        printStatus(server, kv);
        return true;
    }

    if (strcmp(cmd, "HELP") == 0) {
        printf("\nCommands:\n");
        printf("  SET key value    - Store a key-value pair\n");
        printf("  GET key          - Retrieve a value\n");
        printf("  DEL key          - Delete a key\n");
        printf("  INCR key [n]     - Increment numeric value\n");
        printf("  DECR key [n]     - Decrement numeric value\n");
        printf("  STAT             - Show KV store statistics\n");
        printf("  STATUS           - Show Raft cluster status\n");
        printf("  SNAPSHOT         - Create a snapshot (demo)\n");
        printf("  TRANSFER <id>    - Transfer leadership to node\n");
        printf("  ADD <id> <addr>  - Add node to cluster\n");
        printf("  REMOVE <id>      - Remove node from cluster\n");
        printf("  DUMP             - Dump all keys (local)\n");
        printf("  QUIT             - Shutdown the node\n");
        printf("\n");
        return true;
    }

    if (strcmp(cmd, "DUMP") == 0) {
        loopyKVDump(kv);
        return true;
    }

    if (strcmp(cmd, "SNAPSHOT") == 0) {
        /* Manually trigger a snapshot (for demonstration) */
        loopyStateMachine *sm = loopyKVGetStateMachine(kv);
        if (sm && sm->snapshot) {
            void *snapData = NULL;
            size_t snapLen = 0;
            uint64_t lastIndex = 0;
            uint64_t lastTerm = 0;

            if (sm->snapshot(sm, &snapData, &snapLen, &lastIndex, &lastTerm)) {
                printf("\033[32mSNAPSHOT\033[0m: Created snapshot\n");
                printf("  Size:       %lu bytes\n", (unsigned long)snapLen);
                printf("  Last Index: %lu\n", (unsigned long)lastIndex);
                printf("  Last Term:  %lu\n", (unsigned long)lastTerm);

                /* In a real system, this would be persisted to storage */
                free(snapData);
            } else {
                printf("\033[31mERROR\033[0m: Failed to create snapshot\n");
            }
        } else {
            printf("\033[31mERROR\033[0m: Snapshot not available\n");
        }
        return true;
    }

    if (strcmp(cmd, "TRANSFER") == 0) {
        /* Leadership transfer command */
        const char *arg = line + 8;
        while (*arg == ' ') {
            arg++;
        }

        if (*arg == '\0') {
            printf("\033[31mERROR\033[0m: Usage: TRANSFER <node-id>\n");
            return true;
        }

        uint64_t targetNodeId = strtoull(arg, NULL, 10);
        if (targetNodeId == 0) {
            printf("\033[31mERROR\033[0m: Invalid node ID\n");
            return true;
        }

        if (!loopyServerIsLeader(server)) {
            printf("\033[31mERROR\033[0m: Not the leader - cannot transfer "
                   "leadership\n");
            return true;
        }

        printf("\033[33mTRANSFER\033[0m: Initiating leadership transfer to "
               "node %lu\n",
               (unsigned long)targetNodeId);

        if (loopyServerTransferLeadership(server, targetNodeId)) {
            printf("\033[32mOK\033[0m: Leadership transfer initiated\n");
        } else {
            printf("\033[33mNOTE\033[0m: Leadership transfer not yet fully "
                   "integrated with kraft\n");
            printf("  (This demonstrates the command API for when kraft "
                   "integration is complete)\n");
        }
        return true;
    }

    if (strcmp(cmd, "ADD") == 0) {
        /* Add node to cluster: ADD <node-id> <addr:port> */
        const char *arg = line + 3;
        while (*arg == ' ') {
            arg++;
        }

        if (*arg == '\0') {
            printf("\033[31mERROR\033[0m: Usage: ADD <node-id> <addr:port>\n");
            printf("  Example: ADD 4 127.0.0.1:5004\n");
            return true;
        }

        /* Parse node ID */
        char *endptr = NULL;
        uint64_t nodeId = strtoull(arg, &endptr, 10);
        if (nodeId == 0 || !endptr || *endptr != ' ') {
            printf("\033[31mERROR\033[0m: Invalid node ID\n");
            return true;
        }

        /* Parse address:port */
        while (*endptr == ' ') {
            endptr++;
        }
        char addr[64];
        int port = 0;

        /* Parse addr:port format */
        char *colon = strchr(endptr, ':');
        if (!colon) {
            printf("\033[31mERROR\033[0m: Invalid address format (use "
                   "addr:port)\n");
            return true;
        }

        size_t addrLen = colon - endptr;
        if (addrLen >= sizeof(addr)) {
            addrLen = sizeof(addr) - 1;
        }
        strncpy(addr, endptr, addrLen);
        addr[addrLen] = '\0';
        port = atoi(colon + 1);

        if (port <= 0) {
            printf("\033[31mERROR\033[0m: Invalid port number\n");
            return true;
        }

        printf("\033[33mADD\033[0m: Adding node %lu at %s:%d\n",
               (unsigned long)nodeId, addr, port);

        if (loopyServerAddNode(server, nodeId, addr, port)) {
            printf("\033[32mOK\033[0m: Node connection initiated\n");
        } else {
            printf(
                "\033[31mERROR\033[0m: Failed to add node (must be leader)\n");
        }
        return true;
    }

    if (strcmp(cmd, "REMOVE") == 0) {
        /* Remove node from cluster: REMOVE <node-id> */
        const char *arg = line + 6;
        while (*arg == ' ') {
            arg++;
        }

        if (*arg == '\0') {
            printf("\033[31mERROR\033[0m: Usage: REMOVE <node-id>\n");
            return true;
        }

        uint64_t nodeId = strtoull(arg, NULL, 10);
        if (nodeId == 0) {
            printf("\033[31mERROR\033[0m: Invalid node ID\n");
            return true;
        }

        printf("\033[33mREMOVE\033[0m: Removing node %lu from cluster\n",
               (unsigned long)nodeId);

        if (loopyServerRemoveNode(server, nodeId)) {
            printf("\033[32mOK\033[0m: Node removed\n");
        } else {
            printf("\033[31mERROR\033[0m: Failed to remove node (must be "
                   "leader)\n");
        }
        return true;
    }

    /* KV commands - encode and submit */
    void *data = NULL;
    size_t dataLen = 0;

    if (!loopyKVParseCommand(line, lineLen, &data, &dataLen)) {
        printf("\033[31mERROR\033[0m: Unknown command. Type HELP for usage.\n");
        return true;
    }

    /* Check if this is a read (GET) - can be handled locally */
    if (((uint8_t *)data)[0] == LOOPY_KV_CMD_GET) {
        /* Local read for now (not linearizable without ReadIndex) */
        void *result = NULL;
        size_t resultLen = 0;
        loopyStateMachine *sm = loopyKVGetStateMachine(kv);
        if (sm && sm->apply) {
            sm->apply(sm, 0, data, dataLen, &result, &resultLen);
            if (result) {
                char *text = NULL;
                if (loopyKVFormatResponse(result, resultLen, &text)) {
                    printf("%s\n", text);
                    free(text);
                }
                free(result);
            }
        }
        free(data);
        return true;
    }

    /* Write commands go through Raft */
    uint64_t idx =
        loopyServerSubmit(server, data, dataLen, commandResultCallback, NULL);
    if (idx == 0) {
        printf("\033[31mERROR\033[0m: Command rejected (not leader?)\n");
    }
    free(data);

    return true;
}

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(int argc, char **argv) {
    CliConfig cfg;
    int exitCode = 0;

    /* Parse arguments */
    if (!parseArgs(argc, argv, &cfg)) {
        printUsage(argv[0]);
        return 1;
    }

    /* Save config for signal handler */
    g_cfg = &cfg;

    /* Setup signal handlers (needs g_cfg for daemon mode check) */
    setupSignals();

    /* Create KV store */
    g_kv = loopyKVCreate();
    if (!g_kv) {
        fprintf(stderr, "Error: Failed to create KV store\n");
        freeConfig(&cfg);
        return 1;
    }

    /* Setup change callback for live display */
    loopyKVSetChangeCallback(g_kv, kvChangeCallback, &cfg);

    /* Create server configuration */
    loopyServerConfig serverCfg;
    loopyServerConfigInit(&serverCfg);

    serverCfg.nodeId = cfg.nodeId;
    serverCfg.clusterId = cfg.clusterId;
    serverCfg.listenAddr = cfg.listenAddr;
    serverCfg.listenPort = cfg.listenPort;
    serverCfg.electionTimeoutMinMs = cfg.electionTimeoutMinMs;
    serverCfg.electionTimeoutMaxMs = cfg.electionTimeoutMaxMs;
    serverCfg.heartbeatIntervalMs = cfg.heartbeatIntervalMs;
    serverCfg.preVoteEnabled = cfg.preVoteEnabled;
    serverCfg.readIndexEnabled = cfg.readIndexEnabled;
    serverCfg.callback = serverCallback;
    serverCfg.userData = &cfg;

    /* Add cluster peers if specified */
    if (cfg.clusterAddrs) {
        if (!parseClusterPeers(&serverCfg, cfg.clusterAddrs, cfg.nodeId)) {
            loopyKVDestroy(g_kv);
            freeConfig(&cfg);
            return 1;
        }
    }

    /* Create server */
    if (cfg.clusterAddrs == NULL && cfg.joinAddr == NULL) {
        /* Single-node mode */
        if (cfg.verbose) {
            printf("Starting in single-node mode\n");
        }
        g_server =
            loopyServerCreateSingle(cfg.nodeId, cfg.listenAddr, cfg.listenPort);
    } else {
        /* Cluster mode */
        g_server = loopyServerCreate(&serverCfg);
    }

    loopyServerConfigCleanup(&serverCfg);

    if (!g_server) {
        fprintf(stderr, "Error: Failed to create server\n");
        loopyKVDestroy(g_kv);
        freeConfig(&cfg);
        return 1;
    }

    /* Set callback (must be done after creation for single-node mode) */
    loopyServerSetCallback(g_server, serverCallback, &cfg);

    /* Connect KV state machine to server */
    loopyServerSetStateMachine(g_server, loopyKVGetStateMachine(g_kv));

    /* Handle join request if specified */
    if (cfg.joinAddr) {
        /* TODO: Implement dynamic join via loopyCluster */
        fprintf(stderr,
                "Warning: --join not yet implemented, starting standalone\n");
    }

    /* Setup stdin reading for interactive commands (unless in daemon mode) */
    if (!cfg.daemonMode) {
        if (!setupStdinReading(g_server)) {
            fprintf(stderr, "Error: Failed to setup stdin reading\n");
            loopyServerDestroy(g_server);
            loopyKVDestroy(g_kv);
            freeConfig(&cfg);
            return 1;
        }
    } else {
        printf("[DAEMON] Stdin reading disabled (daemon mode)\n");
    }

    /* Setup client API if requested */
    if (cfg.clientPort > 0) {
        loopyPlatform *platform = loopyServerGetPlatform(g_server);
        if (platform) {
            loopyLoop *loop = loopyPlatformGetLoop(platform);
            g_clientAPI =
                loopyClientAPICreate(loop, cfg.clientPort, g_server, g_kv);
            if (g_clientAPI) {
                if (loopyClientAPIStart(g_clientAPI) == 0) {
                    printf("Client API listening on port %d\n", cfg.clientPort);
                } else {
                    fprintf(stderr,
                            "Warning: Failed to start client API on port %d\n",
                            cfg.clientPort);
                    loopyClientAPIDestroy(g_clientAPI);
                    g_clientAPI = NULL;
                }
            } else {
                fprintf(stderr, "Warning: Failed to create client API\n");
            }
        }
    }

    /* Run server (blocks until shutdown) */
    printf("\033[1mKraft Loopy Demo\033[0m - Node %lu\n",
           (unsigned long)cfg.nodeId);
    printf("================\n\n");

    int result = loopyServerRun(g_server);
    if (result != 0) {
        fprintf(stderr, "Error: Server exited with error %d\n", result);
        exitCode = 1;
    }

    /* Cleanup */
    cleanupStdinReading();
    if (g_clientAPI) {
        loopyClientAPIDestroy(g_clientAPI);
        g_clientAPI = NULL;
    }
    loopyServerDestroy(g_server);
    g_server = NULL;
    loopyKVDestroy(g_kv);
    g_kv = NULL;
    g_cfg = NULL;
    freeConfig(&cfg);

    return exitCode;
}
