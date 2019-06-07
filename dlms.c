/*
 * dlms.c - Device Language Message Specification dissector plugin for Wireshark
 *
 * Copyright (C) 2018 Andre B. Oliveira
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define WS_BUILD_DLL
#define NEW_PROTO_TREE_API
#include <config.h>
#include <epan/dissectors/packet-tcp.h>
#include <epan/exceptions.h>
#include <epan/expert.h>
#include <epan/packet.h>
#include <epan/reassemble.h>
#include <epan/uat.h>
#include <ws_symbol_export.h>
#include <wsutil/wsgcrypt.h>
#include "obis.h"

/* The TCP and UDP port number assigned by IANA for DLMS */
#define DLMS_PORT 4059

/* Choice values for the currently supported ACSE and xDLMS APDUs */
#define DLMS_INITIATE_REQUEST 1
#define DLMS_INITIATE_RESPONSE 8
#define DLMS_DATA_NOTIFICATION 15
#define DLMS_GLO_INITIATE_REQUEST 33
#define DLMS_GLO_INITIATE_RESPONSE 40
#define DLMS_AARQ 96
#define DLMS_AARE 97
#define DLMS_RLRQ 98
#define DLMS_RLRE 99
#define DLMS_GET_REQUEST 192
#define DLMS_SET_REQUEST 193
#define DLMS_EVENT_NOTIFICATION_REQUEST 194
#define DLMS_ACTION_REQUEST 195
#define DLMS_GET_RESPONSE 196
#define DLMS_SET_RESPONSE 197
#define DLMS_ACTION_RESPONSE 199
#define DLMS_GLO_GET_REQUEST 200
#define DLMS_GLO_SET_REQUEST 201
#define DLMS_GLO_EVENT_NOTIFICATION_REQUEST 202
#define DLMS_GLO_ACTION_REQUEST 203
#define DLMS_GLO_GET_RESPONSE 204
#define DLMS_GLO_SET_RESPONSE 205
#define DLMS_GLO_ACTION_RESPONSE 207
#define DLMS_DED_GET_REQUEST 208
#define DLMS_DED_SET_REQUEST 209
#define DLMS_DED_EVENT_NOTIFICATION_REQUEST 210
#define DLMS_DED_ACTION_REQUEST 211
#define DLMS_DED_GET_RESPONSE 212
#define DLMS_DED_SET_RESPONSE 213
#define DLMS_DED_ACTION_RESPONSE 215
#define DLMS_EXCEPTION_RESPONSE 216
#define DLMS_ACCESS_REQUEST 217
#define DLMS_ACCESS_RESPONSE 218
static const value_string dlms_apdu_names[] = {
    { DLMS_INITIATE_REQUEST, "initiate-request" },
    { DLMS_INITIATE_RESPONSE, "initiate-response" },
    { DLMS_DATA_NOTIFICATION, "data-notification" },
    { DLMS_GLO_INITIATE_REQUEST, "glo-initiate-request" },
    { DLMS_GLO_INITIATE_RESPONSE, "glo-initiate-response" },
    { DLMS_AARQ, "aarq" },
    { DLMS_AARE, "aare" },
    { DLMS_RLRQ, "rlrq" },
    { DLMS_RLRE, "rlre" },
    { DLMS_GET_REQUEST, "get-request" },
    { DLMS_SET_REQUEST, "set-request" },
    { DLMS_EVENT_NOTIFICATION_REQUEST, "event-notification-request" },
    { DLMS_ACTION_REQUEST, "action-request" },
    { DLMS_GET_RESPONSE, "get-response" },
    { DLMS_SET_RESPONSE, "set-response" },
    { DLMS_ACTION_RESPONSE, "action-response" },
    { DLMS_GLO_GET_REQUEST, "glo-get-request" },
    { DLMS_GLO_SET_REQUEST, "glo-set-request" },
    { DLMS_GLO_EVENT_NOTIFICATION_REQUEST, "glo-event-notification-request" },
    { DLMS_GLO_ACTION_REQUEST, "glo-action-request" },
    { DLMS_GLO_GET_RESPONSE, "glo-get-response" },
    { DLMS_GLO_SET_RESPONSE, "glo-set-response" },
    { DLMS_GLO_ACTION_RESPONSE, "glo-action-response" },
    { DLMS_DED_GET_REQUEST, "ded-get-request" },
    { DLMS_DED_SET_REQUEST, "ded-set-request" },
    { DLMS_DED_EVENT_NOTIFICATION_REQUEST, "ded-event-notification-request" },
    { DLMS_DED_ACTION_REQUEST, "ded-action-request" },
    { DLMS_DED_GET_RESPONSE, "ded-get-response" },
    { DLMS_DED_SET_RESPONSE, "ded-set-response" },
    { DLMS_DED_ACTION_RESPONSE, "ded-action-response" },
    { DLMS_EXCEPTION_RESPONSE, "exception-response" },
    { DLMS_ACCESS_REQUEST, "access-request" },
    { DLMS_ACCESS_RESPONSE, "access-response" },
    { 0, 0 }
};

/* Choice values for a Get-Request */
#define DLMS_GET_REQUEST_NORMAL 1
#define DLMS_GET_REQUEST_NEXT 2
#define DLMS_GET_REQUEST_WITH_LIST 3
static const value_string dlms_get_request_names[] = {
    { DLMS_GET_REQUEST_NORMAL, "get-request-normal" },
    { DLMS_GET_REQUEST_NEXT, "get-request-next" },
    { DLMS_GET_REQUEST_WITH_LIST, "get-request-with-list" },
    { 0, 0 }
};

/* Choice values for a Get-Response */
#define DLMS_GET_RESPONSE_NORMAL 1
#define DLMS_GET_RESPONSE_WITH_DATABLOCK 2
#define DLMS_GET_RESPONSE_WITH_LIST 3
static const value_string dlms_get_response_names[] = {
    { DLMS_GET_RESPONSE_NORMAL, "get-response-normal" },
    { DLMS_GET_RESPONSE_WITH_DATABLOCK, "get-response-with-datablock" },
    { DLMS_GET_RESPONSE_WITH_LIST, "get-response-with-list" },
    { 0, 0 }
};

/* Choice values for a Set-Request */
#define DLMS_SET_REQUEST_NORMAL 1
#define DLMS_SET_REQUEST_WITH_FIRST_DATABLOCK 2
#define DLMS_SET_REQUEST_WITH_DATABLOCK 3
#define DLMS_SET_REQUEST_WITH_LIST 4
#define DLMS_SET_REQUEST_WITH_LIST_AND_FIRST_DATABLOCK 5
static const value_string dlms_set_request_names[] = {
    { DLMS_SET_REQUEST_NORMAL, "set-request-normal" },
    { DLMS_SET_REQUEST_WITH_FIRST_DATABLOCK, "set-request-with-first-datablock" },
    { DLMS_SET_REQUEST_WITH_DATABLOCK, "set-request-with-datablock" },
    { DLMS_SET_REQUEST_WITH_LIST, "set-request-with-list" },
    { DLMS_SET_REQUEST_WITH_LIST_AND_FIRST_DATABLOCK, "set-request-with-list-and-first-datablock" },
    { 0, 0 }
};

/* Choice values for a Set-Response */
#define DLMS_SET_RESPONSE_NORMAL 1
#define DLMS_SET_RESPONSE_DATABLOCK 2
#define DLMS_SET_RESPONSE_LAST_DATABLOCK 3
#define DLMS_SET_RESPONSE_LAST_DATABLOCK_WITH_LIST 4
#define DLMS_SET_RESPONSE_WITH_LIST 5
static const value_string dlms_set_response_names[] = {
    { DLMS_SET_RESPONSE_NORMAL, "set-response-normal" },
    { DLMS_SET_RESPONSE_DATABLOCK, "set-response-datablock" },
    { DLMS_SET_RESPONSE_LAST_DATABLOCK, "set-response-last-datablock" },
    { DLMS_SET_RESPONSE_LAST_DATABLOCK_WITH_LIST, "set-response-last-datablock-with-list" },
    { DLMS_SET_RESPONSE_WITH_LIST, "set-response-with-list" },
    { 0, 0 }
};

/* Choice values for an Action-Request */
#define DLMS_ACTION_REQUEST_NORMAL 1
#define DLMS_ACTION_REQUEST_NEXT_PBLOCK 2
#define DLMS_ACTION_REQUEST_WITH_LIST 3
#define DLMS_ACTION_REQUEST_WITH_FIRST_PBLOCK 4
#define DLMS_ACTION_REQUEST_WITH_LIST_AND_FIRST_PBLOCK 5
#define DLMS_ACTION_REQUEST_WITH_PBLOCK 6
static const value_string dlms_action_request_names[] = {
    { DLMS_ACTION_REQUEST_NORMAL, "action-request-normal" },
    { DLMS_ACTION_REQUEST_NEXT_PBLOCK, "action-request-next-pblock" },
    { DLMS_ACTION_REQUEST_WITH_LIST, "action-request-with-list" },
    { DLMS_ACTION_REQUEST_WITH_FIRST_PBLOCK, "action-request-with-first-pblock" },
    { DLMS_ACTION_REQUEST_WITH_LIST_AND_FIRST_PBLOCK, "action-request-with-list-and-first-pblock" },
    { DLMS_ACTION_REQUEST_WITH_PBLOCK, "action-request-with-pblock" },
    { 0, 0 }
};

/* Choice values for an Action-Response */
#define DLMS_ACTION_RESPONSE_NORMAL 1
#define DLMS_ACTION_RESPONSE_WITH_PBLOCK 2
#define DLMS_ACTION_RESPONSE_WITH_LIST 3
#define DLMS_ACTION_RESPONSE_NEXT_PBLOCK 4
static const value_string dlms_action_response_names[] = {
    { DLMS_ACTION_RESPONSE_NORMAL, "action-response-normal" },
    { DLMS_ACTION_RESPONSE_WITH_PBLOCK, "action-response-with-pblock" },
    { DLMS_ACTION_RESPONSE_WITH_LIST, "action-response-with-list" },
    { DLMS_ACTION_RESPONSE_NEXT_PBLOCK, "action-response-next-pblock" },
    { 0, 0 },
};

/* Choice values for an Access-Request-Specification */
#define DLMS_ACCESS_REQUEST_GET 1
#define DLMS_ACCESS_REQUEST_SET 2
#define DLMS_ACCESS_REQUEST_ACTION 3
#define DLMS_ACCESS_REQUEST_GET_WITH_SELECTION 4
#define DLMS_ACCESS_REQUEST_SET_WITH_SELECTION 5
static const value_string dlms_access_request_names[] = {
    { DLMS_ACCESS_REQUEST_GET, "access-request-get" },
    { DLMS_ACCESS_REQUEST_SET, "access-request-set" },
    { DLMS_ACCESS_REQUEST_ACTION, "access-request-action" },
    { DLMS_ACCESS_REQUEST_GET_WITH_SELECTION, "access-request-get-with-selection" },
    { DLMS_ACCESS_REQUEST_SET_WITH_SELECTION, "access-request-set-with-selection" },
    { 0, 0 },
};

/* Choice values for an Access-Response-Specification */
static const value_string dlms_access_response_names[] = {
    { 1, "access-response-get" },
    { 2, "access-response-set" },
    { 3, "access-response-action" },
    { 0, 0 },
};

/* Enumerated values for a Data-Access-Result */
static const value_string dlms_data_access_result_names[] = {
    { 0, "success" },
    { 1, "hardware-fault" },
    { 2, "temporary-failure" },
    { 3, "read-write-denied" },
    { 4, "object-undefined" },
    { 9, "object-class-inconsistent" },
    { 11, "object-unvailable" },
    { 12, "type-unmatched" },
    { 13, "scope-of-access-violated" },
    { 14, "data-block-unavailable" },
    { 15, "long-get-aborted" },
    { 16, "no-long-get-in-progress" },
    { 17, "long-set-aborted" },
    { 18, "no-long-set-in-progress" },
    { 19, "data-block-number-invalid" },
    { 250, "other-reason" },
    { 0, 0 }
};

/* Enumerated values for an Action-Result */
static const value_string dlms_action_result_names[] = {
    { 0, "success" },
    { 1, "hardware-fault" },
    { 2, "temporary-failure" },
    { 3, "read-write-denied" },
    { 4, "object-undefined" },
    { 9, "object-class-inconsistent" },
    { 11, "object-unavailable" },
    { 12, "type-unmatched" },
    { 13, "scope-of-access-violated" },
    { 14, "data-block-unavailable" },
    { 15, "long-action-aborted" },
    { 16, "no-long-action-in-progress" },
    { 250, "other-reason" },
    { 0, 0 }
};

/* Enumerated values for a state-error in an Exception-Response */
static const value_string dlms_state_error_names[] = {
    { 1, "service-not-allowed" },
    { 2, "service-unknown" },
    { 0, 0 }
};

/* Enumerated values for a service-error in an Exception-Response */
static const value_string dlms_service_error_names[] = {
    { 1, "operation-not-possible" },
    { 2, "service-not-supported" },
    { 3, "other-reason" },
    { 0, 0 }
};

/* Names of the values of the service-class bit in the Invoke-Id-And-Priority */
static const value_string dlms_service_class_names[] = {
    { 0, "unconfirmed" },
    { 1, "confirmed" },
    { 0, 0 }
};

/* Names of the values of the priority bit in the Invoke-Id-And-Priority */
static const value_string dlms_priority_names[] = {
    { 0, "normal" },
    { 1, "high" },
    { 0, 0 }
};

/* Names of the values of the self-descriptive bit in the Long-Invoke-Id-And-Priority */
static const value_string dlms_self_descriptive_names[] = {
    { 0, "not-self-descriptive" },
    { 1, "self-descriptive" },
    { 0, 0 }
};

/* Names of the values of the processing-option bit in the Long-Invoke-Id-And-Priority */
static const value_string dlms_processing_option_names[] = {
    { 0, "continue-on-error" },
    { 1, "break-on-error" },
    { 0, 0 }
};

