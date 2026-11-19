/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2025-2026 OpenSensor Engineering and LightNVR contributors
 *
 * This file is part of LightNVR <https://github.com/opensensor/lightNVR>.
 */

/**
 * @file go2rtc_integration.c
 * @brief Implementation of the go2rtc integration with existing recording and HLS systems
 *
 * This module also contains the unified health monitor that handles both:
 * - Stream-level health (re-registering individual streams when they fail)
 * - Process-level health (restarting go2rtc when it becomes unresponsive)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "video/go2rtc/go2rtc_integration.h"
#include "video/go2rtc/go2rtc_consumer.h"
#include "video/go2rtc/go2rtc_stream.h"
#include "video/go2rtc/go2rtc_process.h"
#include "video/go2rtc/go2rtc_lifecycle.h"
#include "video/go2rtc/go2rtc_api.h"
#include "core/logger.h"
#include "core/config.h"
#include "core/url_utils.h"
#include "core/shutdown_coordinator.h"  // For is_shutdown_initiated
#include "utils/strings.h"
#include "video/stream_manager.h"
#include "video/mp4_recording.h"
#include "video/hls/hls_api.h"
#include "video/streams.h"
#include "database/db_streams.h"
#include "video/stream_state.h"
#include "video/unified_detection_thread.h"

// Tracking for streams using go2rtc
#define MAX_TRACKED_STREAMS MAX_STREAMS

typedef struct {
    char stream_name[MAX_STREAM_NAME];
    bool using_go2rtc_for_recording;
    bool using_go2rtc_for_hls;
} go2rtc_stream_tracking_t;

// Store original stream URLs for restoration when stopping HLS
typedef struct {
    char stream_name[MAX_STREAM_NAME];
    char original_url[MAX_PATH_LENGTH];
    char original_username[MAX_STREAM_NAME];
    char original_password[MAX_STREAM_NAME];
} original_stream_config_t;

static go2rtc_stream_tracking_t g_tracked_streams[MAX_TRACKED_STREAMS] = {0};
static original_stream_config_t g_original_configs[MAX_TRACKED_STREAMS] = {0};
static bool g_initialized = false;

// ============================================================================
// Unified Health Monitor Configuration
// ============================================================================

// Health check interval in seconds (unified for both stream and process checks)
#define HEALTH_CHECK_INTERVAL_SEC 30

// Stream health: consecutive failures before re-registration
#define STREAM_MAX_CONSECUTIVE_FAILURES 3

// Stream health: cooldown after re-registration (seconds)
#define STREAM_REREGISTRATION_COOLDOWN_SEC 60

// Process health: consecutive API failures before restart
#define PROCESS_MAX_API_FAILURES 3

// Process health: minimum streams for consensus check
#define PROCESS_MIN_STREAMS_FOR_CONSENSUS 2

// Process health: cooldown after restart (seconds)
#define PROCESS_RESTART_COOLDOWN_SEC 120

// Process health: max restarts within window
#define PROCESS_MAX_RESTARTS_PER_WINDOW 5
#define PROCESS_RESTART_WINDOW_SEC 600  // 10 minutes

// Stuck stream detection: consecutive checks with no byte count increase
#define STUCK_STREAM_MAX_STALLED_CHECKS 3
// How long after a connect/reconnect to suppress stuck detection.
// Must cover the go2rtc replay warmup (2 * pre_buffer_seconds, typically
// 20-40s) plus at least one full health-check interval as margin.
// 30 * (3+1) = 120s — safely beyond the ~90s false-positive window.
#define STUCK_STREAM_WARMUP_SEC  (HEALTH_CHECK_INTERVAL_SEC * (STUCK_STREAM_MAX_STALLED_CHECKS + 1))

// Stuck stream tracking per stream
typedef struct {
    char stream_name[MAX_STREAM_NAME];
    int64_t last_bytes_recv;      // Last known bytes received by producer
    int64_t last_bytes_send;      // Last known bytes sent by preload consumer
    int last_producer_count;      // Last known active producer count
    int stalled_checks;           // Consecutive checks with no increase
    time_t last_check_time;       // Time of last check
    time_t tracking_start_time;   // Wall time when tracker was created or last reset
    bool tracking_active;         // Whether we're actively tracking this stream
} stuck_stream_tracker_t;

static stuck_stream_tracker_t g_stuck_trackers[MAX_TRACKED_STREAMS] = {0};
static pthread_mutex_t g_stuck_tracker_mutex = PTHREAD_MUTEX_INITIALIZER;

// Result of one data-flow probe against go2rtc's /api/streams byte counters.
typedef enum {
    STREAM_FLOW_UNKNOWN = 0,  // could not tell (warmup, API error, first sample)
    STREAM_FLOW_MISSING,      // go2rtc does not list the stream at all
    STREAM_FLOW_FLOWING,      // byte counters advanced since the last probe
    STREAM_FLOW_STALLED,      // no advance, below the stuck threshold
    STREAM_FLOW_STUCK,        // no advance for STUCK_STREAM_MAX_STALLED_CHECKS probes
} stream_flow_t;

// ============================================================================
// Per-stream recovery bookkeeping (#620)
// ============================================================================
//
// A scoped recovery (DELETE + PUT of one stream through the go2rtc API) is
// cheap but not free. In the bundled go2rtc a DELETE stops the old Stream
// object, but a PUT that replaces an existing entry does not, and the preload
// registry keeps pointing at whichever object it was attached to; a producer
// blocked on a stalled camera connection can also outlive its Stream. Each
// reload that is not followed by data flow therefore risks leaving another
// producer behind holding an RTSP session on the camera, and in the field only
// a go2rtc restart cleared the resulting persistent 404s (#620). Without
// bookkeeping the monitor re-ran the same reload every 60-90 s for hours.
// These helpers make the loop progress-aware:
//   * the cooldown before the next scoped recovery doubles after every
//     recovery that is not followed by observed data flow (capped);
//   * after STREAM_RECOVERY_ESCALATE_AFTER fruitless recoveries on a stream
//     that was healthy in this go2rtc process lifetime and whose camera is
//     still TCP-reachable from LightNVR, the monitor escalates to a
//     rate-limited go2rtc process restart, the only action that clears
//     orphaned producers inside go2rtc.
// Access to g_recovery_states is serialized by g_recovery_mutex because the
// writer/detection threads feed it through
// go2rtc_integration_report_proxy_open_failure().

#define STREAM_RECOVERY_BACKOFF_MAX_SEC         (15 * 60)
#define STREAM_RECOVERY_ESCALATE_AFTER          3
#define STREAM_RECOVERY_ESCALATION_COOLDOWN_SEC (30 * 60)
#define STREAM_PROXY_OPEN_FAILURE_THRESHOLD     3
#define STREAM_CAMERA_PROBE_TIMEOUT_MS          3000

typedef struct {
    char stream_name[MAX_STREAM_NAME];
    bool active;
    int attempts_since_progress;          // scoped recoveries since data last flowed
    time_t last_attempt_time;             // wall time of the last scoped recovery (any outcome)
    time_t last_escalation_time;          // wall time of the last process-restart escalation
    bool healthy_seen;                    // data flow observed at least once
    uint64_t healthy_restart_generation;  // lifecycle restart generation when data last flowed
    int proxy_open_failures;              // consecutive local-RTSP open failures reported by consumers
} stream_recovery_state_t;

static stream_recovery_state_t g_recovery_states[MAX_TRACKED_STREAMS] = {0};
static pthread_mutex_t g_recovery_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Caller must hold g_recovery_mutex. */
static stream_recovery_state_t *recovery_state_locked(const char *stream_name, bool create) {
    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (g_recovery_states[i].active &&
            strcmp(g_recovery_states[i].stream_name, stream_name) == 0) {
            return &g_recovery_states[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (!g_recovery_states[i].active) {
            memset(&g_recovery_states[i], 0, sizeof(g_recovery_states[i]));
            safe_strcpy(g_recovery_states[i].stream_name, stream_name, MAX_STREAM_NAME, 0);
            g_recovery_states[i].active = true;
            return &g_recovery_states[i];
        }
    }
    return NULL;
}

static time_t recovery_cooldown_sec(int attempts_since_progress) {
    if (attempts_since_progress < 0) attempts_since_progress = 0;
    if (attempts_since_progress > 8) attempts_since_progress = 8;
    time_t cooldown = (time_t)STREAM_REREGISTRATION_COOLDOWN_SEC << attempts_since_progress;
    if (cooldown > STREAM_RECOVERY_BACKOFF_MAX_SEC) {
        cooldown = STREAM_RECOVERY_BACKOFF_MAX_SEC;
    }
    return cooldown;
}

static bool recovery_cooldown_elapsed(const char *stream_name, time_t now) {
    bool elapsed = true;
    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(stream_name, false);
    if (st && st->last_attempt_time > 0) {
        elapsed = (now - st->last_attempt_time) >= recovery_cooldown_sec(st->attempts_since_progress);
    }
    pthread_mutex_unlock(&g_recovery_mutex);
    return elapsed;
}

static void note_recovery_attempt(const char *stream_name) {
    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(stream_name, true);
    if (st) {
        if (st->attempts_since_progress < INT_MAX) st->attempts_since_progress++;
        st->last_attempt_time = time(NULL);
        st->proxy_open_failures = 0;
        log_info("Stream %s: scoped recovery #%d since last data flow; next one no sooner than %lds",
                 stream_name, st->attempts_since_progress,
                 (long)recovery_cooldown_sec(st->attempts_since_progress));
    }
    pthread_mutex_unlock(&g_recovery_mutex);
}

static void note_stream_progress(const char *stream_name) {
    uint64_t restart_generation = go2rtc_lifecycle_restart_generation();
    bool restored_from_disruption = false;

    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(stream_name, true);
    if (st) {
        if (st->attempts_since_progress > 0 || st->proxy_open_failures > 0) {
            restored_from_disruption = true;
            if (st->attempts_since_progress > 0) {
                log_info("Stream %s: data flow restored after %d scoped recover%s",
                         stream_name, st->attempts_since_progress,
                         st->attempts_since_progress == 1 ? "y" : "ies");
            }
        }
        st->attempts_since_progress = 0;
        st->proxy_open_failures = 0;
        st->healthy_seen = true;
        st->healthy_restart_generation = restart_generation;
    }
    pthread_mutex_unlock(&g_recovery_mutex);

    if (restored_from_disruption) {
        log_info("Stream %s: camera reconnected/restored, triggering PTZ default preset", stream_name);
        trigger_ptz_default_preset(stream_name);
    }
}

static void note_escalation(const char *stream_name) {
    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(stream_name, true);
    if (st) {
        st->last_escalation_time = time(NULL);
        st->attempts_since_progress = 0;
        st->proxy_open_failures = 0;
    }
    pthread_mutex_unlock(&g_recovery_mutex);
}

static int recovery_proxy_open_failures(const char *stream_name) {
    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(stream_name, false);
    int failures = st ? st->proxy_open_failures : 0;
    pthread_mutex_unlock(&g_recovery_mutex);
    return failures;
}

/**
 * @brief Extract host and port from a camera URL
 *
 * Accepts scheme://[user[:pass]@]host[:port][/...] (IPv6 hosts in brackets).
 * The default port follows the scheme (rtsp 554, rtsps 322, http 80, https 443).
 */
static bool parse_url_host_port(const char *url, char *host, size_t host_size, int *port) {
    if (!url || !host || host_size == 0 || !port) {
        return false;
    }

    int default_port = 554;
    const char *p = strstr(url, "://");
    if (p) {
        size_t scheme_len = (size_t)(p - url);
        if (scheme_len == 4 && strncasecmp(url, "http", 4) == 0) {
            default_port = 80;
        } else if (scheme_len == 5 && strncasecmp(url, "https", 5) == 0) {
            default_port = 443;
        } else if (scheme_len == 5 && strncasecmp(url, "rtsps", 5) == 0) {
            default_port = 322;
        }
        p += 3;
    } else {
        p = url;
    }

    const char *end = p + strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *q = p; q < end; q++) {
        if (*q == '@') at = q;
    }
    const char *hstart = at ? at + 1 : p;
    const char *hend = end;
    const char *pstart = NULL;

    if (hstart < end && *hstart == '[') {
        const char *rb = memchr(hstart, ']', (size_t)(end - hstart));
        if (!rb) {
            return false;
        }
        hstart++;
        hend = rb;
        if (rb + 1 < end && rb[1] == ':') {
            pstart = rb + 2;
        }
    } else {
        const char *colon = NULL;
        for (const char *q = hstart; q < end; q++) {
            if (*q == ':') colon = q;
        }
        if (colon) {
            hend = colon;
            pstart = colon + 1;
        }
    }

    size_t hlen = (size_t)(hend - hstart);
    if (hlen == 0 || hlen >= host_size) {
        return false;
    }
    memcpy(host, hstart, hlen);
    host[hlen] = '\0';

    *port = default_port;
    if (pstart && pstart < end) {
        int v = atoi(pstart);
        if (v > 0 && v <= 65535) {
            *port = v;
        }
    }
    return true;
}

