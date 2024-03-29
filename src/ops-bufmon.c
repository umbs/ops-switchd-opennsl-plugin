/*
 * Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 *
 * File: ops-bufmon.c
 *
 * Purpose: This file contains OpenSwitch bufmon related application code
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <openvswitch/vlog.h>
#include <opennsl/error.h>
#include <opennsl/types.h>
#include <opennsl/cosq.h>
#include <opennsl/switch.h>

#include "bufmon-bcm-provider.h"
#include "platform-defines.h"
#include "ops-debug.h"

VLOG_DEFINE_THIS_MODULE(ops_bufmon);

/* structure to map realm string to statid  and counter helper functions */
typedef struct realm_helper {
    /* realm string */
    const char *realm;

    /* statid type */
    opennsl_bst_stat_id_t statid;

    /* internal function to collect the statistics or setting threshold */
    void (*bufmon_counter_handler)(int statid, counter_operations_t type,
                                   bufmon_counter_info_t *counter);
} realm_helper_t;

#define OPENNSL_RV_ERROR_CHECK(_rv, fmt, args...)     \
        if ((_rv) != OPENNSL_E_NONE) {   \
            VLOG_DBG("Opennsl error (%s:%d %d) "fmt, __FILE__,    \
                     __LINE__, _rv, ##args);     \
            return;     \
        }

static int trigger_user_data[MAX_SWITCH_UNITS];

#define INVALID  (-1)

/* Checks the input parameters are valid */
#define INPUT_PARAM_VALIDATE(_param)                   \
        if ((_param) == INVALID) {   \
            VLOG_DBG("%s:%d invalid parameter  ",  \
                     __FUNCTION__, __LINE__); \
            return;                              \
        }

#define MAX_STATS get_max_stats()

/* Opennsl Mapping functions */
#define BCM_API_SWITCH_CONTROL_GET(_unit, _control_type, _arg_ptr)      \
        opennsl_switch_control_get((_unit), (_control_type), (_arg_ptr))

#define BCM_API_SWITCH_CONTROL_SET(_unit, _control_type, _arg)      \
        opennsl_switch_control_set((_unit), (_control_type), (_arg))

#define BCM_API_BST_STAT_GET(_unit, _gport, _cosq, _bid,    \
                             _options, _data_ptr)     \
        opennsl_cosq_bst_stat_get((_unit), (_gport), (_cosq), (_bid),   \
                                  (_options), (uint64 *)(_data_ptr))

#define BCM_API_BST_PROFILE_SET(_unit, _gport, _cosq, _bid, _profile_ptr)  \
        opennsl_cosq_bst_profile_set((_unit), (_gport), (_cosq),    \
                                     (_bid), (_profile_ptr))

#define BCM_API_BST_PROFILE_GET(_unit, _gport, _cosq, _bid, _profile_ptr)  \
        opennsl_cosq_bst_profile_get((_unit), (_gport), (_cosq),    \
                                     (_bid), (_profile_ptr))

#define BCM_API_BST_STAT_CLEAR(_unit, _gport, _cosq, _bid)                \
        opennsl_cosq_bst_stat_clear((_unit), (_gport), (_cosq), (_bid))

#define BCM_API_COSQ_BST_STAT_SYNC(_unit, _bid)                          \
        opennsl_cosq_bst_stat_sync((_unit), (_bid))

static inline unsigned int get_max_stats(void);
static inline int get_realm_index(int statid, char *str);
static const realm_helper_t *get_all_realm_list(void);

static void
device_data_stats(int statid, counter_operations_t type,
                  bufmon_counter_info_t *counter)
{
    opennsl_cosq_bst_profile_t profile;
    opennsl_error_t rv = OPENNSL_E_NONE;

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, 0, 0,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, 0, 0,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", statid);
}/* device_data_stats */

static void
ingress_port_priority_group_stats(int statid, counter_operations_t type,
                                  bufmon_counter_info_t *counter)
{
    int port, pg;
    opennsl_gport_t gport;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    port = smap_get_int(&counter->counter_vendor_specific_info,
                        "port", INVALID);

    INPUT_PARAM_VALIDATE(port);

    pg = smap_get_int(&counter->counter_vendor_specific_info,
                      "priority-group", INVALID);

    INPUT_PARAM_VALIDATE(pg);

    rv = opennsl_port_gport_get(counter->hw_unit_id, port, &gport);

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d %d", counter->hw_unit_id, port, pg);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, gport, pg - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, gport, pg - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d %d", gport, port, pg);
}/* ingress_port_priority_group_stats */