/* Names of the values of the Application-context-name object identifiers 2.16.756.5.8.1.x */
static const val64_string dlms_application_context_names[] = {
    { 0x60857405080101, "logical-name-referencing-no-ciphering" },
    { 0x60857405080102, "short-name-referencing-no-ciphering" },
    { 0x60857405080103, "logical-name-referencing-with-ciphering" },
    { 0x60857405080104, "short-name-referencing-with-ciphering" },
    { 0, 0 }
};

/* Names of the values of the Mechanism-name object identifiers 2.16.756.5.8.2.x */
static const val64_string dlms_mechanism_names[] = {
    { 0x60857405080200, "lowest-level-security" },
    { 0x60857405080201, "low-level-security" },
    { 0x60857405080202, "high-level-security" },
    { 0x60857405080203, "high-level-security-MD5" },
    { 0x60857405080204, "high-level-security-SHA1" },
    { 0x60857405080205, "high-level-security-GMAC" },
    { 0x60857405080206, "high-level-security-SHA256" },
    { 0x60857405080207, "high-level-security-ECDSA" },
    { 0, 0 }
};

/* Bit flags of the security control byte */
#define DLMS_SECURITY_CONTROL_AUTHENTICATION 0x10
#define DLMS_SECURITY_CONTROL_ENCRYPTION 0x20
#define DLMS_SECURITY_CONTROL_COMPRESSION 0x80

/* HDLC frame names for the control field values (with the RRR, P/F, and SSS bits masked off) */
static const value_string dlms_hdlc_frame_names[] = {
    { 0x00, "I (Information)" },
    { 0x01, "RR (Receive Ready)" },
    { 0x03, "UI (Unnumbered Information)" },
    { 0x05, "RNR (Receive Not Ready)" },
    { 0x0f, "DM (Disconnected Mode)" },
    { 0x43, "DISC (Disconnect)" },
    { 0x63, "UA (Unnumbered Acknowledge)" },
    { 0x83, "SNRM (Set Normal Response Mode)" },
    { 0x87, "FRMR (Frame Reject)" },
    { 0, 0 }
};

/* Structure with the names of a DLMS/COSEM class */
struct dlms_cosem_class {
    const char *name;
    const char *attributes[18]; /* index 0 is attribute 2 (attribute 1 is always "logical_name") */
    const char *methods[11]; /* index 0 is method 1 */
};
typedef struct dlms_cosem_class dlms_cosem_class;

/* Get the DLMS/COSEM class with the specified class_id */
static const dlms_cosem_class *
dlms_get_class(int class_id) {
    const short ids[] = {
        1, /* data */
        3, /* register */
        4, /* extended register */
        5, /* demand register */
        7, /* profile generic */
        8, /* clock */
        9, /* script table */
        10, /* schedule */
        11, /* special days table */
        15, /* association ln */
        17, /* sap assignment */
        18, /* image transfer */
        20, /* activity calendar */
        21, /* register monitor */
        22, /* single action schedule */
        23, /* iec hdlc setup */
        30, /* data protection */
        64, /* security setup */
        70, /* disconnect control */
        71, /* limiter */
        104, /* zigbee network control */
        111, /* account */
        112, /* credit */
        113, /* charge */
        115, /* token gateway */
        9000, /* extended data */
    };
    static const struct dlms_cosem_class classes[] = {
        {
            "data",
            {
                "value"
            }
        },{
            "register",
            {
                "value",
                "scaler_unit"
            },{
                "reset"
            }
        },{
            "extended_register",
            {
                "value",
                "scaler_unit",
                "status",
                "capture_time"
            },{
                "reset"
            }
        },{
            "demand_register",
            {
                "current_average_value",
                "last_average_value",
                "scaler_unit",
                "status",
                "capture_time",
                "start_time_current",
                "period",
                "number_of_periods"
            },{
                "reset",
                "next_period"
            }
        },{
            "profile_generic",
            {
                "buffer",
                "capture_objects",
                "capture_period",
                "sort_method",
                "sort_object",
                "entries_in_use",
                "profile_entries"
            },{
                "reset",
                "capture",
                "get_buffer_by_range",
                "get_buffer_by_index"
            }
        },{
            "clock",
            {
                "time",
                "time_zone",
                "status",
                "daylight_savings_begin",
                "daylight_savings_end",
                "daylight_savings_deviation",
                "daylight_savings_enabled",
                "clock_base"
            },{
                "adjust_to_quarter",
                "adjust_to_measuring_period",
                "adjust_to_minute",
                "adjust_to_preset_time",
                "preset_adjusting_time",
                "shift_time"
            }
        },{
            "script_table",
            {
                "scripts"
            },{
                "execute"
            }
        },{
            "schedule",
            {
                "entries"
            },{
                "enable_disable",
                "insert",
                "delete"
            }
        },{
            "special_days_table",
            {
                "entries"
            },{
                "insert",
                "delete"
            }
        },{
            "association_ln",
            {
                "object_list",
                "associated_partners_id",
                "application_context_name",
                "xdlms_context_info",
                "authentication_mechanism_name",
                "secret",
                "association_status",
                "security_setup_reference",
                "user_list",
                "current_user"
            },{
                "reply_to_hls_authentication",
                "change_hls_secret",
                "add_object",
                "remove_object",
                "add_user",
                "remove_user"
            }
        },{
            "sap_assignment",
            {
                "sap_assignment_list"
            },{
                "connect_logical_devices"
            }
        },{
            "image_transfer",
            {
                "image_block_size",
                "image_transferred_blocks_status",
                "image_first_not_transferred_block_number",
                "image_transfer_enabled",
                "image_transfer_status",
                "image_to_activate_info"
            },{
                "image_transfer_initiate",
                "image_block_transfer",
                "image_verify",
                "image_activate"
            }
        },{
            "activity_calendar",
            {
                "calendar_name_active",
                "season_profile_active",
                "week_profile_table_active",
                "day_profile_table_active",
                "calendar_name_passive",
                "season_profile_passive",
                "week_profile_table_passive",
                "day_profile_table_passive",
                "active_passive_calendar_time"
            },{
                "active_passive_calendar"
            }
        },{
            "register_monitor",
            {
                "thresholds",
                "monitored_value",
                "actions"
            }
        },{
            "single_action_schedule",
            {
                "executed_script",
                "type",
                "execution_time"
            }
        },{
            "iec_hdlc_setup",
            {
                "comm_speed",
                "window_size_transmit",
                "window_size_receive",
                "max_info_field_length_transmit",
                "max_info_field_length_receive",
                "inter_octet_time_out",
                "inactivity_time_out",
                "device_address"
            }
        },{
            "data_protection",
            {
                "protection_buffer",
                "protection_object_list",
                "protection_parameters_get",
                "protection_parameters_set",
                "required_protection"
            },{
                "get_protected_attributes",
                "set_protected_attributes",
                "invoke_protected_method"
            }
        },{
            "security_setup",
            {
                "security_policy",
                "security_suite",
                "client_system_title",
                "server_system_title",
                "certificates"
            },{
                "security_activate",
                "key_transfer",
                "key_agreement",
                "generate_key_pair",
                "generate_certificate_request",
                "import_certificate",
                "export_certificate",
                "remove_certificate"
            }
        },{
            "disconnect_control",
            {
                "output_state",
                "control_state",
                "control_mode"
            },{
                "remote_disconnect",
                "remote_reconnect"
            }
        },{
            "limiter",
            {
                "monitored_value",
                "threshold_active",
                "threshold_normal",
                "threshold_emergency",
                "min_over_threshold_duration",
                "min_under_threshold_duration",
                "emergency_profile",
                "emergency_profile_group_id_list",
                "emergency_profile_active",
                "actions"
            }
        },{
            "zigbee_network_control",
            {
                "enable_disable_joining",
                "join_timeout",
                "active_devices"
            },{
                "register_device",
                "unregister_device",
                "unregister_all_devices",
                "backup_pan",
                "restore_pan",
                "identify_device",
                "remove_mirror",
                "update_network_key",
                "update_link_key",
                "create_pan",
                "remove_pan"
            }
        },{
            "account",
            {
                "account_mode_and_status",
                "current_credit_in_use",
                "current_credit_status",
                "available_credit",
                "amount_to_clear",
                "clearance_threshold",
                "aggregated_debt",
                "credit_reference_list",
                "charge_reference_list",
                "credit_charge_configuration",
                "token_gateway_configuration",
                "account_activation_time",
                "account_closure_time",
                "currency",
                "low_credit_threshold",
                "next_credit_available_threshold",
                "max_provision",
                "max_provision_period"
            },{
                "activate_account",
                "close_account",
                "reset_account"
            }
        },{
            "credit",
            {
                "current_credit_amount",
                "credit_type",
                "priority",
                "warning_threshold",
                "limit",
                "credit_configuration",
                "credit_status",
                "preset_credit_amount",
                "credit_available_threshold",
                "period"
            },{
                "update_amount",
                "set_amount_to_value",
                "invoke_credit"
            }
        },{
            "charge",
            {
                "total_amount_paid",
                "charge_type",
                "priority",
                "unit_charge_active",
                "unit_charge_passive",
                "unit_charge_activation_time",
                "period",
                "charge_configuration",
                "last_collection_time",
                "last_collection_amount",
                "total_amount_remaining",
                "proportion"
            },{
                "update_unit_charge",
                "activate_passive_unit_charge",
                "collect",
                "update_total_amount_remaining",
                "set_total_amount_remaining"
            }
        },{
            "token_gateway",
            {
                "token",
                "token_time",
                "token_description",
                "token_delivery_method",
                "token_status"
            },{
                "enter"
            }
        },{
            "extended_data",
            {
                "value_active",
                "scaler_unit_active",
                "value_passive",
                "scaler_unit_passive",
                "activate_passive_value_time"
            },{
                "reset",
                "activate_passive_value"
            }
        }
    };
    unsigned i;

    for (i = 0; i < array_length(ids); i++) {
        if (ids[i] == class_id) {
            return &classes[i];
        }
    }

    return 0;
}

static const char *
dlms_get_attribute_name(const dlms_cosem_class *c, int attribute_id) {
    if (attribute_id > 1 && attribute_id < array_length(c->attributes) + 2) {
        return c->attributes[attribute_id - 2];
    } else if (attribute_id == 1) {
        return "logical_name";
    }
    return 0;
}

static const char *
dlms_get_method_name(const dlms_cosem_class *c, int method_id) {
    if (method_id > 0 && method_id < array_length(c->methods) + 1) {
        return c->methods[method_id - 1];
    }
    return 0;
}

/* The DLMS protocol handle */
static int dlms_proto;