/**
 * @brief Decide whether scoped recoveries have stopped making progress and a
 *        go2rtc process restart is warranted for this stream.
 *
 * Deliberately conservative: a camera that is offline, or one that never
 * produced data in this go2rtc lifetime (misconfigured), must not restart the
 * shared go2rtc process. Only a stream that *was* healthy, whose camera still
 * accepts TCP connections, and that go2rtc still cannot pull media from after
 * repeated reloads qualifies -- that combination points at state inside go2rtc.
 */
static bool should_escalate_to_process_restart(const stream_config_t *config) {
    if (!config || config->name[0] == '\0') {
        return false;
    }

    time_t now = time(NULL);
    uint64_t current_generation = go2rtc_lifecycle_restart_generation();
    int attempts = 0;
    bool eligible = false;

    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(config->name, false);
    if (st) {
        attempts = st->attempts_since_progress;
        eligible = st->healthy_seen &&
                   st->healthy_restart_generation == current_generation &&
                   st->attempts_since_progress >= STREAM_RECOVERY_ESCALATE_AFTER &&
                   (st->last_escalation_time == 0 ||
                    now - st->last_escalation_time >= STREAM_RECOVERY_ESCALATION_COOLDOWN_SEC);
    }
    pthread_mutex_unlock(&g_recovery_mutex);

    if (!eligible) {
        return false;
    }

    char host[256];
    int port = 0;
    if (!parse_url_host_port(config->url, host, sizeof(host), &port)) {
        return false;
    }

    if (!go2rtc_stream_tcp_port_open(host, port, STREAM_CAMERA_PROBE_TIMEOUT_MS)) {
        return false;
    }

    return true;
}

