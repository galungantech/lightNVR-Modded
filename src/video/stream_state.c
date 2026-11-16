/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2025-2026 OpenSensor Engineering and LightNVR contributors
 *
 * This file is part of LightNVR <https://github.com/opensensor/lightNVR>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "video/stream_state.h"
#include "core/logger.h"
#include "core/config.h"
#include "core/url_utils.h"
#include "database/db_streams.h"
#include "utils/strings.h"
#include "video/stream_reader.h"
#include "video/hls_streaming.h"
#include "video/mp4_recording.h"
#include "video/detection.h"
#include "video/stream_transcoding.h"
#include "video/streams.h"
#include "video/onvif_ptz.h"
#ifdef USE_GO2RTC
#include "video/go2rtc/go2rtc_integration.h"
#endif
#include "telemetry/stream_metrics.h"

// Global dynamically-allocated array of stream state manager pointers
static stream_state_manager_t **stream_states = NULL;
static int states_capacity = 0;  // allocated slot count
static pthread_mutex_t states_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool initialized = false;
static bool states_shutting_down = false;

// Deduplication guard so concurrent triggers for the same stream don't spawn duplicate workers
static pthread_mutex_t ptz_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static char active_ptz_streams[MAX_STREAMS][MAX_STREAM_NAME];