static void
ingress_port_service_pool_stats(int statid, counter_operations_t type,
                                bufmon_counter_info_t *counter)
{
    int port, sp;
    opennsl_gport_t gport;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    port = smap_get_int(&counter->counter_vendor_specific_info,
                        "port", INVALID);

    INPUT_PARAM_VALIDATE(port);

    sp = smap_get_int(&counter->counter_vendor_specific_info,
                      "service-pool", INVALID);

    INPUT_PARAM_VALIDATE(sp);

    rv = opennsl_port_gport_get(counter->hw_unit_id, port, &gport);;

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d", port, gport);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, gport, sp - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, gport, sp - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d %d", gport, port, sp);
}/* ingress_port_service_pool_stats */

static void
ingress_service_pool_stats(int statid, counter_operations_t type,
                           bufmon_counter_info_t *counter)
{
    int sp;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    sp = smap_get_int(&counter->counter_vendor_specific_info,
                      "service-pool", INVALID);

    INPUT_PARAM_VALIDATE(sp);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, 0,
                                  sp - 1, statid, 0,
                                  &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, 0, sp - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", sp);
}/* ingress_service_pool_stats */

static void
egress_service_pool_stats(int statid, counter_operations_t type,
                          bufmon_counter_info_t *counter)
{
    int sp;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    sp = smap_get_int(&counter->counter_vendor_specific_info,
                      "service-pool", INVALID);

    INPUT_PARAM_VALIDATE(sp);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, 0, sp - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, 0, sp - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d ", sp);
}/* egress_service_pool_stats */

static void
egress_unicast_stats(int statid, counter_operations_t type,
                     bufmon_counter_info_t *counter)
{
    int queue, port;
    opennsl_gport_t gport;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    queue = smap_get_int(&counter->counter_vendor_specific_info,
                         "queue", INVALID);
    INPUT_PARAM_VALIDATE(queue);

    port = queue / 8 +  1;
    queue = queue % 8;

    rv = opennsl_port_gport_get(counter->hw_unit_id, port, &gport);

    if (rv != OPENNSL_E_NONE)
      return;

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, gport, queue - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, gport, queue - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d", port, queue);
}/* egress_unicast_stats */

static void
egress_multicast_stats(int statid, counter_operations_t type,
                       bufmon_counter_info_t *counter)
{
    int queue, port;
    opennsl_gport_t gport;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    queue = smap_get_int(&counter->counter_vendor_specific_info,
                         "queue", INVALID);

    INPUT_PARAM_VALIDATE(queue);

    port = queue / 8 +  1;
    queue = queue % 8;

    rv = opennsl_port_gport_get(counter->hw_unit_id, port, &gport);

    if (rv != OPENNSL_E_NONE)
      return;

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, gport, queue - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, gport, queue - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", queue);
}/* egress_multicast_stats */

static void
egress_port_service_pool_stats (int statid, counter_operations_t type,
                                bufmon_counter_info_t *counter)
{
    int sp, port;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_gport_t gport;
    opennsl_cosq_bst_profile_t profile;

    port = smap_get_int(&counter->counter_vendor_specific_info,
                        "port", INVALID);

    INPUT_PARAM_VALIDATE(port);

    sp = smap_get_int(&counter->counter_vendor_specific_info,
                         "service-pool", INVALID);

    INPUT_PARAM_VALIDATE(sp);

    rv = opennsl_port_gport_get(counter->hw_unit_id, port, &gport);

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d", port, gport);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, gport, sp - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, gport, sp - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", sp);
}/* egress_port_service_pool_stats */

static void
egress_cpu_stats (int statid, counter_operations_t type,
                  bufmon_counter_info_t *counter)
{
    int queue;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_gport_t gport;
    opennsl_cosq_bst_profile_t profile;

    queue = smap_get_int(&counter->counter_vendor_specific_info,
                         "queue", INVALID);

    INPUT_PARAM_VALIDATE(queue);

    rv = opennsl_port_gport_get(counter->hw_unit_id, 0, &gport);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, gport, queue - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, gport, queue - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", queue);
}/* egress_cpu_stats */

static void
egress_rqe_queue_stats (int statid, counter_operations_t type,
                        bufmon_counter_info_t *counter)
{
    int queue;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    queue = smap_get_int(&counter->counter_vendor_specific_info,
                         "queue", INVALID);

    INPUT_PARAM_VALIDATE(queue);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, 0, queue - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, 0, queue - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", queue);
}/* egress_rqe_queue_stats */