/* The DLMS header_field_info (hfi) structures */
static struct {
    /* HDLC */
    header_field_info hdlc_flag; /* opening/closing flag */
    header_field_info hdlc_type; /* frame format type */
    header_field_info hdlc_segmentation; /* frame format segmentation bit */
    header_field_info hdlc_length; /* frame format length sub-field */
    header_field_info hdlc_address; /* destination/source address */
    header_field_info hdlc_frame_i; /* control field & 0x01 (I) */
    header_field_info hdlc_frame_rr_rnr; /* control field & 0x0f (RR or RNR) */
    header_field_info hdlc_frame_other; /* control field & 0xef (all other) */
    header_field_info hdlc_pf; /* poll/final bit */
    header_field_info hdlc_rsn; /* receive sequence number N(R) */
    header_field_info hdlc_ssn; /* send sequence number N(S) */
    header_field_info hdlc_hcs; /* header check sequence */
    header_field_info hdlc_fcs; /* frame check sequence */
    header_field_info hdlc_parameter; /* information field parameter */
    header_field_info hdlc_llc; /* LLC header */
    /* IEC 4-32 LLC */
    header_field_info iec432llc;
    /* Wrapper Protocol Data Unit (WPDU) */
    header_field_info wrapper_header;
    /* APDU */
    header_field_info apdu;
    header_field_info get_request;
    header_field_info set_request;
    header_field_info action_request;
    header_field_info get_response;
    header_field_info set_response;
    header_field_info action_response;
    header_field_info access_request;
    header_field_info access_response;
    header_field_info class_id;
    header_field_info instance_id;
    header_field_info attribute_id;
    header_field_info method_id;
    header_field_info access_selector;
    header_field_info data_access_result;
    header_field_info action_result;
    header_field_info block_number;
    header_field_info last_block;
    header_field_info type_description;
    header_field_info data;
    header_field_info date_time;
    header_field_info length;
    header_field_info state_error;
    header_field_info service_error;
    /* Invoke-Id-And-Priority */
    header_field_info invoke_id;
    header_field_info service_class;
    header_field_info priority;
    /* Long-Invoke-Id-And-Priority */
    header_field_info long_invoke_id;
    header_field_info long_self_descriptive;
    header_field_info long_processing_option;
    header_field_info long_service_class;
    header_field_info long_priority;
    /* Security */
    header_field_info security_suite_id;
    header_field_info authentication;
    header_field_info encryption;
    header_field_info key_set;
    header_field_info compression;
    header_field_info invocation_counter;
    header_field_info authentication_tag;
    /* AARQ/AARE */
    header_field_info protocol_version;
    header_field_info application_context_name;
    header_field_info called_ap_title;
    header_field_info called_ae_qualifier;
    header_field_info called_ap_invocation_id;
    header_field_info called_ae_invocation_id;
    header_field_info calling_ap_title;
    header_field_info calling_ae_qualifier;
    header_field_info calling_ap_invocation_id;
    header_field_info calling_ae_invocation_id;
    header_field_info sender_acse_requirements;
    header_field_info mechanism_name;
    header_field_info calling_authentication_value;
    header_field_info implementation_information;
    header_field_info user_information;
    header_field_info result;
    header_field_info result_source_diagnostic;
    header_field_info responding_ap_title;
    header_field_info responding_ae_qualifier;
    header_field_info responding_ap_invocation_id;
    header_field_info responding_ae_invocation_id;
    header_field_info responder_acse_requirements;
    header_field_info responding_authentication_value;
    /* InitiateRequest/InitiateResponse */
    header_field_info dedicated_key;
    header_field_info response_allowed;
    header_field_info quality_of_service;
    header_field_info dlms_version_number;
    header_field_info max_receive_pdu_size;
    header_field_info vaa_name;
    /* Conformance bits */
    header_field_info conformance_general_protection;
    header_field_info conformance_general_block_transfer;
    header_field_info conformance_read;
    header_field_info conformance_write;
    header_field_info conformance_unconfirmed_write;
    header_field_info conformance_attribute0_supported_with_set;
    header_field_info conformance_priority_mgmt_supported;
    header_field_info conformance_attribute0_supported_with_get;
    header_field_info conformance_block_transfer_with_get_or_read;
    header_field_info conformance_block_transfer_with_set_or_write;
    header_field_info conformance_block_transfer_with_action;
    header_field_info conformance_multiple_references;
    header_field_info conformance_information_report;
    header_field_info conformance_data_notification;
    header_field_info conformance_access;
    header_field_info conformance_parameterized_access;
    header_field_info conformance_get;
    header_field_info conformance_set;
    header_field_info conformance_selective_access;
    header_field_info conformance_event_notification;
    header_field_info conformance_action;
    /* fragment_items */
    header_field_info fragments;
    header_field_info fragment;
    header_field_info fragment_overlap;
    header_field_info fragment_overlap_conflict;
    header_field_info fragment_multiple_tails;
    header_field_info fragment_too_long_fragment;
    header_field_info fragment_error;
    header_field_info fragment_count;
    header_field_info reassembled_in;
    header_field_info reassembled_length;
    header_field_info reassembled_data;
} dlms_hfi HFI_INIT(dlms_proto) = {
    /* HDLC */
    { "Flag", "dlms.hdlc.flag", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Type", "dlms.hdlc.type", FT_UINT16, BASE_DEC, 0, 0xf000, 0, HFILL },
    { "Segmentation", "dlms.hdlc.segmentation", FT_UINT16, BASE_DEC, 0, 0x0800, 0, HFILL },
    { "Length", "dlms.hdlc.length", FT_UINT16, BASE_DEC, 0, 0x07ff, 0, HFILL },
    { "Upper HDLC Address", "dlms.hdlc.address", FT_UINT8, BASE_DEC, 0, 0xfe, 0, HFILL },
    { "Frame", "dlms.hdlc.frame", FT_UINT8, BASE_DEC, dlms_hdlc_frame_names, 0x01, 0, HFILL },
    { "Frame", "dlms.hdlc.frame", FT_UINT8, BASE_DEC, dlms_hdlc_frame_names, 0x0f, 0, HFILL },
    { "Frame", "dlms.hdlc.frame", FT_UINT8, BASE_DEC, dlms_hdlc_frame_names, 0xef, 0, HFILL },
    { "Poll/Final", "dlms.hdlc.pf", FT_UINT8, BASE_DEC, 0, 0x10, 0, HFILL },
    { "Receive Sequence Number", "dlms.hdlc.rsn", FT_UINT8, BASE_DEC, 0, 0xe0, 0, HFILL },
    { "Send Sequence Number", "dlms.hdlc.ssn", FT_UINT8, BASE_DEC, 0, 0x0e, 0, HFILL },
    { "Header Check Sequence", "dlms.hdlc.hcs", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Frame Check Sequence", "dlms.hdlc.fcs", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Parameter", "dlms.hdlc.parameter", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "LLC Header", "dlms.hdlc.llc", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    /* IEC 4-32 LLC */
    { "IEC 4-32 LLC Header", "dlms.iec432llc", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    /* Wrapper Protocol Data Unit (WPDU) */
    { "Wrapper Header", "dlms.wrapper", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    /* APDU */
    { "APDU", "dlms.apdu", FT_UINT8, BASE_DEC, dlms_apdu_names, 0, 0, HFILL },
    { "Get Request", "dlms.get_request", FT_UINT8, BASE_DEC, dlms_get_request_names, 0, 0, HFILL },
    { "Set Request", "dlms.set_request", FT_UINT8, BASE_DEC, dlms_set_request_names, 0, 0, HFILL },
    { "Action Request", "dlms.action_request", FT_UINT8, BASE_DEC, dlms_action_request_names, 0, 0, HFILL },
    { "Get Response", "dlms.get_response", FT_UINT8, BASE_DEC, dlms_get_response_names, 0, 0, HFILL },
    { "Set Response", "dlms.set_response", FT_UINT8, BASE_DEC, dlms_set_response_names, 0, 0, HFILL },
    { "Action Response", "dlms.action_response", FT_UINT8, BASE_DEC, dlms_action_response_names, 0, 0, HFILL },
    { "Access Request", "dlms.action_request", FT_UINT8, BASE_DEC, dlms_access_request_names, 0, 0, HFILL },
    { "Access Response", "dlms.action_response", FT_UINT8, BASE_DEC, dlms_access_response_names, 0, 0, HFILL },
    { "Class Id", "dlms.class_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Instance Id", "dlms.instance_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Attribute Id", "dlms.attribute_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Method Id", "dlms.method_id", FT_UINT8, BASE_DEC, 0, 0, 0, HFILL },
    { "Access Selector", "dlms.access_selector", FT_UINT8, BASE_DEC, 0, 0, 0, HFILL },
    { "Data Access Result", "dlms.data_access_result", FT_UINT8, BASE_DEC, dlms_data_access_result_names, 0, 0, HFILL },
    { "Action Result", "dlms.action_result", FT_UINT8, BASE_DEC, dlms_action_result_names, 0, 0, HFILL },
    { "Block Number", "dlms.block_number", FT_UINT32, BASE_DEC, 0, 0, 0, HFILL },
    { "Last Block", "dlms.last_block", FT_BOOLEAN, BASE_DEC, 0, 0, 0, HFILL },
    { "Type Description", "dlms.type_description", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Data", "dlms.data", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Date-Time", "dlms.date_time", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Length", "dlms.length", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "State Error", "dlms.state_error", FT_UINT8, BASE_DEC, dlms_state_error_names, 0, 0, HFILL },
    { "Service Error", "dlms.service_error", FT_UINT8, BASE_DEC, dlms_service_error_names, 0, 0, HFILL },
    /* Invoke-Id-And-Priority */
    { "Invoke Id", "dlms.invoke_id", FT_UINT8, BASE_DEC, 0, 0x0f, 0, HFILL },
    { "Service Class", "dlms.service_class", FT_UINT8, BASE_DEC, dlms_service_class_names, 0x40, 0, HFILL },
    { "Priority", "dlms.priority", FT_UINT8, BASE_DEC, dlms_priority_names, 0x80, 0, HFILL },
    /* Long-Invoke-Id-And-Priority */
    { "Long Invoke Id", "dlms.long_invoke_id", FT_UINT32, BASE_DEC, 0, 0xffffff, 0, HFILL },
    { "Self Descriptive", "dlms.self_descriptive", FT_UINT32, BASE_DEC, dlms_self_descriptive_names, 0x10000000, 0, HFILL },
    { "Processing Option", "dlms.processing_option", FT_UINT32, BASE_DEC, dlms_processing_option_names, 0x20000000, 0, HFILL },
    { "Service Class", "dlms.service_class", FT_UINT32, BASE_DEC, dlms_service_class_names, 0x40000000, 0, HFILL },
    { "Priority", "dlms.priority", FT_UINT32, BASE_DEC, dlms_priority_names, 0x80000000, 0, HFILL },
    /* Security */
    { "Security Suite Id", "dlms.security_suite_id", FT_UINT8, BASE_DEC, 0, 0xf, 0, HFILL },
    { "Authentication", "dlms.authentication", FT_UINT8, BASE_DEC, 0, DLMS_SECURITY_CONTROL_AUTHENTICATION, 0, HFILL },
    { "Encryption", "dlms.encryption", FT_UINT8, BASE_DEC, 0, DLMS_SECURITY_CONTROL_ENCRYPTION, 0, HFILL },
    { "Key Set", "dlms.key_set", FT_UINT8, BASE_DEC, 0, 0x40, 0, HFILL },
    { "Compression", "dlms.compression", FT_UINT8, BASE_DEC, 0, DLMS_SECURITY_CONTROL_COMPRESSION, 0, HFILL },
    { "Invocation Counter", "dlms.invocation_counter", FT_UINT32, BASE_DEC, 0, 0, 0, HFILL },
    { "Authentication Tag", "dlms.authentication_tag", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    /* AARQ/AARE */
    { "Protocol Version", "dlms.protocol_version", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Application Context Name", "dlms.application_context_name", FT_UINT56, BASE_VAL64_STRING, dlms_application_context_names, 0, 0, HFILL },
    { "Called AP Title", "dlms.called_ap_title", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Called AE Qualifier", "dlms.called_ae_qualifier", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Called AP Invocation Id", "dlms.called_ap_invocation_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Called AE Invocation Id", "dlms.called_ae_invocation_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Calling AP Title", "dlms.calling_ap_title", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Calling AE Qualifier", "dlms.calling_ae_qualifier", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Calling AP Invocation Id", "dlms.calling_ap_invocation_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Calling AE Invocation Id", "dlms.calling_ae_invocation_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Sender ACSE Requirements", "dlms.sender_acse_requirements", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Mechanism Name", "dlms.mechanism_name", FT_UINT56, BASE_VAL64_STRING, dlms_mechanism_names, 0, 0, HFILL },
    { "Calling Authentication Value", "dlms.calling_authentication_value", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Implementation Information", "dlms.implementation_information", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "User Information", "dlms.user_information", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Result", "dlms.result", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Result Source Diagnostic", "dlms.result_source_diagnostic", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Responding AP Title", "dlms.responding_ap_title", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Responding AE Qualifier", "dlms.responding_ae_qualifier", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Responding AP Invocation Id", "dlms.responding_ap_invocation_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Responding AE Invocation Id", "dlms.responding_ae_invocation_id", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Responder ACSE Requirements", "dlms.responder_acse_requirements", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Responding Authentication Value", "dlms.responding_authentication_value", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    /* InitiateRequest/InitiateResponse */
    { "Dedicated Key", "dlms.dedicated_key", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Response Allowed", "dlms.response_allowed", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Quality of Service", "dlms.quality_of_service", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "DLMS Version Number", "dlms.dlms_version_number", FT_UINT8, BASE_DEC, 0, 0, 0, HFILL },
    { "Max Receive PDU Size", "dlms.max_receive_pdu_size", FT_UINT16, BASE_DEC, 0, 0, 0, HFILL },
    { "VAA Name", "dlms.vaa_name", FT_UINT16, BASE_DEC, 0, 0, 0, HFILL },
    /* proposed-conformance and negotiated-conformance bits */
    { "general-protection", "dlms.conformance.general_protection", FT_UINT24, BASE_DEC, 0, 0x400000, 0, HFILL },
    { "general-block-transfer", "dlms.conformance.general_block_transfer", FT_UINT24, BASE_DEC, 0, 0x200000, 0, HFILL },
    { "read", "dlms.conformance.read", FT_UINT24, BASE_DEC, 0, 0x100000, 0, HFILL },
    { "write", "dlms.conformance.write", FT_UINT24, BASE_DEC, 0, 0x080000, 0, HFILL },
    { "unconfirmed-write", "dlms.conformance.unconfirmed_write", FT_UINT24, BASE_DEC, 0, 0x040000, 0, HFILL },
    { "attribute0-supported-with-set", "dlms.conformance.attribute0_supported_with_set", FT_UINT24, BASE_DEC, 0, 0x008000, 0, HFILL },
    { "priority-mgmt-supported", "dlms.conformance.priority_mgmt_supported", FT_UINT24, BASE_DEC, 0, 0x004000, 0, HFILL },
    { "attribute0-supported-with-get", "dlms.conformance.attribute0_supported_with_get", FT_UINT24, BASE_DEC, 0, 0x002000, 0, HFILL },
    { "block-transfer-with-get-or-read", "dlms.conformance.block_transfer_with_get_or_read", FT_UINT24, BASE_DEC, 0, 0x001000, 0, HFILL },
    { "block-transfer-with-set-or-write", "dlms.conformance.block_transfer_with_set_or_write", FT_UINT24, BASE_DEC, 0, 0x000800, 0, HFILL },
    { "block-transfer-with-action", "dlms.conformance.block_transfer_with_action", FT_UINT24, BASE_DEC, 0, 0x000400, 0, HFILL },
    { "multiple-references", "dlms.conformance.multiple_references", FT_UINT24, BASE_DEC, 0, 0x000200, 0, HFILL },
    { "information-report", "dlms.conformance.information_report", FT_UINT24, BASE_DEC, 0, 0x000100, 0, HFILL },
    { "data-notification", "dlms.conformance.data_notification", FT_UINT24, BASE_DEC, 0, 0x000080, 0, HFILL },
    { "access", "dlms.conformance.access", FT_UINT24, BASE_DEC, 0, 0x000040, 0, HFILL },
    { "parameterized-access", "dlms.conformance.parameterized_access", FT_UINT24, BASE_DEC, 0, 0x000020, 0, HFILL },
    { "get", "dlms.conformance.get", FT_UINT24, BASE_DEC, 0, 0x000010, 0, HFILL },
    { "set", "dlms.conformance.set", FT_UINT24, BASE_DEC, 0, 0x000008, 0, HFILL },
    { "selective-access", "dlms.conformance.selective_access", FT_UINT24, BASE_DEC, 0, 0x000004, 0, HFILL },
    { "event-notification", "dlms.conformance.event_notification", FT_UINT24, BASE_DEC, 0, 0x000002, 0, HFILL },
    { "action", "dlms.conformance.action", FT_UINT24, BASE_DEC, 0, 0x000001, 0, HFILL },
    /* fragment_items */
    { "Fragments", "dlms.fragments", FT_NONE, BASE_NONE, 0, 0, 0, HFILL },
    { "Fragment", "dlms.fragment", FT_FRAMENUM, BASE_NONE, 0, 0, 0, HFILL },
    { "Fragment Overlap", "dlms.fragment.overlap", FT_BOOLEAN, 0, 0, 0, 0, HFILL },
    { "Fragment Conflict", "dlms.fragment.conflict", FT_BOOLEAN, 0, 0, 0, 0, HFILL },
    { "Fragment Multiple", "dlms.fragment.multiple", FT_BOOLEAN, 0, 0, 0, 0, HFILL },
    { "Fragment Too Long", "dlms.fragment.too_long", FT_BOOLEAN, 0, 0, 0, 0, HFILL },
    { "Fragment Error", "dlms.fragment.error", FT_FRAMENUM, BASE_NONE, 0, 0, 0, HFILL },
    { "Fragment Count", "dlms.fragment.count", FT_UINT32, BASE_DEC, 0, 0, 0, HFILL },
    { "Reassembled In", "dlms.reassembled_in", FT_FRAMENUM, BASE_NONE, 0, 0, 0, HFILL },
    { "Reassembled Length", "dlms.reassembled_length", FT_UINT32, BASE_DEC, 0, 0, 0, HFILL },
    { "Reassembled Data", "dlms.reassembled_data", FT_BYTES, SEP_SPACE, 0, 0, 0, HFILL },
};

/* Protocol subtree (ett) indices */
static struct {
    gint dlms;
    gint hdlc;
    gint hdlc_format;
    gint hdlc_address;
    gint hdlc_control;
    gint hdlc_information;
    gint invoke_id_and_priority;
    gint access_request_specification;
    gint access_request;
    gint access_response_specification;
    gint access_response;
    gint cosem_attribute_or_method_descriptor;
    gint selective_access_descriptor;
    gint composite_data;
    gint user_information; /* AARQ and AARE user-information field */
    gint conformance; /* InitiateRequest proposed-conformance and InitiateResponse negotiated-confirmance */
    gint datablock;
    gint data;
    gint security_control;
    gint protected_apdu;
    /* fragment_items */
    gint fragment;
    gint fragments;
} dlms_ett;

/* Expert information (ei) fields */
static struct {
    expert_field no_success;
    expert_field not_implemented;
    expert_field check_sequence; /* bad HDLC check sequence (HCS or FCS) value */
} dlms_ei;

/*
 * The reassembly table is used for reassembling both
 * HDLC I frame segments and DLMS APDU datablocks.
 * The reassembly id is used as hash key to distinguish between the two.
 */
static reassembly_table dlms_reassembly_table;

enum {
    /* Do not use 0 as id because that would return a NULL key */
    DLMS_REASSEMBLY_ID_HDLC = 1,
    DLMS_REASSEMBLY_ID_DATABLOCK,
};

static guint
dlms_reassembly_hash_func(gconstpointer key)
{
    return (gsize)key;
}

static gint
dlms_reassembly_equal_func(gconstpointer key1, gconstpointer key2)
{
    return key1 == key2;
}

static gpointer
dlms_reassembly_key_func(const packet_info *pinfo, guint32 id, const void *data)
{
    return (gpointer)(gsize)id;
}

static void
dlms_reassembly_free_key_func(gpointer ptr)
{
}

static const fragment_items dlms_fragment_items = {
    &dlms_ett.fragment,
    &dlms_ett.fragments,
    &dlms_hfi.fragments.id,
    &dlms_hfi.fragment.id,
    &dlms_hfi.fragment_overlap.id,
    &dlms_hfi.fragment_overlap_conflict.id,
    &dlms_hfi.fragment_multiple_tails.id,
    &dlms_hfi.fragment_too_long_fragment.id,
    &dlms_hfi.fragment_error.id,
    &dlms_hfi.fragment_count.id,
    &dlms_hfi.reassembled_in.id,
    &dlms_hfi.reassembled_length.id,
    &dlms_hfi.reassembled_data.id,
    "Fragments"
};

/*
 * Decryption.
 * The security material required for decryption is inserted by the user
 * in the GUI preference dialog (Edit -> Preferences -> Protocols -> DLMS -> Keys).
 * Each entry specifies one set of security materials to try when decryting.
 * The entries are tried in order. The first one that decrypts successfully is used.
 */

typedef struct _dlms_key_entry_t {
    unsigned char system_title[8];
    unsigned char guek[16];  /* global unicast encryption key */
    unsigned char gak[16];  /* global authentication key */
} dlms_key_entry_t;

static gboolean
dlms_keys_check(void *record, const char *ptr, unsigned len, const void *chk_data, const void *fld_data, char **err) {
    unsigned expected_len = (size_t)chk_data;
    if (len != expected_len) {
        *err = g_strdup_printf("Invalid field length %u (expecting %u)", len, expected_len);
        return FALSE;
    }
    return TRUE;
}

static void
dlms_keys_set(void *record, const char *ptr, unsigned len, const void *set_data, const void *fld_data) {
    unsigned offset = (size_t)fld_data;
    memcpy((char *)record + offset, ptr, len);
}

static void
dlms_keys_tostring(void *record, char **out_ptr, unsigned *out_len, const void *tostr_data, const void *fld_data) {
    unsigned len = (size_t)tostr_data;
    unsigned offset = (size_t)fld_data;
    *out_ptr = g_memdup((char *)record + offset, len);
    *out_len = len;
}

static uat_field_t dlms_keys_uat_fields[] = {
#define DLMS_KEYS_UAT_FIELD(field, title, description) { #field, title, PT_TXTMOD_HEXBYTES, { dlms_keys_check, dlms_keys_set, dlms_keys_tostring }, { (void *)sizeof(((dlms_key_entry_t *)0)->field), 0, (void *)sizeof(((dlms_key_entry_t *)0)->field) }, (void *)offsetof(dlms_key_entry_t, field), description, FLDFILL }
    DLMS_KEYS_UAT_FIELD(system_title, "System Title", "8 bytes in hexadecimal format"),
    DLMS_KEYS_UAT_FIELD(guek, "Global Unicast Encryption Key", "16 bytes in hexadecimal format"),
    DLMS_KEYS_UAT_FIELD(gak, "Global Authentication Key", "16 bytes in hexadecimal format"),
    UAT_END_FIELDS
#undef DLMS_KEYS_UAT_FIELD
};

static dlms_key_entry_t *dlms_key_entries;
static guint dlms_key_entry_count;
static gcry_cipher_hd_t dlms_gcry_cipher_hd;

static tvbuff_t *
dlms_decrypt(tvbuff_t *tvb, packet_info *pinfo, gint offset, gint length, guint security_control, guint invocation_counter) {
    tvbuff_t *decrypted_tvb = NULL;
    unsigned char iv[12]; /* initialization vector */
    unsigned char ad[17]; /* associated data */
    void *plaintext;
    const void *ciphertext, *tag;
    const int tag_length = 12;
    unsigned i, j;

    plaintext = wmem_alloc(pinfo->pool, length);
    ciphertext = tvb_get_ptr(tvb, offset, length);
    tag = tvb_get_ptr(tvb, offset + length, tag_length);

    iv[8] = invocation_counter >> 24;
    iv[9] = invocation_counter >> 16;
    iv[10] = invocation_counter >> 8;
    iv[11] = invocation_counter;

    ad[0] = security_control;

    for (i = 0; i < dlms_key_entry_count; i++) {
        gcry_cipher_setkey(dlms_gcry_cipher_hd, dlms_key_entries[i].guek, sizeof(dlms_key_entries[i].guek));

        for (j = 0; j < sizeof(dlms_key_entries[i].system_title); j++) {
            iv[j] = dlms_key_entries[i].system_title[j];
        }
        gcry_cipher_setiv(dlms_gcry_cipher_hd, iv, sizeof(iv));

        for (j = 0; j < sizeof(dlms_key_entries[i].gak); j++) {
            ad[j + 1] = dlms_key_entries[i].gak[j];
        }
        gcry_cipher_authenticate(dlms_gcry_cipher_hd, ad, sizeof(ad));

        gcry_cipher_decrypt(dlms_gcry_cipher_hd, plaintext, length, ciphertext, length);
        if (!gcry_cipher_checktag(dlms_gcry_cipher_hd, tag, tag_length)) {
            decrypted_tvb = tvb_new_child_real_data(tvb, plaintext, length, length);
            add_new_data_source(pinfo, decrypted_tvb, "Decrypted data");
            break;
        }
    }

    return decrypted_tvb;
}

static void
dlms_dissect_invoke_id_and_priority(proto_tree *tree, tvbuff_t *tvb, gint *offset)
{
    proto_tree *subtree;

    subtree = proto_tree_add_subtree(tree, tvb, *offset, 1, dlms_ett.invoke_id_and_priority, 0, "Invoke Id And Priority");
    proto_tree_add_item(subtree, &dlms_hfi.invoke_id, tvb, *offset, 1, ENC_NA);
    proto_tree_add_item(subtree, &dlms_hfi.service_class, tvb, *offset, 1, ENC_NA);
    proto_tree_add_item(subtree, &dlms_hfi.priority, tvb, *offset, 1, ENC_NA);
    *offset += 1;
}

static void
dlms_dissect_long_invoke_id_and_priority(proto_tree *tree, tvbuff_t *tvb, gint *offset)
{
    proto_tree *subtree;

    subtree = proto_tree_add_subtree(tree, tvb, *offset, 4, dlms_ett.invoke_id_and_priority, 0, "Long Invoke Id And Priority");
    proto_tree_add_item(subtree, &dlms_hfi.long_invoke_id, tvb, *offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(subtree, &dlms_hfi.long_self_descriptive, tvb, *offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(subtree, &dlms_hfi.long_processing_option, tvb, *offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(subtree, &dlms_hfi.long_service_class, tvb, *offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(subtree, &dlms_hfi.long_priority, tvb, *offset, 4, ENC_BIG_ENDIAN);
    *offset += 4;
}

static void
dlms_dissect_cosem_attribute_or_method_descriptor(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset, int is_attribute)
{
    unsigned class_id, attribute_method_id;
    const dlms_cosem_class *cosem_class;
    const char *attribute_method_name;
    const gchar *instance_name;
    proto_tree *subtree;
    proto_item *item;

    class_id = tvb_get_ntohs(tvb, *offset);
    attribute_method_id = tvb_get_guint8(tvb, *offset + 8);

    cosem_class = dlms_get_class(class_id);
    if (cosem_class) {
        col_append_fstr(pinfo->cinfo, COL_INFO, " %s", cosem_class->name);
        if (is_attribute) {
            attribute_method_name = dlms_get_attribute_name(cosem_class, attribute_method_id);
        } else {
            attribute_method_name = dlms_get_method_name(cosem_class, attribute_method_id);
        }
    } else {
        col_append_fstr(pinfo->cinfo, COL_INFO, " %u", class_id);
        attribute_method_name = 0;
    }

    if (attribute_method_name) {
        col_append_fstr(pinfo->cinfo, COL_INFO, ".%s", attribute_method_name);
    } else {
        col_append_fstr(pinfo->cinfo, COL_INFO, ".%u", attribute_method_id);
    }

	instance_name = try_val64_to_str(tvb_get_ntoh48(tvb, *offset + 2), obis_code_names);
	if (instance_name) {
		col_append_fstr(pinfo->cinfo, COL_INFO, " %s", instance_name);
	}
	else {
		col_append_fstr(pinfo->cinfo, COL_INFO, " %u.%u.%u.%u.%u.%u",
			tvb_get_guint8(tvb, *offset + 2),
			tvb_get_guint8(tvb, *offset + 3),
			tvb_get_guint8(tvb, *offset + 4),
			tvb_get_guint8(tvb, *offset + 5),
			tvb_get_guint8(tvb, *offset + 6),
			tvb_get_guint8(tvb, *offset + 7));
	}

    subtree = proto_tree_add_subtree(tree, tvb, *offset, 9, dlms_ett.cosem_attribute_or_method_descriptor, 0,
                                     is_attribute ? "COSEM Attribute Descriptor" : "COSEM Method Descriptor");

    item = proto_tree_add_item(subtree, &dlms_hfi.class_id, tvb, *offset, 2, ENC_BIG_ENDIAN);
    if (cosem_class) {
        proto_item_append_text(item, ": %s (%u)", cosem_class->name, class_id);
    } else {
        proto_item_append_text(item, ": Unknown (%u)", class_id);
        expert_add_info(pinfo, item, &dlms_ei.not_implemented);
    }
    *offset += 2;

    item = proto_tree_add_item(subtree, &dlms_hfi.instance_id, tvb, *offset, 6, ENC_NA);
    proto_item_append_text(item, ": %s (%u.%u.%u.%u.%u.%u)",
                           instance_name ? instance_name : "Unknown",
                           tvb_get_guint8(tvb, *offset),
                           tvb_get_guint8(tvb, *offset + 1),
                           tvb_get_guint8(tvb, *offset + 2),
                           tvb_get_guint8(tvb, *offset + 3),
                           tvb_get_guint8(tvb, *offset + 4),
                           tvb_get_guint8(tvb, *offset + 5));
    *offset += 6;

    item = proto_tree_add_item(subtree,
                               is_attribute ? &dlms_hfi.attribute_id : &dlms_hfi.method_id,
                               tvb, *offset, 1, ENC_BIG_ENDIAN);
    if (attribute_method_name) {
        proto_item_append_text(item, ": %s (%u)", attribute_method_name, attribute_method_id);
    } else {
        proto_item_append_text(item, ": Unknown (%u)", attribute_method_id);
    }
    *offset += 1;
}

static void
dlms_dissect_cosem_attribute_descriptor(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    dlms_dissect_cosem_attribute_or_method_descriptor(tvb, pinfo, tree, offset, 1);
}

static void
dlms_dissect_cosem_method_descriptor(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    dlms_dissect_cosem_attribute_or_method_descriptor(tvb, pinfo, tree, offset, 0);
}

static void
dlms_dissect_data_access_result(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    proto_item *item;
    int result;

    item = proto_tree_add_item(tree, &dlms_hfi.data_access_result, tvb, *offset, 1, ENC_NA);
    result = tvb_get_guint8(tvb, *offset);
    *offset += 1;
    if (result != 0) {
        const gchar *str = val_to_str_const(result, dlms_data_access_result_names, "unknown result");
        col_append_fstr(pinfo->cinfo, COL_INFO, " (%s)", str);
        expert_add_info(pinfo, item, &dlms_ei.no_success);
    }
}

/* Get the value encoded in the specified length octets in definite form */
static unsigned
dlms_get_length(tvbuff_t *tvb, gint *offset)
{
    unsigned length;

    length = tvb_get_guint8(tvb, *offset);
    if ((length & 0x80) == 0) {
        *offset += 1;
    } else {
        unsigned i, n = length & 0x7f;
        length = 0;
        for (i = 0; i < n; i++) {
            length = (length << 8) + tvb_get_guint8(tvb, *offset + 1 + i);
        }
        *offset += 1 + n;
    }

    return length;
}

static unsigned
dlms_dissect_length(tvbuff_t *tvb, proto_tree *tree, gint *offset)
{
    gint start;
    unsigned length;
    proto_item *item;

    start = *offset;
    length = dlms_get_length(tvb, offset);
    item = proto_tree_add_item(tree, &dlms_hfi.length, tvb, start, *offset - start, ENC_NA);
    proto_item_append_text(item, ": %u", length);

    return length;
}

/* Calculate the number of bytes used by a TypeDescription of a compact array */
static int
dlms_get_type_description_length(tvbuff_t *tvb, gint offset)
{
    int choice = tvb_get_guint8(tvb, offset);
    if (choice == 1) { // array
        return 1 + 2 + dlms_get_type_description_length(tvb, offset + 3);
    } else if (choice == 2) { // structure
        gint end_offset = offset + 1;
        int sequence_of = dlms_get_length(tvb, &end_offset);
        while (sequence_of--) {
            end_offset += dlms_get_type_description_length(tvb, end_offset);
        }
        return end_offset - offset;
    } else {
        return 1;
    }
}

/* Attempt to parse a date-time from an octet-string */
static void
dlms_append_date_time_maybe(tvbuff_t *tvb, proto_item *item, gint offset, unsigned length)
{
    unsigned year, month, day_of_month, day_of_week;
    unsigned hour, minute, second, hundredths;
    /* TODO: unsigned deviation, clock; */

    if (length != 12) return;
    year = tvb_get_ntohs(tvb, offset);
    month = tvb_get_guint8(tvb, offset + 2);
    if (month < 1 || (month > 12 && month < 0xfd)) return;
    day_of_month = tvb_get_guint8(tvb, offset + 3);
    if (day_of_month < 1 || (day_of_month > 31 && day_of_month < 0xfd)) return;
    day_of_week = tvb_get_guint8(tvb, offset + 4);
    if (day_of_week < 1 || (day_of_week > 7 && day_of_week < 0xff)) return;
    hour = tvb_get_guint8(tvb, offset + 5);
    if (hour > 23 && hour < 0xff) return;
    minute = tvb_get_guint8(tvb, offset + 6);
    if (minute > 59 && minute < 0xff) return;
    second = tvb_get_guint8(tvb, offset + 7);
    if (second > 59 && second < 0xff) return;
    hundredths = tvb_get_guint8(tvb, offset + 8);
    if (hundredths > 99 && hundredths < 0xff) return;

    proto_item_append_text(item, year < 0xffff ? " (%u" : " (%X", year);
    proto_item_append_text(item, month < 13 ? "/%02u" : "/%02X", month);
    proto_item_append_text(item, day_of_month < 32 ? "/%02u" : "/%02X", day_of_month);
    proto_item_append_text(item, hour < 24 ? " %02u" : " %02X", hour);
    proto_item_append_text(item, minute < 60 ? ":%02u" : ":%02X", minute);
    proto_item_append_text(item, second < 60 ? ":%02u" : ":%02X", second);
    proto_item_append_text(item, hundredths < 100 ? ".%02u)" : ".%02X)", hundredths);
}

/* Set the value of an item with a planar data type (not array nor structure) */
static void
dlms_set_data_value(tvbuff_t *tvb, proto_item *item, gint choice, gint *offset)
{
    if (choice == 0) {
        proto_item_set_text(item, "Null");
    } else if (choice == 3) {
        gboolean value = tvb_get_guint8(tvb, *offset);
        proto_item_set_text(item, "Boolean: %s", value ? "true" : "false");
        *offset += 1;
    } else if (choice == 4) {
        guint bits = dlms_get_length(tvb, offset);
        guint bytes = (bits + 7) / 8;
        proto_item_set_text(item, "Bit-string (bits: %u, bytes: %u):", bits, bytes);
        *offset += bytes;
    } else if (choice == 5) {
        gint32 value = tvb_get_ntohl(tvb, *offset);
        proto_item_set_text(item, "Double Long: %d", value);
        *offset += 4;
    } else if (choice == 6) {
        guint32 value = tvb_get_ntohl(tvb, *offset);
        proto_item_set_text(item, "Double Long Unsigned: %u", value);
        *offset += 4;
    } else if (choice == 9) {
        guint length = dlms_get_length(tvb, offset);
        proto_item_set_text(item, "Octet String (length %u)", length);
        dlms_append_date_time_maybe(tvb, item, *offset, length);
        *offset += length;
    } else if (choice == 10) {
        guint length = dlms_get_length(tvb, offset);
        proto_item_set_text(item, "Visible String (length %u)", length);
        *offset += length;
    } else if (choice == 12) {
        guint length = dlms_get_length(tvb, offset);
        proto_item_set_text(item, "UTF8 String (length %u)", length);
        *offset += length;
    } else if (choice == 13) {
        guint value = tvb_get_guint8(tvb, *offset);
        proto_item_set_text(item, "BCD: 0x%02x", value);
        *offset += 1;
    } else if (choice == 15) {
	gint8 value = tvb_get_guint8(tvb, *offset);
        proto_item_set_text(item, "Integer: %d", value);
        *offset += 1;
    } else if (choice == 16) {
        gint16 value = tvb_get_ntohs(tvb, *offset);
        proto_item_set_text(item, "Long: %d", value);
        *offset += 2;
    } else if (choice == 17) {
        guint8 value = tvb_get_guint8(tvb, *offset);
        proto_item_set_text(item, "Unsigned: %u", value);
        *offset += 1;
    } else if (choice == 18) {
        guint16 value = tvb_get_ntohs(tvb, *offset);
        proto_item_set_text(item, "Long Unsigned: %u", value);
        *offset += 2;
    } else if (choice == 20) {
        gint64 value = tvb_get_ntoh64(tvb, *offset);
        proto_item_set_text(item, "Long64: %ld", value);
        *offset += 8;
    } else if (choice == 21) {
        guint64 value = tvb_get_ntoh64(tvb, *offset);
        proto_item_set_text(item, "Long64 Unsigned: %lu", value);
        *offset += 8;
    } else if (choice == 22) {
        guint8 value = tvb_get_guint8(tvb, *offset);
        proto_item_set_text(item, "Enum: %u", value);
        *offset += 1;
    } else if (choice == 23) {
        gfloat value = tvb_get_ntohieee_float(tvb, *offset);
        proto_item_set_text(item, "Float32: %f", value);
        *offset += 4;
    } else if (choice == 24) {
        gdouble value = tvb_get_ntohieee_double(tvb, *offset);
        proto_item_set_text(item, "Float64: %f", value);
        *offset += 8;
    } else if (choice == 25) {
        proto_item_set_text(item, "Date Time");
        *offset += 12;
    } else if (choice == 26) {
        proto_item_set_text(item, "Date");
        *offset += 5;
    } else if (choice == 27) {
        proto_item_set_text(item, "Time");
        *offset += 4;
    } else if (choice == 255) {
        proto_item_set_text(item, "Don't Care");
    } else {
        DISSECTOR_ASSERT_HINT(choice, "Invalid data type");
    }
}

static proto_item *
dlms_dissect_compact_array_content(tvbuff_t *tvb, proto_tree *tree, gint description_offset, gint *content_offset)
{
    proto_item *item, *subitem;
    proto_tree *subtree;
    unsigned choice;

    item = proto_tree_add_item(tree, &dlms_hfi.data, tvb, *content_offset, 0, ENC_NA);
    choice = tvb_get_guint8(tvb, description_offset);
    description_offset += 1;
    if (choice == 1) { /* array */
        guint16 i, elements = tvb_get_ntohs(tvb, description_offset);
        description_offset += 2;
        proto_item_set_text(item, "Array (%u elements)", elements);
        subtree = proto_item_add_subtree(item, dlms_ett.composite_data);
        for (i = 0; i < elements; i++) {
            subitem = dlms_dissect_compact_array_content(tvb, subtree, description_offset, content_offset);
            proto_item_prepend_text(subitem, "[%u] ", i + 1);
        }
    } else if (choice == 2) { /* structure */
        guint32 elements = dlms_get_length(tvb, &description_offset);
        proto_item_set_text(item, "Structure");
        subtree = proto_item_add_subtree(item, dlms_ett.composite_data);
        while (elements--) {
            dlms_dissect_compact_array_content(tvb, subtree, description_offset, content_offset);
            description_offset += dlms_get_type_description_length(tvb, description_offset);
        }
    } else { /* planar type */
        dlms_set_data_value(tvb, item, choice, content_offset);
    }
    proto_item_set_end(item, tvb, *content_offset);

    return item;
}

static proto_item *
dlms_dissect_data(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    proto_item *item, *subitem;
    proto_tree *subtree;
    unsigned choice, length, i;

    item = proto_tree_add_item(tree, &dlms_hfi.data, tvb, *offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, *offset);
    *offset += 1;
    if (choice == 1) { /* array */
        length = dlms_get_length(tvb, offset);
        proto_item_set_text(item, "Array (%u elements)", length);
        subtree = proto_item_add_subtree(item, dlms_ett.composite_data);
        for (i = 0; i < length; i++) {
            subitem = dlms_dissect_data(tvb, pinfo, subtree, offset);
            proto_item_prepend_text(subitem, "[%u] ", i + 1);
        }
    } else if (choice == 2) { /* structure */
        length = dlms_get_length(tvb, offset);
        proto_item_set_text(item, "Structure");
        subtree = proto_item_add_subtree(item, dlms_ett.composite_data);
        for (i = 0; i < length; i++) {
            dlms_dissect_data(tvb, pinfo, subtree, offset);
        }
    } else if (choice == 19) { /* compact-array */
        int description_offset = *offset;
        int description_length = dlms_get_type_description_length(tvb, *offset);
        int content_end;
        unsigned elements;
        subtree = proto_item_add_subtree(item, dlms_ett.composite_data);
        proto_tree_add_item(subtree, &dlms_hfi.type_description, tvb, description_offset, description_length, ENC_NA);
        *offset += description_length;
        length = dlms_dissect_length(tvb, subtree, offset);
        elements = 0;
        content_end = *offset + length;
        while (*offset < content_end) {
            subitem = dlms_dissect_compact_array_content(tvb, subtree, description_offset, offset);
            proto_item_prepend_text(subitem, "[%u] ", ++elements);
        }
        proto_item_set_text(item, "Compact Array (%u elements)", elements);
    } else { /* planar type */
        dlms_set_data_value(tvb, item, choice, offset);
    }
    proto_item_set_end(item, tvb, *offset);

    return item;
}

static void
dlms_dissect_list_of_data(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset, const char *name)
{
    proto_tree *item;
    proto_tree *subtree;
    int sequence_of, i;

    subtree = proto_tree_add_subtree(tree, tvb, *offset, 0, dlms_ett.data, &item, name);
    sequence_of = dlms_get_length(tvb, offset);
    for (i = 0; i < sequence_of; i++) {
        proto_item *subitem = dlms_dissect_data(tvb, pinfo, subtree, offset);
        proto_item_prepend_text(subitem, "[%u] ", i + 1);
    }
    proto_item_set_end(item, tvb, *offset);
}

static void
dlms_dissect_datablock_data(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, proto_tree *subtree, gint *offset, unsigned block_number, unsigned last_block)
{
    unsigned saved_offset, raw_data_length;
    proto_item *item;
    fragment_head *frags;
    tvbuff_t *rtvb;

    col_append_fstr(pinfo->cinfo, COL_INFO, " (block %u)", block_number);
    if (last_block) {
        col_append_str(pinfo->cinfo, COL_INFO, " (last block)");
    }

    saved_offset = *offset;
    raw_data_length = dlms_get_length(tvb, offset);
    item = proto_tree_add_item(subtree, &dlms_hfi.data, tvb, saved_offset, *offset - saved_offset + raw_data_length, ENC_NA);
    proto_item_append_text(item, " (length %u)", raw_data_length);

    if (block_number == 1) {
        fragment_delete(&dlms_reassembly_table, pinfo, DLMS_REASSEMBLY_ID_DATABLOCK, 0);
    }
    frags = fragment_add_seq_next(&dlms_reassembly_table, tvb, *offset, pinfo, DLMS_REASSEMBLY_ID_DATABLOCK, 0, raw_data_length, last_block == 0);
    rtvb = process_reassembled_data(tvb, *offset, pinfo, "Reassembled", frags, &dlms_fragment_items, 0, tree);
    if (rtvb) {
        gint offset = 0;
        subtree = proto_tree_add_subtree(tree, rtvb, 0, 0, dlms_ett.data, 0, "Reassembled Data");
        dlms_dissect_data(rtvb, pinfo, subtree, &offset);
    }

    *offset += raw_data_length;
}

static void
dlms_dissect_datablock_g(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    proto_tree *subtree;
    unsigned last_block, block_number;
    int result;

    subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.datablock, 0, "Datablock G");

    proto_tree_add_item(subtree, &dlms_hfi.last_block, tvb, *offset, 1, ENC_NA);
    last_block = tvb_get_guint8(tvb, *offset);
    *offset += 1;

    proto_tree_add_item(subtree, &dlms_hfi.block_number, tvb, *offset, 4, ENC_BIG_ENDIAN);
    block_number = tvb_get_ntohl(tvb, *offset);
    *offset += 4;

    result = tvb_get_guint8(tvb, *offset);
    *offset += 1;
    if (result == 0) {
        dlms_dissect_datablock_data(tvb, pinfo, tree, subtree, offset, block_number, last_block);
    } else if (result == 1) {
        dlms_dissect_data_access_result(tvb, pinfo, subtree, offset);
    }
}

static void
dlms_dissect_datablock_sa(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    proto_tree *subtree;    
    unsigned last_block, block_number;

    subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.datablock, 0, "Datablock SA");

    proto_tree_add_item(subtree, &dlms_hfi.last_block, tvb, *offset, 1, ENC_NA);
    last_block = tvb_get_guint8(tvb, *offset);
    *offset += 1;

    proto_tree_add_item(subtree, &dlms_hfi.block_number, tvb, *offset, 4, ENC_BIG_ENDIAN);
    block_number = tvb_get_ntohl(tvb, *offset);
    *offset += 4;

    dlms_dissect_datablock_data(tvb, pinfo, tree, subtree, offset, block_number, last_block);
}

static void
dlms_dissect_selective_access_descriptor(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    proto_item *item;
    proto_tree *subtree = proto_tree_add_subtree(tree, tvb, *offset, 0, dlms_ett.selective_access_descriptor, &item, "Selective Access Descriptor");
    int selector = tvb_get_guint8(tvb, *offset);
    proto_tree_add_item(subtree, &dlms_hfi.access_selector, tvb, *offset, 1, ENC_NA);
    *offset += 1;
    if (selector) {
        dlms_dissect_data(tvb, pinfo, subtree, offset);
    }
    proto_item_set_end(item, tvb, *offset);
}

static void
dlms_dissect_access_request_specification(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint *offset)
{
    proto_item *item, *subitem;
    proto_tree *subtree, *subsubtree;
    int sequence_of, i;

    subtree = proto_tree_add_subtree(tree, tvb, *offset, 0, dlms_ett.access_request_specification, &item, "Access Request Specification");
    sequence_of = dlms_get_length(tvb, offset);
    for (i = 0; i < sequence_of; i++) {
        int choice = tvb_get_guint8(tvb, *offset);
        subitem = proto_tree_add_item(subtree, &dlms_hfi.access_request, tvb, *offset, 1, ENC_NA);
        proto_item_prepend_text(subitem, "[%u] ", i + 1);
        subsubtree = proto_item_add_subtree(subitem, dlms_ett.access_request);
        *offset += 1;
        switch (choice) {
        case DLMS_ACCESS_REQUEST_GET:
        case DLMS_ACCESS_REQUEST_SET:
            dlms_dissect_cosem_attribute_descriptor(tvb, pinfo, subsubtree, offset);
            break;
        case DLMS_ACCESS_REQUEST_ACTION:
            dlms_dissect_cosem_method_descriptor(tvb, pinfo, subsubtree, offset);
            break;
        case DLMS_ACCESS_REQUEST_GET_WITH_SELECTION:
        case DLMS_ACCESS_REQUEST_SET_WITH_SELECTION:
            dlms_dissect_cosem_attribute_descriptor(tvb, pinfo, subsubtree, offset);
            dlms_dissect_selective_access_descriptor(tvb, pinfo, subsubtree, offset);
            break;
        default:
            DISSECTOR_ASSERT_HINT(choice, "Invalid Access-Request-Specification CHOICE");
        }
    }
    proto_item_set_end(item, tvb, *offset);
}

static void
dlms_dissect_conformance(tvbuff_t *tvb, proto_tree *tree, gint offset)
{
    proto_tree *subtree;
    header_field_info *hfi;

    subtree = proto_tree_add_subtree(tree, tvb, offset, 7, dlms_ett.conformance, 0, "Conformance");
    for (hfi = &dlms_hfi.conformance_general_protection; hfi <= &dlms_hfi.conformance_action; hfi++) {
        proto_tree_add_item(subtree, hfi, tvb, offset + 4, 3, ENC_BIG_ENDIAN);
    }
}

static void
dlms_dissect_data_notification(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    gint date_time_offset;
    gint date_time_length;
    proto_item *item;

    col_set_str(pinfo->cinfo, COL_INFO, "Data-Notification");

    dlms_dissect_long_invoke_id_and_priority(tree, tvb, &offset);

    date_time_offset = offset;
    date_time_length = dlms_get_length(tvb, &offset);
    item = proto_tree_add_item(tree, &dlms_hfi.date_time, tvb, date_time_offset, offset - date_time_offset + date_time_length, ENC_NA);
    dlms_append_date_time_maybe(tvb, item, offset, date_time_length);

    /* notification-body */
    dlms_dissect_data(tvb, pinfo, tree, &offset);
}

static void
dlms_dissect_application_context_name(tvbuff_t *tvb, proto_tree *tree, gint offset, gint length)
{
    if (length == 9) {
        proto_tree_add_item(tree, &dlms_hfi.application_context_name, tvb, offset + 4, 7, ENC_BIG_ENDIAN);
    }
}

static void
dlms_dissect_mechanism_name(tvbuff_t *tvb, proto_tree *tree, gint offset, gint length)
{
    if (length == 7) {
        proto_tree_add_item(tree, &dlms_hfi.mechanism_name, tvb, offset + 2, 7, ENC_BIG_ENDIAN);
    }
}

static void
dlms_dissect_initiate_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    gint val, len;

    /* dedicated-key */
    val = tvb_get_guint8(tvb, offset);
    if (val) {
        len = 2 + tvb_get_guint8(tvb, offset + 1);
    } else {
        len = 1;
    }
    proto_tree_add_item(tree, &dlms_hfi.dedicated_key, tvb, offset, len, ENC_NA);
    offset += len;

    /* response-allowed */
    val = tvb_get_guint8(tvb, offset);
    if (val == 0) { /* default (true) */
        len = 1;
    } else {
        return;
    }
    proto_tree_add_item(tree, &dlms_hfi.response_allowed, tvb, offset, len, ENC_NA);
    offset += len;

    /* proposed-quality-of-service */
    val = tvb_get_guint8(tvb, offset);
    if (val == 0) { /* not present */
        len = 1;
    } else {
        return;
    }
    proto_tree_add_item(tree, &dlms_hfi.quality_of_service, tvb, offset, len, ENC_NA);
    offset += len;

    /* proposed-dlms-version-number */
    proto_tree_add_item(tree, &dlms_hfi.dlms_version_number, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* proposed-conformance */
    dlms_dissect_conformance(tvb, tree, offset);
    offset += 7;

    /* client-max-receive-pdu-size */
    proto_tree_add_item(tree, &dlms_hfi.max_receive_pdu_size, tvb, offset, 2, ENC_BIG_ENDIAN);
}

static void
dlms_dissect_initiate_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    gint val, len;

    /* negotiated-quality-of-service */
    val = tvb_get_guint8(tvb, offset);
    if (val == 0) { /* not present */
        len = 1;
    } else {
        return;
    }
    proto_tree_add_item(tree, &dlms_hfi.quality_of_service, tvb, offset, len, ENC_NA);
    offset += len;

    /* negotiated-dlms-version-number */
    proto_tree_add_item(tree, &dlms_hfi.dlms_version_number, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* negotiated-conformance */
    dlms_dissect_conformance(tvb, tree, offset);
    offset += 7;

    /* server-max-receive-pdu-size */
    proto_tree_add_item(tree, &dlms_hfi.max_receive_pdu_size, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /* vaa-name */
    proto_tree_add_item(tree, &dlms_hfi.vaa_name, tvb, offset, 2, ENC_BIG_ENDIAN);
}

static void dlms_dissect_apdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset);

static void
dlms_dissect_aarq(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    proto_tree *subtree;
    tvbuff_t *subtvb;
    int end, length, tag;

    col_set_str(pinfo->cinfo, COL_INFO, "AARQ");
    length = tvb_get_guint8(tvb, offset);
    offset += 1;
    end = offset + length;
    while (offset < end) {
        tag = tvb_get_guint8(tvb, offset);
        length = tvb_get_guint8(tvb, offset + 1);
        switch (tag & 31) {
        case 0:
            proto_tree_add_item(tree, &dlms_hfi.protocol_version, tvb, offset, 2 + length, ENC_NA);
            break;
        case 1:
            dlms_dissect_application_context_name(tvb, tree, offset, length);
            break;
        case 2:
            proto_tree_add_item(tree, &dlms_hfi.called_ap_title, tvb, offset, 2 + length, ENC_NA);
            break;
        case 3:
            proto_tree_add_item(tree, &dlms_hfi.called_ae_qualifier, tvb, offset, 2 + length, ENC_NA);
            break;
        case 4:
            proto_tree_add_item(tree, &dlms_hfi.called_ap_invocation_id, tvb, offset, 2 + length, ENC_NA);
            break;
        case 5:
            proto_tree_add_item(tree, &dlms_hfi.called_ae_invocation_id, tvb, offset, 2 + length, ENC_NA);
            break;
        case 6:
            proto_tree_add_item(tree, &dlms_hfi.calling_ap_title, tvb, offset, 2 + length, ENC_NA);
            break;
        case 7:
            proto_tree_add_item(tree, &dlms_hfi.calling_ae_qualifier, tvb, offset, 2 + length, ENC_NA);
            break;
        case 8:
            proto_tree_add_item(tree, &dlms_hfi.calling_ap_invocation_id, tvb, offset, 2 + length, ENC_NA);
            break;
        case 9:
            proto_tree_add_item(tree, &dlms_hfi.calling_ae_invocation_id, tvb, offset, 2 + length, ENC_NA);
            break;
        case 10:
            proto_tree_add_item(tree, &dlms_hfi.sender_acse_requirements, tvb, offset, 2 + length, ENC_NA);
            break;
        case 11:
            dlms_dissect_mechanism_name(tvb, tree, offset, length);
            break;
        case 12:
            proto_tree_add_item(tree, &dlms_hfi.calling_authentication_value, tvb, offset, 2 + length, ENC_NA);
            break;
        case 29:
            proto_tree_add_item(tree, &dlms_hfi.implementation_information, tvb, offset, 2 + length, ENC_NA);
            break;
        case 30:
            subtree = proto_tree_add_subtree(tree, tvb, offset, 2 + length, dlms_ett.user_information, 0, "User Information");
            subtvb = tvb_new_subset_length(tvb, offset + 4, length - 2);
            dlms_dissect_apdu(subtvb, pinfo, subtree, 0);
            break;
        }
        offset += 2 + length;
    }
}

static void
dlms_dissect_aare(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    proto_tree *subtree;
    tvbuff_t *subtvb;
    int end, length, tag;

    col_set_str(pinfo->cinfo, COL_INFO, "AARE");
    length = tvb_get_guint8(tvb, offset);
    offset += 1;
    end = offset + length;
    while (offset < end) {
        tag = tvb_get_guint8(tvb, offset);
        length = tvb_get_guint8(tvb, offset + 1);
        switch (tag & 31) {
        case 0:
            proto_tree_add_item(tree, &dlms_hfi.protocol_version, tvb, offset, 2 + length, ENC_NA);
            break;
        case 1:
            dlms_dissect_application_context_name(tvb, tree, offset, length);
            break;
        case 2:
            proto_tree_add_item(tree, &dlms_hfi.result, tvb, offset, 2 + length, ENC_NA);
            break;
        case 3:
            proto_tree_add_item(tree, &dlms_hfi.result_source_diagnostic, tvb, offset, 2 + length, ENC_NA);
            break;
        case 4:
            proto_tree_add_item(tree, &dlms_hfi.responding_ap_title, tvb, offset, 2 + length, ENC_NA);
            break;
        case 5:
            proto_tree_add_item(tree, &dlms_hfi.responding_ae_qualifier, tvb, offset, 2 + length, ENC_NA);
            break;
        case 6:
            proto_tree_add_item(tree, &dlms_hfi.responding_ap_invocation_id, tvb, offset, 2 + length, ENC_NA);
            break;
        case 7:
            proto_tree_add_item(tree, &dlms_hfi.responding_ae_invocation_id, tvb, offset, 2 + length, ENC_NA);
            break;
        case 8:
            proto_tree_add_item(tree, &dlms_hfi.responder_acse_requirements, tvb, offset, 2 + length, ENC_NA);
            break;
        case 9:
            dlms_dissect_mechanism_name(tvb, tree, offset, length);
            break;
        case 10:
            proto_tree_add_item(tree, &dlms_hfi.responding_authentication_value, tvb, offset, 2 + length, ENC_NA);
            break;
        case 29:
            proto_tree_add_item(tree, &dlms_hfi.implementation_information, tvb, offset, 2 + length, ENC_NA);
            break;
        case 30:
            subtree = proto_tree_add_subtree(tree, tvb, offset, 2 + length, dlms_ett.user_information, 0, "User Information");
            subtvb = tvb_new_subset_length(tvb, offset + 4, length - 2);
            dlms_dissect_apdu(subtvb, pinfo, subtree, 0);
            break;
        }
        offset += 2 + length;
    }
}

static void
dlms_dissect_get_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    int choice;
    unsigned block_number;

    proto_tree_add_item(tree, &dlms_hfi.get_request, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    dlms_dissect_invoke_id_and_priority(tree, tvb, &offset);
    if (choice == DLMS_GET_REQUEST_NORMAL) {
        col_add_str(pinfo->cinfo, COL_INFO, "Get-Request-Normal");
        dlms_dissect_cosem_attribute_descriptor(tvb, pinfo, tree, &offset);
        dlms_dissect_selective_access_descriptor(tvb, pinfo, tree, &offset);
    } else if (choice == DLMS_GET_REQUEST_NEXT) {
        proto_tree_add_item(tree, &dlms_hfi.block_number, tvb, offset, 4, ENC_BIG_ENDIAN);
        block_number = tvb_get_ntohl(tvb, offset);
        offset += 4;
        col_add_fstr(pinfo->cinfo, COL_INFO, "Get-Request-Next (block %u)", block_number);
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Get-Request");
    }
}

static void
dlms_dissect_set_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    int choice;
    proto_tree *subtree;

    proto_tree_add_item(tree, &dlms_hfi.set_request, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    dlms_dissect_invoke_id_and_priority(tree, tvb, &offset);
    if (choice == DLMS_SET_REQUEST_NORMAL) {
        col_add_str(pinfo->cinfo, COL_INFO, "Set-Request-Normal");
        dlms_dissect_cosem_attribute_descriptor(tvb, pinfo, tree, &offset);
        dlms_dissect_selective_access_descriptor(tvb, pinfo, tree, &offset);
        subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.data, 0, "Data");
        dlms_dissect_data(tvb, pinfo, subtree, &offset);
    } else if (choice == DLMS_SET_REQUEST_WITH_FIRST_DATABLOCK) {
        col_add_str(pinfo->cinfo, COL_INFO, "Set-Request-With-First-Datablock");
        dlms_dissect_cosem_attribute_descriptor(tvb, pinfo, tree, &offset);
        dlms_dissect_selective_access_descriptor(tvb, pinfo, tree, &offset);
        dlms_dissect_datablock_sa(tvb, pinfo, tree, &offset);
    } else if (choice == DLMS_SET_REQUEST_WITH_DATABLOCK) {
        col_add_str(pinfo->cinfo, COL_INFO, "Set-Request-With-Datablock");
        dlms_dissect_datablock_sa(tvb, pinfo, tree, &offset);
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Set-Request");
    }
}

static void
dlms_dissect_event_notification_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    proto_tree *subtree;

    col_add_str(pinfo->cinfo, COL_INFO, "Event-Notification-Request");
    offset += 1; /* time OPTIONAL (assume it is not present) */
    dlms_dissect_cosem_attribute_descriptor(tvb, pinfo, tree, &offset);
    subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.data, 0, "Data");
    dlms_dissect_data(tvb, pinfo, subtree, &offset);
}

static void
dlms_dissect_action_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    int choice, method_invocation_parameters;
    proto_tree *subtree;

    proto_tree_add_item(tree, &dlms_hfi.action_request, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    dlms_dissect_invoke_id_and_priority(tree, tvb, &offset);
    if (choice == DLMS_ACTION_REQUEST_NORMAL) {
        col_add_str(pinfo->cinfo, COL_INFO, "Action-Request-Normal");
        dlms_dissect_cosem_method_descriptor(tvb, pinfo, tree, &offset);
        method_invocation_parameters = tvb_get_guint8(tvb, offset);
        if (method_invocation_parameters) {
            offset += 1;
            subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.data, 0, "Data");
            dlms_dissect_data(tvb, pinfo, subtree, &offset);
        }
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Action-Request");
    }
}

static void
dlms_dissect_get_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    int choice, result;
    proto_tree *subtree;

    proto_tree_add_item(tree, &dlms_hfi.get_response, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    dlms_dissect_invoke_id_and_priority(tree, tvb, &offset);
    if (choice == DLMS_GET_RESPONSE_NORMAL) {
        col_add_str(pinfo->cinfo, COL_INFO, "Get-Response-Normal");
        result = tvb_get_guint8(tvb, offset);
        offset += 1;
        if (result == 0) {
            subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.data, 0, "Data");
            dlms_dissect_data(tvb, pinfo, subtree, &offset);
        } else if (result == 1) {
            dlms_dissect_data_access_result(tvb, pinfo, tree, &offset);
        }
    } else if (choice == DLMS_GET_RESPONSE_WITH_DATABLOCK) {
        col_add_str(pinfo->cinfo, COL_INFO, "Get-Response-With-Datablock");
        dlms_dissect_datablock_g(tvb, pinfo, tree, &offset);
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Get-Response");
    }
}

static void
dlms_dissect_set_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    unsigned choice, block_number;

    proto_tree_add_item(tree, &dlms_hfi.set_response, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    dlms_dissect_invoke_id_and_priority(tree, tvb, &offset);
    if (choice == DLMS_SET_RESPONSE_NORMAL) {
        col_add_str(pinfo->cinfo, COL_INFO, "Set-Response-Normal");
        dlms_dissect_data_access_result(tvb, pinfo, tree, &offset);
    } else if (choice == DLMS_SET_RESPONSE_DATABLOCK) {
        col_add_str(pinfo->cinfo, COL_INFO, "Set-Response-Datablock");
        proto_tree_add_item(tree, &dlms_hfi.block_number, tvb, offset, 4, ENC_BIG_ENDIAN);
        block_number = tvb_get_ntohl(tvb, offset);
        col_append_fstr(pinfo->cinfo, COL_INFO, " (block %u)", block_number);
    } else if (choice == DLMS_SET_RESPONSE_LAST_DATABLOCK) {
        col_add_str(pinfo->cinfo, COL_INFO, "Set-Response-Last-Datablock");
        dlms_dissect_data_access_result(tvb, pinfo, tree, &offset);
        proto_tree_add_item(tree, &dlms_hfi.block_number, tvb, offset, 4, ENC_BIG_ENDIAN);
        block_number = tvb_get_ntohl(tvb, offset);
        col_append_fstr(pinfo->cinfo, COL_INFO, " (block %u)", block_number);
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Set-Response");
    }
}

static void
dlms_dissect_action_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    unsigned choice, result;
    const gchar *result_name;
    proto_item *item;

    proto_tree_add_item(tree, &dlms_hfi.action_response, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    dlms_dissect_invoke_id_and_priority(tree, tvb, &offset);
    if (choice == DLMS_ACTION_RESPONSE_NORMAL) {
        col_add_str(pinfo->cinfo, COL_INFO, "Action-Response-Normal");
        item = proto_tree_add_item(tree, &dlms_hfi.action_result, tvb, offset, 1, ENC_NA);
        result = tvb_get_guint8(tvb, offset);
        offset += 1;
        if (result) {
            result_name = val_to_str_const(result, dlms_action_result_names, "unknown");
            col_append_fstr(pinfo->cinfo, COL_INFO, " (%s)", result_name);
            expert_add_info(pinfo, item, &dlms_ei.no_success);
        }
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Action-Response");
    }
}

static void
dlms_dissect_exception_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    proto_item *item;

    col_set_str(pinfo->cinfo, COL_INFO, "Exception-Response");
    item = proto_tree_add_item(tree, &dlms_hfi.state_error, tvb, offset, 1, ENC_NA);
    expert_add_info(pinfo, item, &dlms_ei.no_success);
    item = proto_tree_add_item(tree, &dlms_hfi.service_error, tvb, offset + 1, 1, ENC_NA);
    expert_add_info(pinfo, item, &dlms_ei.no_success);
}

static void
dlms_dissect_access_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    gint date_time_offset;
    int date_time_length;
    proto_item *item;

    col_set_str(pinfo->cinfo, COL_INFO, "Access-Request");

    dlms_dissect_long_invoke_id_and_priority(tree, tvb, &offset);

    date_time_offset = offset;
    date_time_length = dlms_get_length(tvb, &offset);
    item = proto_tree_add_item(tree, &dlms_hfi.date_time, tvb, date_time_offset, offset - date_time_offset + date_time_length, ENC_NA);
    dlms_append_date_time_maybe(tvb, item, offset, date_time_length);

    dlms_dissect_access_request_specification(tvb, pinfo, tree, &offset);

    dlms_dissect_list_of_data(tvb, pinfo, tree, &offset, "Access Request List Of Data");
}

static void
dlms_dissect_access_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    gint date_time_offset;
    int date_time_length;
    proto_item *item;
    proto_tree *subtree, *subsubtree;
    int sequence_of, i;

    col_set_str(pinfo->cinfo, COL_INFO, "Access-Response");

    dlms_dissect_long_invoke_id_and_priority(tree, tvb, &offset);

    date_time_offset = offset;
    date_time_length = dlms_get_length(tvb, &offset);
    item = proto_tree_add_item(tree, &dlms_hfi.date_time, tvb, date_time_offset, offset - date_time_offset + date_time_length, ENC_NA);
    dlms_append_date_time_maybe(tvb, item, offset, date_time_length);

    dlms_dissect_access_request_specification(tvb, pinfo, tree, &offset);

    dlms_dissect_list_of_data(tvb, pinfo, tree, &offset, "Access Response List Of Data");

    subtree = proto_tree_add_subtree(tree, tvb, offset, 0, dlms_ett.access_response_specification, 0, "Access Response Specification");
    sequence_of = dlms_get_length(tvb, &offset);
    for (i = 0; i < sequence_of; i++) {
        item = proto_tree_add_item(subtree, &dlms_hfi.access_response, tvb, offset, 1, ENC_NA);
        proto_item_prepend_text(item, "[%u] ", i + 1);
        subsubtree = proto_item_add_subtree(item, dlms_ett.access_request);
        offset += 1;
        dlms_dissect_data_access_result(tvb, pinfo, subsubtree, &offset);
    }
}

static void
dlms_dissect_ciphered_apdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    proto_tree *subtree;
    tvbuff_t *subtvb;
    int length, security_control, invocation_counter;

    length = dlms_dissect_length(tvb, tree, &offset);

    subtree = proto_tree_add_subtree(tree, tvb, offset, 1, dlms_ett.security_control, 0, "Security Control");
    proto_tree_add_item(subtree, &dlms_hfi.security_suite_id, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(subtree, &dlms_hfi.authentication, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(subtree, &dlms_hfi.encryption, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(subtree, &dlms_hfi.key_set, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(subtree, &dlms_hfi.compression, tvb, offset, 1, ENC_NA);
    security_control = tvb_get_guint8(tvb, offset);
    offset += 1;
    length -= 1;

    proto_tree_add_item(tree, &dlms_hfi.invocation_counter, tvb, offset, 4, ENC_BIG_ENDIAN);
    invocation_counter = tvb_get_ntohl(tvb, offset);
    offset += 4;
    length -= 4;

    if (security_control & DLMS_SECURITY_CONTROL_AUTHENTICATION) {
        length -= 12;
    }
    subtree = proto_tree_add_subtree(tree, tvb, offset, length, dlms_ett.protected_apdu, 0, "Protected APDU");
    if (security_control & DLMS_SECURITY_CONTROL_ENCRYPTION) {
        subtvb = dlms_decrypt(tvb, pinfo, offset, length, security_control, invocation_counter);
    } else {
        subtvb = tvb_new_subset_length(tvb, offset, length);
    }
    if (subtvb) {
        dlms_dissect_apdu(subtvb, pinfo, subtree, 0);
    }
    offset += length;

    if (security_control & DLMS_SECURITY_CONTROL_AUTHENTICATION) {
        proto_tree_add_item(tree, &dlms_hfi.authentication_tag, tvb, offset, 12, ENC_NA);
    }
}

/* Dissect a DLMS Application Packet Data Unit (APDU) */
static void
dlms_dissect_apdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
    unsigned choice;

    proto_tree_add_item(tree, &dlms_hfi.apdu, tvb, offset, 1, ENC_NA);
    choice = tvb_get_guint8(tvb, offset);
    offset += 1;
    switch (choice) {
    case DLMS_INITIATE_REQUEST:
        dlms_dissect_initiate_request(tvb, pinfo, tree, offset);
        break;
    case DLMS_INITIATE_RESPONSE:
        dlms_dissect_initiate_response(tvb, pinfo, tree, offset);
        break;
    case DLMS_DATA_NOTIFICATION:
        dlms_dissect_data_notification(tvb, pinfo, tree, offset);
        break;
    case DLMS_AARQ:
        dlms_dissect_aarq(tvb, pinfo, tree, offset);
        break;
    case DLMS_AARE:
        dlms_dissect_aare(tvb, pinfo, tree, offset);
        break;
    case DLMS_RLRQ:
        col_set_str(pinfo->cinfo, COL_INFO, "RLRQ");
        break;
    case DLMS_RLRE:
        col_set_str(pinfo->cinfo, COL_INFO, "RLRE");
        break;
    case DLMS_GET_REQUEST:
        dlms_dissect_get_request(tvb, pinfo, tree, offset);
        break;
    case DLMS_SET_REQUEST:
        dlms_dissect_set_request(tvb, pinfo, tree, offset);
        break;
    case DLMS_EVENT_NOTIFICATION_REQUEST:
        dlms_dissect_event_notification_request(tvb, pinfo, tree, offset);
        break;
    case DLMS_ACTION_REQUEST:
        dlms_dissect_action_request(tvb, pinfo, tree, offset);
        break;
    case DLMS_GET_RESPONSE:
        dlms_dissect_get_response(tvb, pinfo, tree, offset);
        break;
    case DLMS_SET_RESPONSE:
        dlms_dissect_set_response(tvb, pinfo, tree, offset);
        break;
    case DLMS_ACTION_RESPONSE:
        dlms_dissect_action_response(tvb, pinfo, tree, offset);
        break;
    case DLMS_GLO_GET_REQUEST:
    case DLMS_GLO_SET_REQUEST:
    case DLMS_GLO_EVENT_NOTIFICATION_REQUEST:
    case DLMS_GLO_GET_RESPONSE:
    case DLMS_GLO_SET_RESPONSE:
    case DLMS_GLO_ACTION_RESPONSE:
    case DLMS_DED_GET_REQUEST:
    case DLMS_DED_SET_REQUEST:
    case DLMS_DED_EVENT_NOTIFICATION_REQUEST:
    case DLMS_DED_ACTION_REQUEST:
    case DLMS_DED_GET_RESPONSE:
    case DLMS_DED_SET_RESPONSE:
    case DLMS_DED_ACTION_RESPONSE:
        col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(choice, dlms_apdu_names, "Unknown APDU"));
        /* fall through */
    case DLMS_GLO_INITIATE_REQUEST:
    case DLMS_GLO_INITIATE_RESPONSE:
        dlms_dissect_ciphered_apdu(tvb, pinfo, tree, offset);
        break;
    case DLMS_EXCEPTION_RESPONSE:
        dlms_dissect_exception_response(tvb, pinfo, tree, offset);
        break;
    case DLMS_ACCESS_REQUEST:
        dlms_dissect_access_request(tvb, pinfo, tree, offset);
        break;
    case DLMS_ACCESS_RESPONSE:
        dlms_dissect_access_response(tvb, pinfo, tree, offset);
        break;
    default:
        col_set_str(pinfo->cinfo, COL_INFO, "Unknown APDU");
        break;
    }
}

/* Dissect a check sequence field (HCS or FCS) of an HDLC frame */
static void
dlms_dissect_hdlc_check_sequence(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset, int length, header_field_info *hfi)
{
    int i, j;
    unsigned cs;
    proto_item *item;

    cs = 0xffff;
    for (i = 0; i < length; i++) {
        cs = cs ^ tvb_get_guint8(tvb, offset + i);
        for (j = 0; j < 8; j++) {
            if (cs & 1) {
                cs = (cs >> 1) ^ 0x8408;
            } else {
                cs = cs >> 1;
            }
        }
    }
    cs = cs ^ 0xffff;

    item = proto_tree_add_item(tree, hfi, tvb, offset + length, 2, ENC_NA);
    if (tvb_get_letohs(tvb, offset + length) != cs) {
        expert_add_info(pinfo, item, &dlms_ei.check_sequence);
    }        
}

/* Dissect the information field of an HDLC (SNRM or UA) frame */
static void
dlms_dissect_hdlc_information(tvbuff_t *tvb, proto_tree *tree, gint *offset)
{
    proto_tree *subtree;

    subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.hdlc_information, 0, "Information");
    unsigned format = tvb_get_guint8(tvb, *offset);
    *offset += 1;
    if (format == 0x81) { /* format identifier */
        unsigned group = tvb_get_guint8(tvb, *offset);
        *offset += 1;
        if (group == 0x80) { /* group identifier */
            unsigned i, length = tvb_get_guint8(tvb, *offset);
            *offset += 1;
            for (i = 0; i < length; ) { /* parameters */
                proto_item *item;
                unsigned parameter = tvb_get_guint8(tvb, *offset);
                unsigned j, parameter_length = tvb_get_guint8(tvb, *offset + 1);
                unsigned value = 0;
                for (j = 0; j < parameter_length; j++) {
                    value = (value << 8) + tvb_get_guint8(tvb, *offset + 2 + j);
                }
                item = proto_tree_add_item(subtree, &dlms_hfi.hdlc_parameter, tvb, *offset, 2 + parameter_length, ENC_NA);
                proto_item_set_text(item, "%s: %u",
                    parameter == 5 ? "Maximum Information Field Length Transmit" :
                    parameter == 6 ? "Maximum Information Field Length Receive" :
                    parameter == 7 ? "Window Size Transmit" :
                    parameter == 8 ? "Window Size Receive" :
                    "Unknown Information Field Parameter",
                    value);
                i += 2 + parameter_length;
                *offset += 2 + parameter_length;
            }
        }
    }
}

/* Dissect a DLMS APDU in an HDLC frame */
static void
dlms_dissect_hdlc(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    proto_tree *subtree, *subsubtree;
    proto_item *item;
    fragment_head *frags;
    tvbuff_t *rtvb; /* reassembled tvb */
    unsigned length, segmentation, control;

    subtree = proto_tree_add_subtree(tree, tvb, 0, 0, dlms_ett.hdlc, 0, "HDLC");

    /* Opening flag */
    proto_tree_add_item(subtree, &dlms_hfi.hdlc_flag, tvb, 0, 1, ENC_NA);

    /* Frame format field */
    subsubtree = proto_tree_add_subtree(subtree, tvb, 1, 2, dlms_ett.hdlc_format, 0, "Frame Format");
    proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_type, tvb, 1, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_segmentation, tvb, 1, 2, ENC_BIG_ENDIAN);
    segmentation = (tvb_get_ntohs(tvb, 1) >> 11) & 1;
    proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_length, tvb, 1, 2, ENC_BIG_ENDIAN);
    length = tvb_get_ntohs(tvb, 1) & 0x7ff; /* length of HDLC frame excluding the opening and closing flag fields */

    /* Destination address field */
    subsubtree = proto_tree_add_subtree(subtree, tvb, 3, 1, dlms_ett.hdlc_address, 0, "Destination Address");
    proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_address, tvb, 3, 1, ENC_NA);

    /* Source address field */
    subsubtree = proto_tree_add_subtree(subtree, tvb, 4, 1, dlms_ett.hdlc_address, 0, "Source Address");
    proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_address, tvb, 4, 1, ENC_NA);

    /* Control field */
    subsubtree = proto_tree_add_subtree(subtree, tvb, 5, 1, dlms_ett.hdlc_control, 0, "Control");
    control = tvb_get_guint8(tvb, 5);

    /* Header check sequence field */
    if (length > 7) {
        dlms_dissect_hdlc_check_sequence(tvb, pinfo, subtree, 1, 5, &dlms_hfi.hdlc_hcs);
    }

    /* Control sub-fields and information field */
    if ((control & 0x01) == 0x00) {
        col_add_str(pinfo->cinfo, COL_INFO, "HDLC I"); /* Information */
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_i, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_rsn, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_ssn, tvb, 5, 1, ENC_NA);

        subsubtree = proto_tree_add_subtree_format(subtree, tvb, 8, length - 9, dlms_ett.hdlc_information, 0, "Information Field (length %u)", length - 9);
        frags = fragment_add_seq_next(&dlms_reassembly_table, tvb, 8, pinfo, DLMS_REASSEMBLY_ID_HDLC, 0, length - 9, segmentation);
        rtvb = process_reassembled_data(tvb, 8, pinfo, "Reassembled", frags, &dlms_fragment_items, 0, tree);
        if (rtvb) {
            proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_llc, rtvb, 0, 3, ENC_NA);
            dlms_dissect_apdu(rtvb, pinfo, tree, 3);
        }
    } else if ((control & 0x0f) == 0x01) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC RR"); /* Receive Ready */
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_rr_rnr, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_rsn, tvb, 5, 1, ENC_NA);
    } else if ((control & 0x0f) == 0x05) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC RNR"); /* Receive Not Ready */
        item = proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_rr_rnr, tvb, 5, 1, ENC_NA);
        expert_add_info(pinfo, item, &dlms_ei.no_success);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_rsn, tvb, 5, 1, ENC_NA);
    } else if ((control & 0xef) == 0x83) { /* Set Normal Response Mode */
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC SNRM");
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_other, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
        if (length > 7) {
            gint offset = 8;
            dlms_dissect_hdlc_information(tvb, subtree, &offset);
        }
    } else if ((control & 0xef) == 0x43) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC DISC"); /* Disconnect */
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_other, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
    } else if ((control & 0xef) == 0x63) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC UA"); /* Unnumbered Acknowledge */
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_other, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
        if (length > 7) {
            gint offset = 8;
            dlms_dissect_hdlc_information(tvb, subtree, &offset);
        }
    } else if ((control & 0xef) == 0x0f) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC DM"); /* Disconnected Mode */
        item = proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_other, tvb, 5, 1, ENC_NA);
        expert_add_info(pinfo, item, &dlms_ei.no_success);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
    } else if ((control & 0xef) == 0x87) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC FRMR"); /* Frame Reject */
        item = proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_other, tvb, 5, 1, ENC_NA);
        expert_add_info(pinfo, item, &dlms_ei.no_success);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
    } else if ((control & 0xef) == 0x03) {
        col_set_str(pinfo->cinfo, COL_INFO, "HDLC UI"); /* Unnumbered Information */
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_frame_other, tvb, 5, 1, ENC_NA);
        proto_tree_add_item(subsubtree, &dlms_hfi.hdlc_pf, tvb, 5, 1, ENC_NA);
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "Unknown HDLC frame");
    }

    /* Frame check sequence field */
    dlms_dissect_hdlc_check_sequence(tvb, pinfo, subtree, 1, length - 2, &dlms_hfi.hdlc_fcs);

    /* Closing flag */
    proto_tree_add_item(subtree, &dlms_hfi.hdlc_flag, tvb, length + 1, 1, ENC_NA);
}