static pthread_t g_monitor_thread;
static bool g_monitor_running = false;
static bool g_monitor_initialized = false;

static int g_restart_count = 0;
static time_t g_last_restart_time = 0;
static int g_consecutive_api_failures = 0;
static time_t g_restart_history[PROCESS_MAX_RESTARTS_PER_WINDOW];
static int g_restart_history_index = 0;

static bool go2rtc_integration_full_start_locked(void);

// CURL write callback for fetching stream info
static size_t stuck_stream_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    char **buffer = (char **)userp;

    if (*buffer == NULL) {
        *buffer = malloc(realsize + 1);
        if (*buffer == NULL) return 0;
        memcpy(*buffer, contents, realsize);
        (*buffer)[realsize] = '\0';
    } else {
        size_t old_len = strlen(*buffer);
        char *new_buffer = realloc(*buffer, old_len + realsize + 1);
        if (new_buffer == NULL) return 0;
        *buffer = new_buffer;
        memcpy(*buffer + old_len, contents, realsize);
        (*buffer)[old_len + realsize] = '\0';
    }
    return realsize;
}

/**
 * @brief Get stuck stream tracker for a stream, or create one
 */
static stuck_stream_tracker_t *get_or_create_stuck_tracker(const char *stream_name) {
    pthread_mutex_lock(&g_stuck_tracker_mutex);

    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (g_stuck_trackers[i].tracking_active &&
            strcmp(g_stuck_trackers[i].stream_name, stream_name) == 0) {
            pthread_mutex_unlock(&g_stuck_tracker_mutex);
            return &g_stuck_trackers[i];
        }
    }

    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (!g_stuck_trackers[i].tracking_active) {
            memset(&g_stuck_trackers[i], 0, sizeof(stuck_stream_tracker_t));
            safe_strcpy(g_stuck_trackers[i].stream_name, stream_name, MAX_STREAM_NAME, 0);
            g_stuck_trackers[i].tracking_active = true;
            g_stuck_trackers[i].last_bytes_recv = -1;
            g_stuck_trackers[i].last_bytes_send = -1;
            g_stuck_trackers[i].last_producer_count = -1;
            g_stuck_trackers[i].tracking_start_time = time(NULL);
            pthread_mutex_unlock(&g_stuck_tracker_mutex);
            return &g_stuck_trackers[i];
        }
    }

    pthread_mutex_unlock(&g_stuck_tracker_mutex);
    return NULL;
}

/**
 * @brief Reset stuck tracker for a stream (e.g., after reload)
 */
static void reset_stuck_tracker(const char *stream_name) {
    pthread_mutex_lock(&g_stuck_tracker_mutex);
    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (g_stuck_trackers[i].tracking_active &&
            strcmp(g_stuck_trackers[i].stream_name, stream_name) == 0) {
            g_stuck_trackers[i].last_bytes_recv = -1;
            g_stuck_trackers[i].last_bytes_send = -1;
            g_stuck_trackers[i].last_producer_count = -1;
            g_stuck_trackers[i].stalled_checks = 0;
            g_stuck_trackers[i].last_check_time = 0;
            g_stuck_trackers[i].tracking_start_time = time(NULL);
            break;
        }
    }
    pthread_mutex_unlock(&g_stuck_tracker_mutex);
}

/**
 * @brief Check if a stream is stuck by monitoring go2rtc byte counts
 *
 * Returns true if the stream appears to be stuck (no data flow)
 */