static void
egress_unicast_queue_group_stats (int statid, counter_operations_t type,
                              bufmon_counter_info_t *counter)
{
    int queue;
    opennsl_error_t rv = OPENNSL_E_NONE;
    opennsl_cosq_bst_profile_t profile;

    queue = smap_get_int(&counter->counter_vendor_specific_info,
                         "queue", INVALID);

    INPUT_PARAM_VALIDATE(queue);

    if (type == GET_COUNTER_VALUE) {
        rv = BCM_API_BST_STAT_GET(counter->hw_unit_id, 0, queue - 1,
                                  statid, 0, &counter->counter_value);
    } else if (type == SET_COUNTER_THRESHOLD
               && counter->trigger_threshold) {
        profile.byte = counter->trigger_threshold;
        rv = BCM_API_BST_PROFILE_SET(counter->hw_unit_id, 0, queue - 1,
                                     statid, &profile);
    }

    OPENNSL_RV_ERROR_CHECK(rv, " %d", queue);
}/* egress_rqe_queue_stats */

static int
get_realm_stat_id(char *str)
{
    unsigned int i = 0;
    const realm_helper_t *realm_list = get_all_realm_list();

    for (i = 0; i < MAX_STATS; i++) {
        if (NULL != strstr(str, realm_list[i].realm)) {
            return realm_list[i].statid;
        }
    }

    return INVALID;
}/* get_realm_stat_id */

static inline int
get_realm_index(int statid, char *str)
{
    unsigned int i = 0;
    const realm_helper_t *realm_list = get_all_realm_list();

    for (i = 0; i < MAX_STATS; i++) {
        if (NULL != strstr(str, realm_list[i].realm)
            && statid == realm_list[i].statid) {
            return i;
        }
    }

    return INVALID;
}/* get_realm_index */

void
realm_sync_all(void)
{
    unsigned int i = 0;
    unsigned int hw_unit = 0;
    const realm_helper_t *realm_list = get_all_realm_list();

    for (hw_unit = 0; hw_unit <= MAX_SWITCH_UNIT_ID; hw_unit++) {
        for (i = 0; i < MAX_STATS; i++) {
            BCM_API_COSQ_BST_STAT_SYNC(hw_unit, realm_list[i].statid);
        }
    }
}/* realm_sync_all */

void
handle_bufmon_counter_mgmt(bufmon_counter_info_t *counter,
                           counter_operations_t type)
{
    int statid = 0;
    const realm_helper_t *realm_list = get_all_realm_list();
    int index = INVALID;

    if (!counter->name) {
        return ;
    }

    statid = get_realm_stat_id(counter->name);

    INPUT_PARAM_VALIDATE(statid);

    index = get_realm_index(statid, counter->name);

    INPUT_PARAM_VALIDATE(index);

    /* Call the bufmon handler specific to realm */
    if ((index) < MAX_STATS) {
        realm_list[index].bufmon_counter_handler(statid, type, counter);
        VLOG_DBG("%s counter value %" PRId64 " ",
                 (counter)->name, (counter)->counter_value);
    }

    return;
}/* handle_bufmon_counter_mgmt */

void
bst_switch_control_get(int unit, opennsl_switch_control_t type, int *value)
{
    opennsl_error_t rv = OPENNSL_E_NONE;

    *value = 0;
    rv = BCM_API_SWITCH_CONTROL_GET(unit, type, value);

    OPENNSL_RV_ERROR_CHECK(rv, " %d %d %d", unit, type, *value);
}/* bst_switch_control_get */

void
bst_switch_control_set(opennsl_switch_control_t  type, int arg)
{
    int old_value = 0;
    opennsl_error_t rv = OPENNSL_E_NONE;
    int hw_unit = 0;

    for (hw_unit = 0; hw_unit <= MAX_SWITCH_UNIT_ID; hw_unit++) {

        bst_switch_control_get(hw_unit, type, &old_value);

        if (old_value != arg) {
            rv = BCM_API_SWITCH_CONTROL_SET(hw_unit, type, arg);
            OPENNSL_RV_ERROR_CHECK(rv, " %d %d %d", hw_unit, type, arg);
        }
    }
}/* bst_switch_control_set */

/*
 * Register hw trigger callback
 *
 * @notes  callback will be executed in driver thread so post the data
 *           to respective task.
 *
 */