/* Dissect a DLMS APDU in an IEC 61334-4-32 convergence layer data frame (PLC) */
static void
dlms_dissect_432(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    proto_tree_add_item(tree, &dlms_hfi.iec432llc, tvb, 0, 3, ENC_NA);
    dlms_dissect_apdu(tvb, pinfo, tree, 3);
}

/* Dissect a DLMS APDU in a Wrapper Protocol Data Unit (TCP/UDP/IP) */
static void
dlms_dissect_wrapper(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    proto_tree_add_item(tree, &dlms_hfi.wrapper_header, tvb, 0, 8, ENC_NA);
    dlms_dissect_apdu(tvb, pinfo, tree, 8);
}

static int
dlms_dissect(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    header_field_info *hfi;
    proto_item *item;
    proto_tree *subtree;
    unsigned first_byte;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "DLMS");

    hfi = proto_registrar_get_nth(dlms_proto);
    item = proto_tree_add_item(tree, hfi, tvb, 0, -1, ENC_NA);
    subtree = proto_item_add_subtree(item, dlms_ett.dlms);

    first_byte = tvb_get_guint8(tvb, 0);
    if (first_byte == 0x7e) {
        dlms_dissect_hdlc(tvb, pinfo, subtree);
    } else if (first_byte == 0x90) {
        dlms_dissect_432(tvb, pinfo, subtree);
    } else if (first_byte == 0) {
        dlms_dissect_wrapper(tvb, pinfo, subtree);
    } else {
        dlms_dissect_apdu(tvb, pinfo, subtree, 0);
    }

    return tvb_captured_length(tvb);
}