static stream_flow_t check_stream_data_flow(const char *stream_name) {
    {
        stuck_stream_tracker_t *t = get_or_create_stuck_tracker(stream_name);
        if (t) {
            pthread_mutex_lock(&g_stuck_tracker_mutex);
            time_t start = t->tracking_start_time;
            pthread_mutex_unlock(&g_stuck_tracker_mutex);
            time_t now_guard = time(NULL);
            if (now_guard - start < (time_t)STUCK_STREAM_WARMUP_SEC) {
                return STREAM_FLOW_UNKNOWN;
            }
        }
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        return STREAM_FLOW_UNKNOWN;
    }

    char url[512];
    int api_port_val = go2rtc_stream_get_api_port();
    if (api_port_val == 0) api_port_val = 1984;

    snprintf(url, sizeof(url), "http://localhost:%d%s/api/streams",
             api_port_val, go2rtc_api_get_base_path());

    char *response = NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stuck_stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !response) {
        if (response) free(response);
        return STREAM_FLOW_UNKNOWN;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);

    if (!root) {
        return STREAM_FLOW_UNKNOWN;
    }

    stuck_stream_tracker_t *tracker = get_or_create_stuck_tracker(stream_name);
    if (!tracker) {
        cJSON_Delete(root);
        return STREAM_FLOW_UNKNOWN;
    }

    cJSON *stream_obj = cJSON_GetObjectItemCaseSensitive(root, stream_name);
    if (!stream_obj && (cJSON_HasObjectItem(root, "producers") ||
                        cJSON_HasObjectItem(root, "consumers"))) {
        stream_obj = root;
    }
    if (!stream_obj) {
        cJSON_Delete(root);
        return STREAM_FLOW_MISSING;
    }

    int64_t total_bytes_recv = 0;
    int64_t total_bytes_send = 0;
    int active_producers = 0;

    cJSON *producers = cJSON_GetObjectItem(stream_obj, "producers");
    if (cJSON_IsArray(producers)) {
        cJSON *producer;
        cJSON_ArrayForEach(producer, producers) {
            cJSON *bytes = cJSON_GetObjectItem(producer, "bytes_recv");
            if (cJSON_IsNumber(bytes)) {
                total_bytes_recv += (int64_t)bytes->valuedouble;
                if (bytes->valuedouble > 0) {
                    active_producers++;
                }
            }
        }
    }

    cJSON *consumers = cJSON_GetObjectItem(stream_obj, "consumers");
    if (cJSON_IsArray(consumers)) {
        cJSON *consumer;
        cJSON_ArrayForEach(consumer, consumers) {
            cJSON *bytes = cJSON_GetObjectItem(consumer, "bytes_send");
            if (cJSON_IsNumber(bytes)) {
                total_bytes_send += (int64_t)bytes->valuedouble;
            }
        }
    }

    cJSON_Delete(root);

    stream_flow_t flow = STREAM_FLOW_UNKNOWN;
    time_t now = time(NULL);
    bool camera_reconnected = false;

    pthread_mutex_lock(&g_stuck_tracker_mutex);

    if (tracker->last_bytes_recv == -1) {
        tracker->last_bytes_recv = total_bytes_recv;
        tracker->last_bytes_send = total_bytes_send;
        tracker->last_producer_count = active_producers;
        tracker->last_check_time = now;
        tracker->stalled_checks = 0;
    } else {
        bool producer_reset = (total_bytes_recv > 0 && total_bytes_recv < tracker->last_bytes_recv);
        bool producer_reconnected = (tracker->last_producer_count == 0 && active_producers > 0) ||
                                    (tracker->last_bytes_recv == 0 && total_bytes_recv > 0);
        bool data_flowing = producer_reset || producer_reconnected ||
                            (total_bytes_recv > tracker->last_bytes_recv) ||
                            (total_bytes_send > tracker->last_bytes_send);

        if (data_flowing) {
            if (tracker->stalled_checks > 0 || producer_reset || producer_reconnected) {
                camera_reconnected = true;
            }
            tracker->stalled_checks = 0;
            flow = STREAM_FLOW_FLOWING;
        } else {
            tracker->stalled_checks++;
            flow = STREAM_FLOW_STALLED;

            if (tracker->stalled_checks >= STUCK_STREAM_MAX_STALLED_CHECKS) {
                flow = STREAM_FLOW_STUCK;
            }
        }

        tracker->last_bytes_recv = total_bytes_recv;
        tracker->last_bytes_send = total_bytes_send;
        tracker->last_producer_count = active_producers;
        tracker->last_check_time = now;
    }

    pthread_mutex_unlock(&g_stuck_tracker_mutex);

    if (camera_reconnected) {
        trigger_ptz_default_preset(stream_name);
    }

    return flow;
}

/**
 * @brief Find a tracked stream by name
 */
static go2rtc_stream_tracking_t *find_tracked_stream(const char *stream_name) {
    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (g_tracked_streams[i].stream_name[0] != '\0' &&
            strcmp(g_tracked_streams[i].stream_name, stream_name) == 0) {
            return &g_tracked_streams[i];
        }
    }
    return NULL;
}

/**
 * @brief Add a new tracked stream
 */
static go2rtc_stream_tracking_t *add_tracked_stream(const char *stream_name) {
    go2rtc_stream_tracking_t *existing = find_tracked_stream(stream_name);
    if (existing) {
        return existing;
    }

    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (g_tracked_streams[i].stream_name[0] == '\0') {
            safe_strcpy(g_tracked_streams[i].stream_name, stream_name, MAX_STREAM_NAME, 0);
            g_tracked_streams[i].using_go2rtc_for_recording = false;
            g_tracked_streams[i].using_go2rtc_for_hls = false;
            return &g_tracked_streams[i];
        }
    }

    return NULL;
}

/**
 * @brief Check if a stream is registered with go2rtc
 */
static bool is_stream_registered_with_go2rtc(const char *stream_name) {
    if (!go2rtc_stream_is_ready()) {
        return false;
    }
    return go2rtc_api_stream_exists(stream_name);
}