static bool try_acquire_ptz_worker_slot(const char *stream_name) {
    pthread_mutex_lock(&ptz_worker_mutex);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (active_ptz_streams[i][0] != '\0' &&
            strcmp(active_ptz_streams[i], stream_name) == 0) {
            pthread_mutex_unlock(&ptz_worker_mutex);
            return false; // Already running for this stream
        }
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (active_ptz_streams[i][0] == '\0') {
            safe_strcpy(active_ptz_streams[i], stream_name, MAX_STREAM_NAME, 0);
            pthread_mutex_unlock(&ptz_worker_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&ptz_worker_mutex);
    return false;
}

static void release_ptz_worker_slot(const char *stream_name) {
    pthread_mutex_lock(&ptz_worker_mutex);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (active_ptz_streams[i][0] != '\0' &&
            strcmp(active_ptz_streams[i], stream_name) == 0) {
            active_ptz_streams[i][0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&ptz_worker_mutex);
}

static void dispose_stream_state(stream_state_manager_t *state) {
    pthread_mutex_destroy(&state->mutex);
    pthread_mutex_destroy(&state->state_mutex);
    free(state);
}

/**
 * Initialize the stream state management system
 */
int init_stream_state_manager(int max_streams) {
    if (max_streams < 1)           max_streams = 1;
    if (max_streams > MAX_STREAMS) max_streams = MAX_STREAMS;

    pthread_mutex_lock(&states_mutex);

    if (initialized) {
        pthread_mutex_unlock(&states_mutex);
        return 0;  // Already initialized
    }

    stream_states = (stream_state_manager_t **)calloc(max_streams, sizeof(stream_state_manager_t *));
    if (!stream_states) {
        pthread_mutex_unlock(&states_mutex);
        log_error("init_stream_state_manager: failed to allocate states array");
        return -1;
    }
    states_capacity = max_streams;

    initialized = true;
    pthread_mutex_unlock(&states_mutex);

    log_info("Stream state manager initialized (%d slots)", max_streams);
    return 0;
}

/**
 * Shutdown the stream state management system
 */
void shutdown_stream_state_manager(void) {
    pthread_mutex_lock(&states_mutex);
    if (!initialized || states_shutting_down) {
        pthread_mutex_unlock(&states_mutex);
        return;
    }
    states_shutting_down = true;

    for (int i = 0; i < states_capacity; i++) {
        if (stream_states[i]) {
            stream_state_manager_t *state = stream_states[i];
            char stream_name[MAX_STREAM_NAME];
            safe_strcpy(stream_name, state->name, MAX_STREAM_NAME, 0);

            if (state->state == STREAM_STATE_ACTIVE ||
                state->state == STREAM_STATE_STARTING ||
                state->state == STREAM_STATE_RECONNECTING ||
                state->state == STREAM_STATE_STOPPING) {
                pthread_mutex_unlock(&states_mutex);
                stop_stream_with_state(state, false);
                pthread_mutex_lock(&states_mutex);
            }

            dispose_stream_state(state);
            stream_states[i] = NULL;

            log_info("Cleaned up stream state for '%s' during shutdown", stream_name);
        }
    }

    free((void *)stream_states);
    stream_states = NULL;
    states_capacity = 0;
    initialized = false;
    states_shutting_down = false;
    pthread_mutex_unlock(&states_mutex);

    log_info("Stream state manager shutdown");
}

/**
 * Create a new stream state manager
 */
stream_state_manager_t *create_stream_state(const stream_config_t *config) {
    if (!config) {
        log_error("Invalid parameters or state manager not initialized");
        return NULL;
    }

    pthread_mutex_lock(&states_mutex);
    if (!initialized || states_shutting_down) {
        pthread_mutex_unlock(&states_mutex);
        log_error("State manager is not initialized or is shutting down");
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < states_capacity; i++) {
        if (!stream_states[i]) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        log_error("No available slots for new stream state");
        pthread_mutex_unlock(&states_mutex);
        return NULL;
    }

    for (int i = 0; i < states_capacity; i++) {
        if (i != slot && stream_states[i] &&
            strcmp(stream_states[i]->name, config->name) == 0) {
            log_error("Stream with name '%s' already exists", config->name);
            pthread_mutex_unlock(&states_mutex);
            return NULL;
        }
    }

    stream_state_manager_t *state = calloc(1, sizeof(stream_state_manager_t));
    if (!state) {
        log_error("Failed to allocate memory for stream state");
        pthread_mutex_unlock(&states_mutex);
        return NULL;
    }

    if (pthread_mutex_init(&state->mutex, NULL) != 0) {
        log_error("Failed to initialize stream state mutex");
        free(state);
        pthread_mutex_unlock(&states_mutex);
        return NULL;
    }

    if (pthread_mutex_init(&state->state_mutex, NULL) != 0) {
        log_error("Failed to initialize stream state transition mutex");
        pthread_mutex_destroy(&state->mutex);
        free(state);
        pthread_mutex_unlock(&states_mutex);
        return NULL;
    }

    memcpy(&state->config, config, sizeof(stream_config_t));
    safe_strcpy(state->name, config->name, MAX_STREAM_NAME, 0);
    state->state = STREAM_STATE_INACTIVE;

    state->features.streaming_enabled = config->streaming_enabled;
    state->features.recording_enabled = config->record;
    state->features.detection_enabled = config->detection_based_recording;
    state->features.motion_detection_enabled = false;

    state->protocol_state.protocol = config->protocol;
    state->protocol_state.is_multicast = false;
    state->protocol_state.reconnect_attempts = 0;
    state->protocol_state.last_reconnect_time = 0;
    state->protocol_state.buffer_size = 0;
    state->protocol_state.timeout_ms = 0;

    state->timestamp_state.last_pts = AV_NOPTS_VALUE;
    state->timestamp_state.last_dts = AV_NOPTS_VALUE;
    state->timestamp_state.expected_next_pts = AV_NOPTS_VALUE;
    state->timestamp_state.pts_discontinuity_count = 0;
    state->timestamp_state.timestamps_initialized = false;

    memset(&state->stats, 0, sizeof(stream_stats_t));

    state->ref_count = 1;
    memset(state->component_refs, 0, sizeof(state->component_refs));
    state->component_refs[STREAM_COMPONENT_API] = 1;
    state->callbacks_enabled = true;

    state->reader_ctx = NULL;
    state->hls_ctx = NULL;
    state->mp4_ctx = NULL;
    state->detection_ctx = NULL;

    stream_states[slot] = state;

    log_info("Created stream state for '%s' in slot %d with initial reference count 1",
             config->name, slot);
    pthread_mutex_unlock(&states_mutex);

    return state;
}

/**
 * Get stream state manager by name
 */
stream_state_manager_t *get_stream_state_by_name(const char *name) {
    if (!name || !initialized) {
        return NULL;
    }

    pthread_mutex_lock(&states_mutex);

    for (int i = 0; i < states_capacity; i++) {
        if (stream_states[i] && strcmp(stream_states[i]->name, name) == 0) {
            pthread_mutex_unlock(&states_mutex);
            return stream_states[i];
        }
    }

    pthread_mutex_unlock(&states_mutex);
    return NULL;
}

/**
 * Update stream state configuration
 */
int update_stream_state_config(stream_state_manager_t *state, const stream_config_t *config) {
    if (!state || !config) {
        log_error("Invalid parameters for update_stream_config");
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    bool protocol_changing = (state->config.protocol != config->protocol);
    bool streaming_changing = (state->config.streaming_enabled != config->streaming_enabled);
    bool recording_changing = (state->config.record != config->record);
    bool detection_changing = (state->config.detection_based_recording != config->detection_based_recording);
    bool onvif_changing = (state->config.is_onvif != config->is_onvif);

    if (onvif_changing) {
        log_info("ONVIF flag changed for stream '%s': %s -> %s",
                state->name,
                state->config.is_onvif ? "true" : "false",
                config->is_onvif ? "true" : "false");
    }

    stream_state_t old_state = state->state;
    stream_protocol_t old_protocol = state->config.protocol;

    memcpy(&state->config, config, sizeof(stream_config_t));

    state->features.streaming_enabled = config->streaming_enabled;
    state->features.recording_enabled = config->record;
    state->features.detection_enabled = config->detection_based_recording;

    if (protocol_changing) {
        state->protocol_state.protocol = config->protocol;
        state->protocol_state.reconnect_attempts = 0;
        state->protocol_state.last_reconnect_time = 0;

        if (config->protocol == STREAM_PROTOCOL_UDP) {
            state->protocol_state.buffer_size = 16777216;
            state->protocol_state.timeout_ms = 10000;
        } else {
            state->protocol_state.buffer_size = 8388608;
            state->protocol_state.timeout_ms = 5000;
        }

        log_info("Protocol changed for stream '%s': %s -> %s",
                state->name,
                old_protocol == STREAM_PROTOCOL_UDP ? "UDP" : "TCP",
                config->protocol == STREAM_PROTOCOL_UDP ? "UDP" : "TCP");
    }

    pthread_mutex_unlock(&state->mutex);

    if (old_state == STREAM_STATE_ACTIVE &&
        (protocol_changing || streaming_changing || recording_changing || detection_changing || onvif_changing)) {
        log_info("Restarting stream '%s' due to configuration changes", state->name);

        stop_stream_with_state(state, false);
        start_stream_with_state(state);
    }

    return 0;
}

/**
 * Update stream protocol
 */
int update_stream_protocol(stream_state_manager_t *state, stream_protocol_t protocol) {
    if (!state) {
        log_error("Invalid parameters for update_stream_protocol");
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    if (state->config.protocol == protocol) {
        pthread_mutex_unlock(&state->mutex);
        log_info("Protocol for stream '%s' already set to %s",
                state->name, protocol == STREAM_PROTOCOL_UDP ? "UDP" : "TCP");
        return 0;
    }

    state->config.protocol = protocol;
    state->protocol_state.protocol = protocol;
    state->protocol_state.reconnect_attempts = 0;
    state->protocol_state.last_reconnect_time = 0;

    if (protocol == STREAM_PROTOCOL_UDP) {
        state->protocol_state.buffer_size = 16777216;
        state->protocol_state.timeout_ms = 10000;
    } else {
        state->protocol_state.buffer_size = 8388608;
        state->protocol_state.timeout_ms = 5000;
    }

    stream_state_t current_state = state->state;

    pthread_mutex_unlock(&state->mutex);

    log_info("Protocol updated for stream '%s' to %s",
            state->name, protocol == STREAM_PROTOCOL_UDP ? "UDP" : "TCP");

    if (current_state == STREAM_STATE_ACTIVE) {
        log_info("Restarting stream '%s' due to protocol change", state->name);

        stop_stream_with_state(state, false);
        start_stream_with_state(state);
    }

    return 0;
}

/**
 * Set stream feature state
 */
int set_stream_feature(stream_state_manager_t *state, const char *feature, bool enabled) {
    if (!state || !feature) {
        log_error("Invalid parameters for set_stream_feature");
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    bool changed = false;

    if (strcmp(feature, "streaming") == 0) {
        if (state->features.streaming_enabled != enabled) {
            state->features.streaming_enabled = enabled;
            state->config.streaming_enabled = enabled;
            changed = true;
        }
    } else if (strcmp(feature, "recording") == 0) {
        if (state->features.recording_enabled != enabled) {
            state->features.recording_enabled = enabled;
            state->config.record = enabled;
            changed = true;
        }
    } else if (strcmp(feature, "detection") == 0) {
        if (state->features.detection_enabled != enabled) {
            state->features.detection_enabled = enabled;
            state->config.detection_based_recording = enabled;
            changed = true;
        }
    } else if (strcmp(feature, "motion_detection") == 0) {
        if (state->features.motion_detection_enabled != enabled) {
            state->features.motion_detection_enabled = enabled;
            changed = true;
        }
    } else {
        pthread_mutex_unlock(&state->mutex);
        log_error("Unknown feature '%s' for stream '%s'", feature, state->name);
        return -1;
    }

    stream_state_t current_state = state->state;

    pthread_mutex_unlock(&state->mutex);

    if (strcmp(feature, "recording") == 0) {
        int actively_recording = get_recording_state(state->name);
        if (enabled) {
            if (current_state == STREAM_STATE_ACTIVE && actively_recording != 1) {
                log_info("Starting recording for stream '%s'", state->name);
                #ifdef USE_GO2RTC
                go2rtc_integration_start_recording(state->name);
                #else
                start_mp4_recording(state->name);
                #endif
            } else if (current_state != STREAM_STATE_ACTIVE) {
                log_info("Stream '%s' is not ACTIVE (state=%d), recording will start when stream becomes active",
                        state->name, current_state);
            }
        } else if (actively_recording == 1) {
            log_info("Stopping recording for stream '%s' (state=%d)", state->name, current_state);
            stop_mp4_recording(state->name);
        }
    }

    if (changed) {
        log_info("Feature '%s' %s for stream '%s'",
                feature, enabled ? "enabled" : "disabled", state->name);

        log_info("Stream '%s' current_state=%d (ACTIVE=%d), applying feature change",
                state->name, current_state, STREAM_STATE_ACTIVE);

        if (strcmp(feature, "recording") != 0) {
            if (current_state == STREAM_STATE_ACTIVE) {
                if (strcmp(feature, "streaming") == 0) {
                    if (enabled) {
                        stream_start_hls(state->name);
                    } else {
                        stream_stop_hls(state->name);
                    }
                } else if (strcmp(feature, "detection") == 0 || strcmp(feature, "motion_detection") == 0) {
                    log_info("Restarting stream '%s' to apply detection settings", state->name);
                    stop_stream_with_state(state, false);
                    start_stream_with_state(state);
                }
            } else {
                log_info("Stream '%s' is not ACTIVE (state=%d), skipping non-recording feature application",
                        state->name, current_state);
            }
        }
    }

    return 0;
}

typedef struct {
    char stream_name[MAX_STREAM_NAME];
} ptz_default_preset_task_t;

static void *ptz_default_preset_worker(void *arg) {
    ptz_default_preset_task_t *task = (ptz_default_preset_task_t *)arg;
    if (!task) {
        return NULL;
    }

    char stream_name[MAX_STREAM_NAME];
    safe_strcpy(stream_name, task->stream_name, sizeof(stream_name), 0);
    free(task);

    log_set_thread_context("PTZDefault", stream_name);
    log_info("PTZ default preset worker started for '%s'", stream_name);

    stream_config_t config;
    if (get_stream_config_by_name(stream_name, &config) != 0 ||
        !config.enabled || !config.ptz_enabled || config.ptz_default_preset[0] == '\0') {
        release_ptz_worker_slot(stream_name);
        return NULL;
    }

    char device_url[MAX_URL_LENGTH];
    if (url_build_onvif_service_url(config.url, config.onvif_port,
                                    "/onvif/device_service",
                                    device_url, sizeof(device_url)) != 0) {
        log_warn("Default PTZ preset: failed to build device URL for '%s'", stream_name);
        release_ptz_worker_slot(stream_name);
        return NULL;
    }

    char ptz_url[512] = {0};
    bool ptz_ready = false;
    bool status_supported = false;
    int idle_streak = 0;
    int unsupported_probes = 0;

    // Fase 1: Tunggu hingga kamera online & motor IDLE (tanpa batas waktu mati/timeout kaku)
    while (!states_shutting_down && initialized) {
        if (get_stream_config_by_name(stream_name, &config) != 0 ||
            !config.enabled || !config.ptz_enabled || config.ptz_default_preset[0] == '\0') {
            release_ptz_worker_slot(stream_name);
            return NULL;
        }

        const char *profile_token = (config.onvif_profile[0] != '\0') ? config.onvif_profile : "Profile_1";

        if (ptz_url[0] == '\0') {
            if (onvif_ptz_get_service_url(device_url, config.onvif_username,
                                          config.onvif_password, ptz_url, sizeof(ptz_url)) != 0) {
                sleep(2);
                continue;
            }
        }

        onvif_ptz_status_t st;
        if (onvif_ptz_get_status(ptz_url, profile_token,
                                 config.onvif_username, config.onvif_password, &st) == 0 && st.valid) {
            status_supported = true;
            if (!st.is_moving) {
                idle_streak++;
                if (idle_streak >= 2) {
                    ptz_ready = true;
                    break;
                }
            } else {
                idle_streak = 0;
            }
        } else {
            onvif_ptz_preset_t probe_preset;
            if (onvif_ptz_get_presets(ptz_url, profile_token,
                                      config.onvif_username, config.onvif_password,
                                      &probe_preset, 1) >= 0) {
                unsupported_probes++;
                if (unsupported_probes >= 2) {
                    ptz_ready = true;
                    break;
                }
            } else {
                ptz_url[0] = '\0';
                unsupported_probes = 0;
            }
        }

        sleep(2);
    }

    if (!ptz_ready || states_shutting_down || !initialized) {
        release_ptz_worker_slot(stream_name);
        return NULL;
    }

    // Fase 2: Eksekusi GotoPreset dan verifikasi kestabilan posisi motor
    const char *profile_token = (config.onvif_profile[0] != '\0') ? config.onvif_profile : "Profile_1";
    for (int attempt = 1; attempt <= 3 && !states_shutting_down; attempt++) {
        if (onvif_ptz_goto_preset(ptz_url, profile_token,
                                  config.onvif_username, config.onvif_password,
                                  config.ptz_default_preset) != 0) {
            sleep(2);
            continue;
        }

        if (!status_supported) {
            sleep(2);
            log_info("Applied default PTZ preset '%s' for stream '%s' (fallback mode)",
                     config.ptz_default_preset, stream_name);
            break;
        }

        // Tunggu hingga motor selesai bergerak (IDLE 2x beruntun)
        int check_idle = 0;
        while (!states_shutting_down) {
            sleep(2);
            onvif_ptz_status_t cur = {0};
            if (onvif_ptz_get_status(ptz_url, profile_token,
                                     config.onvif_username, config.onvif_password, &cur) != 0 || !cur.valid) {
                break;
            }
            if (!cur.is_moving) {
                check_idle++;
                if (check_idle >= 2) {
                    log_info("Applied and verified default PTZ preset '%s' for stream '%s'",
                             config.ptz_default_preset, stream_name);
                    release_ptz_worker_slot(stream_name);
                    return NULL;
                }
            } else {
                check_idle = 0;
            }
        }
    }

    release_ptz_worker_slot(stream_name);
    return NULL;
}

void trigger_ptz_default_preset(const char *stream_name) {
    if (!stream_name || stream_name[0] == '\0' || states_shutting_down) {
        return;
    }

    stream_config_t config;
    if (get_stream_config_by_name(stream_name, &config) != 0) {
        return;
    }

    if (!config.enabled || !config.ptz_enabled || config.ptz_default_preset[0] == '\0') {
        return;
    }

    if (!try_acquire_ptz_worker_slot(stream_name)) {
        log_debug("PTZ default preset worker already active for '%s', skipping duplicate trigger", stream_name);
        return;
    }

    ptz_default_preset_task_t *task = calloc(1, sizeof(ptz_default_preset_task_t));
    if (!task) {
        log_warn("Failed to allocate PTZ default preset task for '%s'", stream_name);
        release_ptz_worker_slot(stream_name);
        return;
    }
    safe_strcpy(task->stream_name, stream_name, sizeof(task->stream_name), 0);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, ptz_default_preset_worker, task) != 0) {
        log_warn("Failed to spawn PTZ default preset thread for '%s'", stream_name);
        release_ptz_worker_slot(stream_name);
        free(task);
    }

    pthread_attr_destroy(&attr);
}

/**
 * Start stream with state
 */
int start_stream_with_state(stream_state_manager_t *state) {
    if (!state) {
        log_error("Invalid parameters for start_stream_with_state");
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    if (state->state == STREAM_STATE_ACTIVE) {
        pthread_mutex_unlock(&state->mutex);
        log_info("Stream '%s' is already running", state->name);
        return 0;
    }

    state->state = STREAM_STATE_STARTING;

    bool streaming_enabled = state->features.streaming_enabled;
    bool recording_enabled = state->features.recording_enabled;
    bool detection_enabled = state->features.detection_enabled;
    stream_protocol_t protocol = state->protocol_state.protocol;

    pthread_mutex_unlock(&state->mutex);

    log_info("Starting stream '%s' (protocol: %s, streaming: %s, recording: %s, detection: %s)",
            state->name,
            protocol == STREAM_PROTOCOL_UDP ? "UDP" : "TCP",
            streaming_enabled ? "enabled" : "disabled",
            recording_enabled ? "enabled" : "disabled",
            detection_enabled ? "enabled" : "disabled");

    bool any_component_started = false;

    if (streaming_enabled) {
        int hls_result = stream_start_hls(state->name);
        if (hls_result != 0) {
            log_error("Failed to start HLS stream '%s'", state->name);
        } else {
            log_info("Started HLS streaming for '%s'", state->name);
            any_component_started = true;
        }
    }

    if (recording_enabled) {
        bool schedule_ok = is_recording_scheduled(&state->config);
        if (!schedule_ok) {
            log_info("Stream '%s': recording deferred — outside scheduled window", state->name);
            any_component_started = true;
        } else {
            #ifdef USE_GO2RTC
            int mp4_result = go2rtc_integration_start_recording(state->name);
            #else
            int mp4_result = start_mp4_recording(state->name);
            #endif
            if (mp4_result != 0) {
                log_error("Failed to start MP4 recording for '%s'", state->name);
            } else {
                log_info("Started recording for '%s'", state->name);
                any_component_started = true;
            }
        }
    }

    pthread_mutex_lock(&state->mutex);
    if (any_component_started) {
        state->state = STREAM_STATE_ACTIVE;
        log_info("Stream '%s' is now running", state->name);

        if (state->config.fps > 0) {
            metrics_set_configured_fps(state->name, (double)state->config.fps);
        }
    } else {
        state->state = STREAM_STATE_ERROR;
        log_error("Failed to start any components for stream '%s'", state->name);
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }
    pthread_mutex_unlock(&state->mutex);

    if (any_component_started) {
        trigger_ptz_default_preset(state->name);
    }

    return 0;
}

/**
 * Stop stream with state
 */
int stop_stream_with_state(stream_state_manager_t *state, bool wait_for_completion) {
    if (!state) {
        log_error("Invalid parameters for stop_stream_with_state");
        return -1;
    }

    pthread_mutex_lock(&state->state_mutex);

    if (state->state == STREAM_STATE_INACTIVE) {
        pthread_mutex_unlock(&state->state_mutex);
        log_info("Stream '%s' is already stopped", state->name);
        return 0;
    }

    if (state->state == STREAM_STATE_STOPPING) {
        pthread_mutex_unlock(&state->state_mutex);
        log_info("Stream '%s' is already in the process of stopping", state->name);

        if (wait_for_completion) {
            return wait_for_stream_stop(state, 10000);
        }
        return 0;
    }

    stream_state_t old_state = state->state;
    state->state = STREAM_STATE_STOPPING;

    bool streaming_enabled = state->features.streaming_enabled;

    char stream_name[MAX_STREAM_NAME];
    safe_strcpy(stream_name, state->name, MAX_STREAM_NAME, 0);

    if (!streaming_enabled) {
        state->callbacks_enabled = false;
    }

    pthread_mutex_unlock(&state->state_mutex);

    log_info("Stream '%s' transitioning from %d to STOPPING state",
             stream_name, old_state);

    int result = stream_stop_hls(stream_name);
    if (result != 0) {
        log_warn("Failed to stop HLS stream '%s'", stream_name);
    } else {
        log_info("Stopped HLS streaming for '%s'", stream_name);
    }

    int mp4_result = stop_mp4_recording(stream_name);
    if (mp4_result != 0) {
        log_warn("Failed to stop MP4 recording for '%s'", stream_name);
    } else {
        log_info("Stopped MP4 recording for '%s'", stream_name);
    }

    reset_timestamp_tracker(stream_name);

    pthread_mutex_lock(&state->state_mutex);
    state->state = STREAM_STATE_INACTIVE;
    state->callbacks_enabled = true;
    pthread_mutex_unlock(&state->state_mutex);

    log_info("Stopped stream '%s'", stream_name);
    return 0;
}

/**
 * Get stream operational state
 */
stream_state_t get_stream_operational_state(stream_state_manager_t *state) {
    if (!state) {
        return STREAM_STATE_INACTIVE;
    }

    pthread_mutex_lock(&state->mutex);
    stream_state_t current_state = state->state;
    pthread_mutex_unlock(&state->mutex);

    return current_state;
}

/**
 * Get stream statistics
 */
int get_stream_statistics(stream_state_manager_t *state, stream_stats_t *stats) {
    if (!state || !stats) {
        return -1;
    }

    pthread_mutex_lock(&state->mutex);
    memcpy(stats, &state->stats, sizeof(stream_stats_t));
    pthread_mutex_unlock(&state->mutex);

    return 0;
}

/**
 * Handle stream error
 */
int handle_stream_error(stream_state_manager_t *state, int error_code, const char *error_message) {
    if (!state) {
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    state->stats.errors++;
    log_error("Stream '%s' error: %s (code: %d)", state->name, error_message, error_code);

    if (error_message) {
        snprintf(state->last_error_message, sizeof state->last_error_message,
                 "%s", error_message);
    } else {
        state->last_error_message[0] = '\0';
    }

    {
        const char *error_type = "io";
        if (error_code == -ETIMEDOUT) {
            error_type = "timeout";
        } else if (error_code == -EINVAL) {
            error_type = "protocol";
        } else if (error_code == -EIO) {
            error_type = "io";
        } else if (error_code == STREAM_ERR_MODEL_LOAD) {
            error_type = "model";
        }
        metrics_record_error(state->name, error_type);
    }

    bool permanent_error = (error_code == STREAM_ERR_MODEL_LOAD);
    bool should_reconnect = false;

    if (state->state == STREAM_STATE_ACTIVE && !permanent_error) {
        should_reconnect = true;
        state->state = STREAM_STATE_RECONNECTING;
        state->protocol_state.reconnect_attempts++;
        state->protocol_state.last_reconnect_time = time(NULL);
        state->stats.reconnects++;
        metrics_record_reconnect(state->name);
    } else {
        state->state = STREAM_STATE_ERROR;
    }

    pthread_mutex_unlock(&state->mutex);

    if (should_reconnect) {
        log_info("Attempting to reconnect stream '%s'", state->name);
        stop_stream_with_state(state, false);
        sleep(1);
        start_stream_with_state(state);
    }

    return 0;
}

/**
 * Update stream timestamps
 */
int update_stream_timestamps(stream_state_manager_t *state, int64_t pts, int64_t dts) {
    if (!state) {
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    state->timestamp_state.last_pts = pts;
    state->timestamp_state.last_dts = dts;

    if (!state->timestamp_state.timestamps_initialized) {
        state->timestamp_state.timestamps_initialized = true;
        state->timestamp_state.expected_next_pts = pts;
    } else {
        int64_t frame_duration = 0;
        if (state->config.fps > 0) {
            frame_duration = 90000 / state->config.fps;
        } else {
            frame_duration = 3000;
        }
        state->timestamp_state.expected_next_pts = pts + frame_duration;
    }

    pthread_mutex_unlock(&state->mutex);

    return 0;
}

/**
 * Get total number of streams
 */
int get_stream_state_count(void) {
    if (!initialized) {
        return 0;
    }

    pthread_mutex_lock(&states_mutex);

    int count = 0;
    for (int i = 0; i < states_capacity; i++) {
        if (stream_states[i]) {
            count++;
        }
    }

    pthread_mutex_unlock(&states_mutex);
    return count;
}

/**
 * Get stream state manager by index
 */
stream_state_manager_t *get_stream_state_by_index(int index) {
    if (index < 0 || index >= states_capacity || !initialized) {
        return NULL;
    }

    pthread_mutex_lock(&states_mutex);

    stream_state_manager_t *state = stream_states[index];

    pthread_mutex_unlock(&states_mutex);
    return state;
}

/**
 * Remove stream state manager
 */
int remove_stream_state(stream_state_manager_t *state) {
    if (!state) {
        return -1;
    }

    pthread_mutex_lock(&states_mutex);
    if (!initialized || states_shutting_down) {
        pthread_mutex_unlock(&states_mutex);
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < states_capacity; i++) {
        if (stream_states[i] == state) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        pthread_mutex_unlock(&states_mutex);
        log_error("Stream state not found in array");
        return -1;
    }

    char stream_name[MAX_STREAM_NAME];
    safe_strcpy(stream_name, state->name, MAX_STREAM_NAME, 0);

    pthread_mutex_lock(&state->mutex);
    int ref_count = state->ref_count;
    pthread_mutex_unlock(&state->mutex);

    if (ref_count > 1) {
        pthread_mutex_unlock(&states_mutex);
        log_warn("Cannot remove stream state for '%s' with reference count %d",
                stream_name, ref_count);
        return -1;
    }

    stream_states[slot] = NULL;

    if (state->state == STREAM_STATE_ACTIVE ||
        state->state == STREAM_STATE_STARTING ||
        state->state == STREAM_STATE_RECONNECTING ||
        state->state == STREAM_STATE_STOPPING) {
        pthread_mutex_unlock(&states_mutex);
        stop_stream_with_state(state, true);
        pthread_mutex_lock(&states_mutex);
    }

    remove_timestamp_tracker(stream_name);

    dispose_stream_state(state);

    pthread_mutex_unlock(&states_mutex);

    log_info("Removed stream state for '%s' from slot %d", stream_name, slot);
    return 0;
}

/**
 * Add a reference to a stream state manager
 */
int stream_state_add_ref(stream_state_manager_t *state, stream_component_t component) {
    if (!state) {
        return -1;
    }

    if (component < 0 || component >= STREAM_COMPONENT_COUNT) {
        log_error("Invalid component type %d for stream '%s'", component, state->name);
        return -1;
    }

    pthread_mutex_lock(&state->mutex);
    state->component_refs[component]++;
    state->ref_count++;
    int new_ref_count = state->ref_count;
    pthread_mutex_unlock(&state->mutex);

    log_debug("Added reference to stream '%s' for component %d, new ref count: %d",
             state->name, component, new_ref_count);

    return new_ref_count;
}

/**
 * Release a reference to a stream state manager
 */
int stream_state_release_ref(stream_state_manager_t *state, stream_component_t component) {
    if (!state) {
        return -1;
    }

    if (component < 0 || component >= STREAM_COMPONENT_COUNT) {
        log_error("Invalid component type %d for stream '%s'", component, state->name);
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    if (state->component_refs[component] <= 0) {
        log_warn("Component %d has no references to release for stream '%s'",
                component, state->name);
        pthread_mutex_unlock(&state->mutex);
        return state->ref_count;
    }

    state->component_refs[component]--;
    state->ref_count--;
    int new_ref_count = state->ref_count;

    pthread_mutex_unlock(&state->mutex);

    log_debug("Released reference to stream '%s' for component %d, new ref count: %d",
             state->name, component, new_ref_count);

    return new_ref_count;
}

/**
 * Get the current reference count for a stream state manager
 */
int stream_state_get_ref_count(stream_state_manager_t *state) {
    if (!state) {
        return -1;
    }

    pthread_mutex_lock(&state->mutex);
    int ref_count = state->ref_count;
    pthread_mutex_unlock(&state->mutex);

    return ref_count;
}

/**
 * Check if a stream is in the process of stopping
 */
bool is_stream_state_stopping(stream_state_manager_t *state) {
    if (!state) {
        return false;
    }

    pthread_mutex_lock(&state->state_mutex);
    bool stopping = (state->state == STREAM_STATE_STOPPING);
    pthread_mutex_unlock(&state->state_mutex);

    return stopping;
}

/**
 * Wait for a stream to complete its stopping process
 */
int wait_for_stream_stop(stream_state_manager_t *state, int timeout_ms) {
    if (!state) {
        return -2;
    }

    struct timeval end_time;
    gettimeofday(&end_time, NULL);

    end_time.tv_sec += timeout_ms / 1000;
    end_time.tv_usec += (long)(timeout_ms % 1000) * 1000;
    if (end_time.tv_usec >= 1000000) {
        end_time.tv_sec += 1;
        end_time.tv_usec -= 1000000;
    }

    while (1) {
        pthread_mutex_lock(&state->state_mutex);
        bool is_inactive = (state->state == STREAM_STATE_INACTIVE);
        pthread_mutex_unlock(&state->state_mutex);

        if (is_inactive) {
            return 0;
        }

        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        if (current_time.tv_sec > end_time.tv_sec ||
            (current_time.tv_sec == end_time.tv_sec &&
             current_time.tv_usec >= end_time.tv_usec)) {
            return -1;
        }

        usleep(10000);
    }
}

/**
 * Enable or disable callbacks for a stream
 */
int set_stream_callbacks_enabled(stream_state_manager_t *state, bool enabled) {
    if (!state) {
        return -1;
    }

    pthread_mutex_lock(&state->mutex);
    state->callbacks_enabled = enabled;
    pthread_mutex_unlock(&state->mutex);

    log_info("Callbacks %s for stream '%s'",
            enabled ? "enabled" : "disabled", state->name);

    return 0;
}

/**
 * Check if callbacks are enabled for a stream
 */
bool are_stream_callbacks_enabled(stream_state_manager_t *state) {
    if (!state) {
        return false;
    }

    pthread_mutex_lock(&state->mutex);
    bool enabled = state->callbacks_enabled;
    pthread_mutex_unlock(&state->mutex);

    return enabled;
}