void
bst_switch_event_register(bool enable)
{
    int hw_unit = 0;
    static bool event_registered = false;

    for (hw_unit = 0; hw_unit <= MAX_SWITCH_UNIT_ID; hw_unit++) {
        if (enable) {
            opennsl_switch_event_register(hw_unit,
                        (opennsl_switch_event_cb_t)bst_switch_event_callback,
                        (void *)&trigger_user_data[hw_unit]);
            event_registered = true;
        } else if (!enable && event_registered) {
            opennsl_switch_event_unregister(hw_unit,
                        (opennsl_switch_event_cb_t)bst_switch_event_callback,
                        (void *)&trigger_user_data[hw_unit]);
            event_registered = false;
        }
    }
}

/*
 * callback function to process Hw triggers
 *
 * @notes  callback will notify the vswitchd to poll the bufmon stats
 *
 */
void
bst_switch_event_callback (int asic, opennsl_switch_event_t event,
                           int bid, int port, int cosq, void *cookie)
{
    unsigned int i = 0;
    bool valid_trigger = false;
    const realm_helper_t *realm_list = get_all_realm_list();

    /* Disable tracking mode to avoid spurious triggers from HW */
    bst_switch_control_set(opennslSwitchBstEnable, 0);

    /* Call the switchd Callback registered with plugin */
    if (event == OPENNSL_SWITCH_EVENT_MMU_BST_TRIGGER) {
        /* Validate BID */
        for (i = 0; i < MAX_STATS; i++) {
            if (realm_list[i].statid == bid) {
                valid_trigger = true;
            }
        }
        /* notify the vswitchd */
        if (valid_trigger) {
            bufmon_trigger_callback();
        }
    }
}

const realm_helper_t realm_list[] = {
    /* Per device BST tracing resource */
    { "device/data", opennslBstStatIdDevice, device_data_stats},

    /* Per Egress Pool BST tracing resource */
    { "egress-service-pool/um-share-buffer-count",
      opennslBstStatIdEgrPool, egress_service_pool_stats},

    /* Per Egress Pool BST tracing resource(Multicast) */
    { "egress-service-pool/mc-share-buffer-count",
      opennslBstStatIdEgrMCastPool, egress_service_pool_stats},

    /* Per Ingress Pool BST tracing resource */
    { "ingress-service-pool/um-share-buffer-count",
      opennslBstStatIdIngPool, ingress_service_pool_stats},

    /* Per Port Pool BST tracing resource */
    { "ingress-port-service-pool/um-share-buffer-count",
      opennslBstStatIdPortPool, ingress_port_service_pool_stats},

    /* Per Shared Priority Group Pool BST tracing resource */
    { "ingress-port-priority-group/um-share-buffer-count",
      opennslBstStatIdPriGroupShared, ingress_port_priority_group_stats},

    /* Per Priority Group Headroom BST tracing resource */
    { "ingress-port-priority-group/um-headroom-buffer-count",
      opennslBstStatIdPriGroupHeadroom, ingress_port_priority_group_stats},

    /* BST Tracing resource for unicast */
    { "egress-uc-queue/uc-buffer-count",
      opennslBstStatIdUcast, egress_unicast_stats},

    /* BST Tracing resource for multicast */
    { "egress-mc-queue/mc-buffer-count",
      opennslBstStatIdMcast, egress_multicast_stats},

    /* BST Tracing resource for Egress Port Service Pool Resource */
    { "egress-port-service-pool/uc-share-buffer-count",
      opennslBstStatIdEgrUCastPortShared, egress_port_service_pool_stats},

    /* BST Tracing resource for Egress Port Service Pool Resource */
    { "egress-port-service-pool/um-share-buffer-count",
      opennslBstStatIdEgrPortShared, egress_port_service_pool_stats},

    /* BST Tracing resource for CPU queue stats */
    { "egress-cpu-queue/cpu-buffer-count",
      opennslBstStatIdMcast, egress_cpu_stats},

    /* BST Tracing resource for RQE Queue stats */
    { "egress-rqe-queue/rqe-buffer-count",
      opennslBstStatIdRQEQueue, egress_rqe_queue_stats},

    /* BST Tracing resource for Unicast Queue Group stats*/
    { "egress-uc-queue-group/uc-buffer-count",
      opennslBstStatIdUcastGroup, egress_unicast_queue_group_stats}
};

static inline unsigned int
get_max_stats(void)
{
    return sizeof(realm_list) / sizeof(realm_helper_t);
}

static const realm_helper_t *
get_all_realm_list(void)
{
    return realm_list;
}