/**
 * @brief Register a stream with go2rtc if not already registered
 */
static bool ensure_stream_registered_with_go2rtc(const char *stream_name) {
    if (!go2rtc_stream_is_ready()) {
        if (!go2rtc_stream_start_service()) {
            return false;
        }

        int retries = 10;
        while (retries > 0 && !go2rtc_stream_is_ready()) {
            sleep(1);
            retries--;
        }

        if (!go2rtc_stream_is_ready()) {
            return false;
        }
    }

    stream_handle_t stream = get_stream_by_name(stream_name);
    if (!stream) {
        return false;
    }

    stream_config_t config;
    if (get_stream_config(stream, &config) != 0) {
        return false;
    }

    if (!g_config.audio_disabled && config.go2rtc_source_override[0] != '\0') {
        return go2rtc_integration_restart_process();
    }

    if (is_stream_registered_with_go2rtc(stream_name)) {
        return true;
    }

    return go2rtc_stream_register(stream_name, config.url,
                                  config.onvif_username[0] != '\0' ? config.onvif_username : NULL,
                                  config.onvif_password[0] != '\0' ? config.onvif_password : NULL,
                                  config.backchannel_enabled, config.protocol,
                                  config.record_audio, config.codec);
}

/**
 * @brief Check if a stream needs re-registration with go2rtc
 */
static bool stream_needs_reregistration(const char *stream_name) {
    if (!stream_name) {
        return false;
    }

    stream_state_manager_t *state = get_stream_state_by_name(stream_name);
    if (!state || !state->config.enabled) {
        return false;
    }

    time_t now = time(NULL);
    bool cooldown_elapsed = recovery_cooldown_elapsed(stream_name, now);

    if (state->state == STREAM_STATE_ERROR || state->state == STREAM_STATE_RECONNECTING) {
        int failures = atomic_load(&state->protocol_state.reconnect_attempts);
        if (failures >= STREAM_MAX_CONSECUTIVE_FAILURES && cooldown_elapsed) {
            return true;
        }
        return false;
    }

    if (state->state == STREAM_STATE_INACTIVE) {
        stream_status_t udt_status = get_unified_detection_effective_status(stream_name);
        if (udt_status == STREAM_STATUS_RECONNECTING) {
            int udt_attempts = get_unified_detection_reconnect_attempts(stream_name);
            if (udt_attempts >= STREAM_MAX_CONSECUTIVE_FAILURES && cooldown_elapsed) {
                return true;
            }
        }
    }

    int proxy_failures = recovery_proxy_open_failures(stream_name);
    if (proxy_failures >= STREAM_PROXY_OPEN_FAILURE_THRESHOLD && cooldown_elapsed) {
        return true;
    }

    return false;
}

/**
 * @brief Recover one unhealthy stream without disrupting healthy streams
 */
static bool recover_stream_scoped(const stream_config_t *config) {
    if (!config || config->name[0] == '\0') {
        return false;
    }

    if (g_config.audio_disabled || config->go2rtc_source_override[0] == '\0') {
        return go2rtc_integration_reload_stream(config->name);
    }

    return true;
}

/**
 * @brief Check stream consensus - if all/most streams are down, it's likely go2rtc
 */
static bool check_stream_consensus(void) {
    int total_streams = 0;
    int failed_streams = 0;
    int stream_count = get_total_stream_count();

    if (stream_count < PROCESS_MIN_STREAMS_FOR_CONSENSUS) {
        return false;
    }

    for (int i = 0; i < stream_count; i++) {
        stream_handle_t stream = get_stream_by_index(i);
        if (!stream) continue;

        stream_config_t config;
        if (get_stream_config(stream, &config) != 0 || !config.enabled) continue;

        total_streams++;

        stream_state_manager_t *state = get_stream_state_by_name(config.name);
        if (!state) continue;

        pthread_mutex_lock(&state->mutex);
        stream_state_t current_state = state->state;
        pthread_mutex_unlock(&state->mutex);

        if (current_state == STREAM_STATE_ERROR || current_state == STREAM_STATE_RECONNECTING) {
            failed_streams++;
        } else if (current_state == STREAM_STATE_INACTIVE) {
            if (get_unified_detection_effective_status(config.name) == STREAM_STATUS_RECONNECTING) {
                failed_streams++;
            }
        }
    }

    return (total_streams >= PROCESS_MIN_STREAMS_FOR_CONSENSUS && failed_streams * 2 > total_streams);
}

/**
 * @brief Check if we can restart (rate limiting)
 */
static bool can_restart_go2rtc(void) {
    time_t now = time(NULL);
    if (now - g_last_restart_time < PROCESS_RESTART_COOLDOWN_SEC) {
        return false;
    }

    int recent_restarts = 0;
    for (int i = 0; i < PROCESS_MAX_RESTARTS_PER_WINDOW; i++) {
        if (g_restart_history[i] > 0 && (now - g_restart_history[i]) < PROCESS_RESTART_WINDOW_SEC) {
            recent_restarts++;
        }
    }

    return (recent_restarts < PROCESS_MAX_RESTARTS_PER_WINDOW);
}

/**
 * @brief Restart the go2rtc process
 */
static bool restart_go2rtc_process_locked(void) {
    if (!go2rtc_process_stop()) {
        return false;
    }

    go2rtc_stream_invalidate_ready_cache();
    sleep(2);

    int api_port = go2rtc_stream_get_api_port();
    if (api_port == 0) api_port = 1984;

    if (!go2rtc_process_start(api_port)) {
        return false;
    }

    int retries = 10;
    while (retries > 0 && !go2rtc_stream_is_ready()) {
        sleep(2);
        retries--;
    }

    if (!go2rtc_stream_is_ready()) {
        return false;
    }

    go2rtc_integration_register_all_streams();
    signal_all_mp4_recordings_reconnect();

    time_t now = time(NULL);
    g_last_restart_time = now;
    g_restart_count++;
    g_restart_history[g_restart_history_index] = now;
    g_restart_history_index = (g_restart_history_index + 1) % PROCESS_MAX_RESTARTS_PER_WINDOW;
    g_consecutive_api_failures = 0;

    return true;
}