static guint
dlms_get_tcp_pdu_length(packet_info *pinfo, tvbuff_t *tvb, int offset, void *data)
{
    const int wrapper_header_length = 8;
    int apdu_length;

    if (tvb_reported_length_remaining(tvb, offset) < wrapper_header_length) {
        return 0;
    }
    apdu_length = tvb_get_ntohs(tvb, offset + 6);

    return wrapper_header_length + apdu_length;
}

static int
dlms_dissect_tcp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    tcp_dissect_pdus(tvb, pinfo, tree, TRUE, 0, dlms_get_tcp_pdu_length, dlms_dissect, data);
    return tvb_captured_length(tvb);
}

static void
dlms_register_protoinfo(void)
{
    dlms_proto = proto_register_protocol("Device Language Message Specification", "DLMS", "dlms");

    /* Register the dlms_hfi header field info structures */
    {
        header_field_info *hfi[sizeof(dlms_hfi) / sizeof(header_field_info)];
        unsigned i;
        for (i = 0; i < array_length(hfi); i++) {
            hfi[i] = (header_field_info *)&dlms_hfi + i;
        }
        proto_register_fields(dlms_proto, hfi, array_length(hfi));
    }

    /* Initialise and register the dlms_ett protocol subtree indices */
    {
        gint *ett[sizeof(dlms_ett) / sizeof(gint)];
        unsigned i;
        for (i = 0; i < array_length(ett); i++) {
            ett[i] = (gint *)&dlms_ett + i;
            *ett[i] = -1;
        }
        proto_register_subtree_array(ett, array_length(ett));
    }

    /* Register the dlms_ei expert info fields */
    {
        static ei_register_info ei[] = {
            { &dlms_ei.no_success, { "dlms.no_success", PI_RESPONSE_CODE, PI_NOTE, "No success response", EXPFILL } },
            { &dlms_ei.not_implemented, { "dlms.not_implemented", PI_UNDECODED, PI_WARN, "Not implemented in the DLMS dissector", EXPFILL } },
            { &dlms_ei.check_sequence, { "dlms.check_sequence", PI_CHECKSUM, PI_WARN, "Bad HDLC check sequence field value", EXPFILL } },
        };
        expert_module_t *em = expert_register_protocol(dlms_proto);
        expert_register_field_array(em, ei, array_length(ei));
    }

    /* Register the reassembly table */
    {
        static const reassembly_table_functions f = {
            dlms_reassembly_hash_func,
            dlms_reassembly_equal_func,
            dlms_reassembly_key_func,
            dlms_reassembly_key_func,
            dlms_reassembly_free_key_func,
            dlms_reassembly_free_key_func,
        };
        reassembly_table_init(&dlms_reassembly_table, &f);
    }

    /* Decryption */
    gcry_cipher_open(&dlms_gcry_cipher_hd, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_GCM, 0);
    uat_t *uat_keys = uat_new("Keys", sizeof(dlms_key_entry_t), "dlms_keys", TRUE,
                              &dlms_key_entries, &dlms_key_entry_count,
                              UAT_AFFECTS_DISSECTION,
                              NULL, NULL, NULL, NULL, NULL, NULL,
                              dlms_keys_uat_fields);

    /* Preferences */
    {
        module_t *prefs = prefs_register_protocol(dlms_proto, NULL);
        prefs_register_uat_preference(prefs, "keys", "Keys", "Keys", uat_keys);
    }

    /* Register the DLMS dissector and the UDP handler */
    {
        dissector_handle_t dh = register_dissector("dlms", dlms_dissect, dlms_proto);
        dissector_add_uint("udp.port", DLMS_PORT, dh);
    }

    /* Register the TCP handler */
    {
        dissector_handle_t dh = create_dissector_handle(dlms_dissect_tcp, dlms_proto);
        dissector_add_uint("tcp.port", DLMS_PORT, dh);
    }
}

/*
 * The symbols that a Wireshark plugin is required to export.
 */

#define DLMS_PLUGIN_VERSION "0.0.3"

#ifdef VERSION_RELEASE /* wireshark >= 2.6 */

WS_DLL_PUBLIC_DEF const gchar plugin_release[] = VERSION_RELEASE;
WS_DLL_PUBLIC_DEF const gchar plugin_version[] = DLMS_PLUGIN_VERSION;
WS_DLL_PUBLIC_DEF void
plugin_register(void)
{
    static proto_plugin p;
    p.register_protoinfo = dlms_register_protoinfo;
    proto_register_plugin(&p);
}

#else /* wireshark < 2.6 */

WS_DLL_PUBLIC_DEF const gchar version[] = DLMS_PLUGIN_VERSION;
WS_DLL_PUBLIC_DEF void
plugin_register(void)
{
    dlms_register_protoinfo();
}

#endif
