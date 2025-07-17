/*
 * dedupmq - Mosquitto plugin to filter duplicate MQTT messages
 * Filters messages based on SHA256 hash of payload
 * Stores hashes in Memcached with configurable TTL
 *
 * LZ5AE
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <xxhash.h>
#include <libmemcached/memcached.h>
#include <mosquitto_broker.h>
#include <mosquitto_plugin.h>
#include <mosquitto.h>

#define MAX_TOPICS 64
#ifndef MOSQ_ERR_PLUGIN_IGNORE
#define MOSQ_ERR_PLUGIN_IGNORE 17
#endif

static mosquitto_plugin_id_t *plugin_id;
static memcached_st *memc;
static char *topics[MAX_TOPICS];
static int topic_count = 0;
static int ttl_seconds = 60;
static int verbose_log = 0;

// Using xxHash because it's faster
static char *xxhash64_hex(const void *payload, int len) {
    XXH64_hash_t hash = XXH64(payload, len, 0);
    char *hex = malloc(17);
    snprintf(hex, 17, "%016llx", (unsigned long long)hash);
    return hex;
}

static int on_message(int event, void *event_data, void *userdata) {
    struct mosquitto_evt_message *msg = event_data;

    if (verbose_log) mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Published to %s -> %s", msg->topic, msg->payload);

    // Check if topic matches any filter
    int matched = 0;
    for (int i = 0; i < topic_count; i++) {
        if (verbose_log) mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Compare incoming topic %s to configured %s", msg->topic, topics[i]);
        bool res;
        if (mosquitto_topic_matches_sub(topics[i], msg->topic, &res) == MOSQ_ERR_SUCCESS && res) {
            matched = 1;
            if (verbose_log) mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Topic matched, let's do some work...");
            break;
        }
    }
    if (!matched) return MOSQ_ERR_SUCCESS; // Skip if not in filter

    // Hash payload
    char *hash_key = xxhash64_hex(msg->payload, msg->payloadlen);

    // Check memcached
    memcached_return rc;
    size_t val_len;
    uint32_t flags;
    char *val = memcached_get(memc, hash_key, strlen(hash_key), &val_len, &flags, &rc);
    if (val) {
        mosquitto_log_printf(MOSQ_LOG_NOTICE, "[dedupmq] Dropped duplicate on %s = %s", msg->topic, msg->payload);
        free(val);
        free(hash_key);
        // Drop message
        return MOSQ_ERR_PLUGIN_IGNORE;
    }

    // Store hash in Memcached
    rc = memcached_set(memc, hash_key, strlen(hash_key), "1", 1, ttl_seconds, 0);
    if (rc != MEMCACHED_SUCCESS) {
        if (verbose_log) mosquitto_log_printf(MOSQ_LOG_ERR, "[dedupmq] Storing failed: %s", memcached_strerror(memc, rc));
    } else {
        if (verbose_log) mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Stored hash %s", hash_key);
    }

    free(hash_key);
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_plugin_version(int supported_version_count, const int *supported_versions) {
    for (int i = 0; i < supported_version_count; i++) {
        if (supported_versions[i] == 5) {
            // Declare support of plugin API v5
            return 5;
        }
    }
    return -1;
}

int mosquitto_plugin_init(mosquitto_plugin_id_t *identifier, void **userdata, struct mosquitto_opt *options, int option_count) {
    plugin_id = identifier;
    mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Plugin initializing...");

    const char *memcached_host = "127.0.0.1";
    const char *memcached_ports = "11211";
    for (int i = 0; i < option_count; i++) {
        if (!strcmp(options[i].key, "topic")) {
            if (topic_count < MAX_TOPICS) {
                topics[topic_count++] = strdup(options[i].value);
            }
        } else if (!strcmp(options[i].key, "memcached_host")) {
            memcached_host = options[i].value;
        } else if (!strcmp(options[i].key, "memcached_port")) {
            memcached_ports = options[i].value;
        } else if (!strcmp(options[i].key, "ttl")) {
            ttl_seconds = atoi(options[i].value);
        } else if (!strcmp(options[i].key, "verbose_log")) {
            verbose_log = (!strcmp(options[i].value, "true") || !strcmp(options[i].value, "1"));
        }
    }

    memcached_return rc;
    memc = memcached_create(NULL);
    int memcached_port = atoi(memcached_ports);
    memcached_server_add(memc, memcached_host, memcached_port);
    if (!memc) {
        mosquitto_log_printf(MOSQ_LOG_ERR, "[dedupmq] Failed to connect to memcached");
        return MOSQ_ERR_UNKNOWN;
    }

    mosquitto_callback_register(plugin_id, MOSQ_EVT_MESSAGE, on_message, NULL, 0);
    mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Plugin loaded. Monitoring %d topics.", topic_count);
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_plugin_cleanup(void *userdata, struct mosquitto_opt *options, int option_count) {
    if (memc) memcached_free(memc);
    for (int i = 0; i < topic_count; i++) free(topics[i]);
    mosquitto_log_printf(MOSQ_LOG_INFO, "[dedupmq] Plugin cleaned up.");
    return MOSQ_ERR_SUCCESS;
}