static bool restart_go2rtc_process(bool intentional) {
    go2rtc_lifecycle_guard_t guard;
    if (!go2rtc_lifecycle_begin(GO2RTC_LIFECYCLE_RESTART, true, intentional, &guard)) {
        return false;
    }
    if (guard.coalesced) {
        return guard.result;
    }

    bool restarted;
    if (!go2rtc_stream_is_initialized() || !g_initialized) {
        restarted = go2rtc_integration_full_start_locked();
    } else {
        restarted = restart_go2rtc_process_locked();
    }

    go2rtc_lifecycle_end(&guard, restarted);
    return restarted;
}

bool go2rtc_integration_restart_process(void) {
    return restart_go2rtc_process(true);
}

/**
 * @brief Unified health monitor thread - handles both stream and process health
 */
static void *unified_health_monitor_thread(void *arg) {
    (void)arg;
    log_set_thread_context("go2rtc", NULL);
    bool process_restarted = false;

    while (g_monitor_running && !is_shutdown_initiated()) {
        for (int i = 0; i < HEALTH_CHECK_INTERVAL_SEC && g_monitor_running && !is_shutdown_initiated(); i++) {
            sleep(1);
        }

        if (!g_monitor_running || is_shutdown_initiated()) break;

        if (go2rtc_lifecycle_intentional_restart_active()) {
            g_consecutive_api_failures = 0;
            continue;
        }

        if (!go2rtc_stream_is_initialized()) continue;

        bool api_healthy = go2rtc_stream_is_ready();
        if (!api_healthy) {
            g_consecutive_api_failures++;
            if (g_consecutive_api_failures >= PROCESS_MAX_API_FAILURES) {
                bool consensus_failure = check_stream_consensus();
                int total_stream_count = get_total_stream_count();
                bool should_restart = consensus_failure || (total_stream_count < PROCESS_MIN_STREAMS_FOR_CONSENSUS);

                if (should_restart && can_restart_go2rtc()) {
                    if (restart_go2rtc_process(false)) {
                        process_restarted = true;
                    }
                }
            }
            continue;
        } else {
            if (g_consecutive_api_failures > 0) {
                g_consecutive_api_failures = 0;
            }
        }

        if (process_restarted) {
            process_restarted = false;
            continue;
        }

        int stream_count = get_total_stream_count();
        for (int i = 0; i < stream_count; i++) {
            if (!g_monitor_running || is_shutdown_initiated()) break;

            stream_handle_t stream = get_stream_by_index(i);
            if (!stream) continue;

            stream_config_t config;
            if (get_stream_config(stream, &config) != 0) continue;

            bool yaml_backed = !g_config.audio_disabled && config.go2rtc_source_override[0] != '\0';

            if (get_unified_detection_effective_status(config.name) == STREAM_STATUS_RUNNING) {
                note_stream_progress(config.name);
            }

            bool recovery_needed = stream_needs_reregistration(config.name);
            const char *reason = "state-based";

            if (!recovery_needed) {
                stream_flow_t flow = check_stream_data_flow(config.name);

                if (flow == STREAM_FLOW_FLOWING) {
                    note_stream_progress(config.name);
                    continue;
                }

                if (flow == STREAM_FLOW_MISSING) {
                    if (!config.enabled || config.privacy_mode || yaml_backed ||
                        !recovery_cooldown_elapsed(config.name, time(NULL))) {
                        continue;
                    }
                    note_recovery_attempt(config.name);
                    if (go2rtc_stream_register(config.name, config.url,
                                               config.onvif_username[0] != '\0' ? config.onvif_username : NULL,
                                               config.onvif_password[0] != '\0' ? config.onvif_password : NULL,
                                               config.backchannel_enabled, config.protocol,
                                               config.record_audio, config.codec)) {
                        reset_stuck_tracker(config.name);
                        signal_mp4_recording_reconnect(config.name);
                        trigger_ptz_default_preset(config.name);
                    }
                    continue;
                }

                if (flow != STREAM_FLOW_STUCK || !recovery_cooldown_elapsed(config.name, time(NULL))) {
                    continue;
                }
                recovery_needed = true;
                reason = "stuck";
            }

            if (!config.enabled) continue;

            if (should_escalate_to_process_restart(&config)) {
                if (can_restart_go2rtc() && restart_go2rtc_process(false)) {
                    note_escalation(config.name);
                    trigger_ptz_default_preset(config.name);
                    process_restarted = true;
                    break;
                }
            }

            note_recovery_attempt(config.name);
            if (recover_stream_scoped(&config)) {
                reset_stuck_tracker(config.name);

                stream_state_manager_t *state = get_stream_state_by_name(config.name);
                if (state) {
                    atomic_store(&state->protocol_state.last_reconnect_time, time(NULL));
                    atomic_store(&state->protocol_state.reconnect_attempts, 0);
                }

                signal_mp4_recording_reconnect(config.name);
                trigger_ptz_default_preset(config.name);
            }
        }
    }
    return NULL;
}

/**
 * @brief Start the unified health monitor
 */
static bool start_unified_health_monitor(void) {
    if (g_monitor_initialized) return true;

    memset(g_restart_history, 0, sizeof(g_restart_history));
    g_restart_history_index = 0;
    g_restart_count = 0;
    g_last_restart_time = 0;
    g_consecutive_api_failures = 0;

    g_monitor_running = true;
    g_monitor_initialized = true;

    if (pthread_create(&g_monitor_thread, NULL, unified_health_monitor_thread, NULL) != 0) {
        g_monitor_running = false;
        g_monitor_initialized = false;
        return false;
    }
    return true;
}

/**
 * @brief Stop the unified health monitor
 */
static void stop_unified_health_monitor(void) {
    if (!g_monitor_initialized) return;

    g_monitor_running = false;
    pthread_join(g_monitor_thread, NULL);
    g_monitor_initialized = false;
}

// ============================================================================
// Module Initialization/Cleanup
// ============================================================================

bool go2rtc_integration_init(void) {
    if (g_initialized) return true;

    if (!go2rtc_consumer_init()) return false;

    memset(g_tracked_streams, 0, sizeof(g_tracked_streams));
    start_unified_health_monitor();

    g_initialized = true;
    return true;
}

static bool go2rtc_integration_full_start_locked(void) {
    const char *binary_path = g_config.go2rtc_binary_path[0] != '\0' ? g_config.go2rtc_binary_path : NULL;
    const char *config_dir  = g_config.go2rtc_config_dir[0] != '\0' ? g_config.go2rtc_config_dir  : "/tmp/go2rtc";
    int api_port = g_config.go2rtc_api_port > 0 ? g_config.go2rtc_api_port : 1984;

    if (!go2rtc_stream_is_initialized()) {
        if (!go2rtc_stream_init(binary_path, config_dir, api_port)) return false;
    }

    if (!go2rtc_stream_start_service()) return false;

    if (!go2rtc_integration_is_initialized()) {
        if (!go2rtc_integration_init()) return false;
    }

    go2rtc_integration_register_all_streams();
    return true;
}

bool go2rtc_integration_full_start(void) {
    go2rtc_lifecycle_guard_t guard;
    if (!go2rtc_lifecycle_begin(GO2RTC_LIFECYCLE_FULL_START, true, true, &guard)) return false;
    if (guard.coalesced) return guard.result;

    bool result = go2rtc_integration_full_start_locked();
    go2rtc_lifecycle_end(&guard, result);
    return result;
}

int go2rtc_integration_start_recording(const char *stream_name) {
    return go2rtc_integration_start_recording_with_trigger(stream_name, NULL);
}

int go2rtc_integration_start_recording_with_trigger(const char *stream_name, const char *trigger_type) {
    if (!g_initialized) return start_mp4_recording_with_trigger(stream_name, trigger_type);
    if (!stream_name) return -1;

    bool using_go2rtc = ensure_stream_registered_with_go2rtc(stream_name);
    if (using_go2rtc) {
        go2rtc_stream_tracking_t *tracking = add_tracked_stream(stream_name);
        if (tracking) tracking->using_go2rtc_for_recording = true;
    }
    return start_mp4_recording_with_trigger(stream_name, trigger_type);
}

int go2rtc_integration_stop_recording(const char *stream_name) {
    if (!g_initialized || !stream_name) return stop_mp4_recording(stream_name);

    go2rtc_stream_tracking_t *tracking = find_tracked_stream(stream_name);
    if (tracking && tracking->using_go2rtc_for_recording) {
        int ret = stop_mp4_recording(stream_name);
        tracking->using_go2rtc_for_recording = false;
        return ret;
    }
    return stop_mp4_recording(stream_name);
}

int go2rtc_integration_start_hls(const char *stream_name) {
    if (!g_initialized || !stream_name || is_shutdown_initiated()) return start_hls_stream(stream_name);
    if (g_config.go2rtc_force_native_hls) return start_hls_stream(stream_name);

    bool using_go2rtc = ensure_stream_registered_with_go2rtc(stream_name);
    if (using_go2rtc) {
        go2rtc_stream_tracking_t *existing = find_tracked_stream(stream_name);
        if (!existing || !existing->using_go2rtc_for_hls) {
            go2rtc_api_preload_stream(stream_name);
        }

        go2rtc_stream_tracking_t *tracking = add_tracked_stream(stream_name);
        if (tracking) tracking->using_go2rtc_for_hls = true;
        return 0;
    }
    return start_hls_stream(stream_name);
}

int go2rtc_integration_stop_hls(const char *stream_name) {
    if (!g_initialized || !stream_name) return stop_hls_stream(stream_name);

    go2rtc_stream_tracking_t *tracking = find_tracked_stream(stream_name);
    if (tracking && tracking->using_go2rtc_for_hls) {
        tracking->using_go2rtc_for_hls = false;
        return 0;
    }
    return stop_hls_stream(stream_name);
}

bool go2rtc_integration_is_using_go2rtc_for_recording(const char *stream_name) {
    if (!g_initialized || !stream_name) return false;
    go2rtc_stream_tracking_t *tracking = find_tracked_stream(stream_name);
    return tracking ? tracking->using_go2rtc_for_recording : false;
}

bool go2rtc_integration_is_using_go2rtc_for_hls(const char *stream_name) {
    if (!g_initialized || !stream_name) return false;
    go2rtc_stream_tracking_t *tracking = find_tracked_stream(stream_name);
    return tracking ? tracking->using_go2rtc_for_hls : false;
}

/**
 * @brief Register all existing streams with go2rtc
 */
bool go2rtc_integration_register_all_streams(void) {
    if (!g_initialized || !go2rtc_stream_is_ready()) return false;

    int ms = configured_stream_slots();
    stream_config_t *streams = calloc(ms, sizeof(stream_config_t));
    if (!streams) return false;
    int count = get_all_stream_configs(streams, ms);

    if (count <= 0) {
        free(streams);
        return true;
    }

    bool all_success = true;
    for (int i = 0; i < count; i++) {
        if (streams[i].enabled) {
            if (!g_config.audio_disabled && streams[i].go2rtc_source_override[0] != '\0') continue;

            if (!go2rtc_stream_register(streams[i].name, streams[i].url,
                                       streams[i].onvif_username[0] != '\0' ? streams[i].onvif_username : NULL,
                                       streams[i].onvif_password[0] != '\0' ? streams[i].onvif_password : NULL,
                                       streams[i].backchannel_enabled, streams[i].protocol,
                                       streams[i].record_audio, streams[i].codec)) {
                all_success = false;
            }
        }
    }
    free(streams);
    return all_success;
}

/**
 * @brief Sync database streams to go2rtc
 */
bool go2rtc_sync_streams_from_database(void) {
    if (!g_initialized || !go2rtc_stream_is_ready()) return false;

    int ms2 = configured_stream_slots();
    stream_config_t *db_streams = calloc(ms2, sizeof(stream_config_t));
    if (!db_streams) return false;
    int count = get_all_stream_configs(db_streams, ms2);

    if (count <= 0) {
        free(db_streams);
        return true;
    }

    bool all_success = true;
    for (int i = 0; i < count; i++) {
        if (!db_streams[i].enabled || db_streams[i].privacy_mode) continue;

        const char *username = (db_streams[i].onvif_username[0] != '\0') ? db_streams[i].onvif_username : NULL;
        const char *password = (db_streams[i].onvif_password[0] != '\0') ? db_streams[i].onvif_password : NULL;

        if (!go2rtc_api_stream_exists(db_streams[i].name)) {
            if (!go2rtc_stream_register(db_streams[i].name, db_streams[i].url,
                                       username, password,
                                       db_streams[i].backchannel_enabled, db_streams[i].protocol,
                                       db_streams[i].record_audio, db_streams[i].codec)) {
                all_success = false;
            }
        }
    }
    free(db_streams);
    return all_success;
}

void go2rtc_integration_cleanup(void) {
    if (!g_initialized) return;

    stop_unified_health_monitor();
    for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
        if (g_tracked_streams[i].stream_name[0] != '\0') {
            if (g_tracked_streams[i].using_go2rtc_for_recording) {
                go2rtc_consumer_stop_recording(g_tracked_streams[i].stream_name);
            }
            if (g_tracked_streams[i].using_go2rtc_for_hls) {
                go2rtc_integration_stop_hls(g_tracked_streams[i].stream_name);
            }
            memset(&g_tracked_streams[i], 0, sizeof(g_tracked_streams[i]));
        }
    }

    go2rtc_consumer_cleanup();
    g_initialized = false;
}

bool go2rtc_integration_is_initialized(void) {
    return g_initialized;
}

bool go2rtc_get_rtsp_url(const char *stream_name, char *url, size_t url_size) {
    if (!stream_name || !url || url_size == 0 || !go2rtc_stream_is_ready()) return false;
    if (!ensure_stream_registered_with_go2rtc(stream_name)) return false;
    return go2rtc_stream_get_rtsp_url(stream_name, url, url_size);
}

bool go2rtc_integration_get_hls_url(const char *stream_name, char *buffer, size_t buffer_size) {
    if (!g_initialized || !stream_name || !buffer || buffer_size == 0) return false;
    if (!go2rtc_integration_is_using_go2rtc_for_hls(stream_name) || !go2rtc_stream_is_ready()) return false;

    char encoded_name[MAX_STREAM_NAME * 3];
    simple_url_escape(stream_name, encoded_name, sizeof(encoded_name));

    int api_port = go2rtc_stream_get_api_port();
    if (api_port == 0) api_port = 1984;

    snprintf(buffer, buffer_size, "http://localhost:%d%s/api/stream.m3u8?src=%s%s",
             api_port, go2rtc_api_get_base_path(), encoded_name, g_config.audio_disabled ? "&video" : "");
    return true;
}

bool go2rtc_integration_reload_stream(const char *stream_name) {
    if (!stream_name) return false;
    return go2rtc_integration_reload_stream_config(stream_name, NULL, NULL, NULL, -1, -1, -1);
}

bool go2rtc_integration_reload_stream_config(const char *stream_name,
                                           const char *new_url,
                                           const char *new_username,
                                           const char *new_password,
                                           int new_backchannel_enabled,
                                           int new_protocol,
                                           int new_record_audio) {
    go2rtc_lifecycle_guard_t guard;
    if (!go2rtc_lifecycle_begin(GO2RTC_LIFECYCLE_RECONFIGURE, false, true, &guard)) return false;

    go2rtc_stream_unregister(stream_name);
    usleep(500000);

    stream_handle_t stream = get_stream_by_name(stream_name);
    stream_config_t config;
    if (stream && get_stream_config(stream, &config) == 0) {
        const char *url = new_url ? new_url : config.url;
        const char *username = new_username ? new_username : (config.onvif_username[0] != '\0' ? config.onvif_username : NULL);
        const char *password = new_password ? new_password : (config.onvif_password[0] != '\0' ? config.onvif_password : NULL);
        bool backchannel = (new_backchannel_enabled >= 0) ? (new_backchannel_enabled != 0) : config.backchannel_enabled;
        stream_protocol_t protocol = (new_protocol >= 0) ? (stream_protocol_t)new_protocol : config.protocol;
        bool record_audio = (new_record_audio >= 0) ? (new_record_audio != 0) : config.record_audio;

        go2rtc_stream_register(stream_name, url, username, password, backchannel, protocol, record_audio, config.codec);
    }

    go2rtc_lifecycle_end(&guard, true);
    return true;
}

bool go2rtc_integration_unregister_stream(const char *stream_name) {
    if (!stream_name || !go2rtc_stream_is_ready()) return false;
    if (go2rtc_stream_unregister(stream_name)) {
        for (int i = 0; i < MAX_TRACKED_STREAMS; i++) {
            if (strcmp(g_tracked_streams[i].stream_name, stream_name) == 0) {
                memset(&g_tracked_streams[i], 0, sizeof(g_tracked_streams[i]));
                break;
            }
        }
        return true;
    }
    return false;
}

bool go2rtc_integration_register_stream(const char *stream_name) {
    return ensure_stream_registered_with_go2rtc(stream_name);
}

// ============================================================================
// Public Health Monitor API
// ============================================================================

void go2rtc_integration_report_proxy_open_failure(const char *stream_name) {
    if (!stream_name || stream_name[0] == '\0') return;
    pthread_mutex_lock(&g_recovery_mutex);
    stream_recovery_state_t *st = recovery_state_locked(stream_name, true);
    if (st && st->proxy_open_failures < INT_MAX) {
        st->proxy_open_failures++;
    }
    pthread_mutex_unlock(&g_recovery_mutex);
}

bool go2rtc_integration_monitor_is_running(void) {
    return g_monitor_initialized && g_monitor_running;
}

int go2rtc_integration_get_restart_count(void) {
    return g_restart_count;
}

time_t go2rtc_integration_get_last_restart_time(void) {
    return g_last_restart_time;
}

bool go2rtc_integration_check_health(void) {
    return go2rtc_stream_is_ready();
}
