/*
 * Copyright (C) 2019-2022 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "nnrf-handler.h"

bool nrf_nnrf_handle_nf_register(ogs_sbi_nf_instance_t *nf_instance,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    int status;
    ogs_sbi_response_t *response = NULL;

    OpenAPI_nf_profile_t *NFProfile = NULL;

    ogs_assert(nf_instance);
    ogs_assert(stream);
    ogs_assert(recvmsg);

    NFProfile = recvmsg->NFProfile;
    if (!NFProfile) {
        ogs_error("No NFProfile");
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No NFProfile", NULL));
        return false;
    }

    ogs_nnrf_nfm_handle_nf_profile(nf_instance, NFProfile);

    if (OGS_FSM_CHECK(&nf_instance->sm, nrf_nf_state_will_register)) {
        recvmsg->http.location = recvmsg->h.uri;
        status = OGS_SBI_HTTP_STATUS_CREATED;
    } else if (OGS_FSM_CHECK(&nf_instance->sm, nrf_nf_state_registered)) {
        status = OGS_SBI_HTTP_STATUS_OK;
    } else
        ogs_assert_if_reached();

    /* NRF uses pre-configured heartbeat if NFs did not send it */
    if (NFProfile->is_heart_beat_timer == false)
        nf_instance->time.heartbeat_interval =
            ogs_app()->time.nf_instance.heartbeat_interval;

    /*
     * TS29.510
     * Annex B (normative):NF Profile changes in NFRegister and NFUpdate
     * (NF Profile Complete Replacement) responses
     */
    if (NFProfile->is_nf_profile_changes_support_ind == true &&
        NFProfile->nf_profile_changes_support_ind == true) {

        OpenAPI_nf_profile_t NFProfileChanges;
        ogs_sbi_message_t sendmsg;

        memset(&NFProfileChanges, 0, sizeof(NFProfileChanges));
        NFProfileChanges.nf_instance_id = NFProfile->nf_instance_id;
        NFProfileChanges.nf_type = NFProfile->nf_type;
        NFProfileChanges.nf_status = NFProfile->nf_status;
        if (NFProfile->is_heart_beat_timer == false) {
            if (nf_instance->time.heartbeat_interval) {
                NFProfileChanges.is_heart_beat_timer = true;
                NFProfileChanges.heart_beat_timer =
                    nf_instance->time.heartbeat_interval;
            }
        }
        NFProfileChanges.is_nf_profile_changes_ind = true;
        NFProfileChanges.nf_profile_changes_ind = true;

        memset(&sendmsg, 0, sizeof(sendmsg));
        sendmsg.http.location = recvmsg->http.location;
        sendmsg.NFProfile = &NFProfileChanges;

        response = ogs_sbi_build_response(&sendmsg, status);

    } else {

        if (NFProfile->is_heart_beat_timer == false) {
            if (nf_instance->time.heartbeat_interval) {
                NFProfile->is_heart_beat_timer = true;
                NFProfile->heart_beat_timer =
                    nf_instance->time.heartbeat_interval;
            }
        }

        response = ogs_sbi_build_response(recvmsg, status);

    }

    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    return true;
}

bool nrf_nnrf_handle_nf_update(ogs_sbi_nf_instance_t *nf_instance,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_sbi_response_t *response = NULL;
    OpenAPI_list_t *PatchItemList = NULL;
    OpenAPI_lnode_t *node;

    ogs_assert(nf_instance);
    ogs_assert(stream);
    ogs_assert(recvmsg);

    SWITCH(recvmsg->h.method)
    CASE(OGS_SBI_HTTP_METHOD_PUT)
        return nrf_nnrf_handle_nf_register(
                nf_instance, stream, recvmsg);

    CASE(OGS_SBI_HTTP_METHOD_PATCH)
        PatchItemList = recvmsg->PatchItemList;
        if (!PatchItemList) {
            ogs_assert(true ==
                ogs_sbi_server_send_error(
                    stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                    recvmsg, "No PatchItemList Array", NULL));
            return false;
        }

        OpenAPI_list_for_each(PatchItemList, node) {
            OpenAPI_patch_item_t *patch_item = node->data;
            if (!patch_item) {
                ogs_assert(true ==
                    ogs_sbi_server_send_error(stream,
                        OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                        recvmsg, "No PatchItemList", NULL));
                return false;
            }
        }

        response = ogs_sbi_build_response(
                recvmsg, OGS_SBI_HTTP_STATUS_NO_CONTENT);
        ogs_assert(response);
        ogs_assert(true == ogs_sbi_server_send_response(stream, response));
        break;

    DEFAULT
        ogs_error("[%s] Invalid HTTP method [%s]",
                nf_instance->id, recvmsg->h.method);
        ogs_assert_if_reached();
    END

    return true;
}

bool nrf_nnrf_handle_nf_status_subscribe(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    bool rc;
    int status;
    ogs_sbi_response_t *response = NULL;
    OpenAPI_subscription_data_t *SubscriptionData = NULL;
    OpenAPI_subscription_data_subscr_cond_t *SubscrCond = NULL;
    ogs_sbi_subscription_data_t *subscription_data = NULL;
    ogs_sbi_client_t *client = NULL;
    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    ogs_sockaddr_t *addr = NULL;

    ogs_uuid_t uuid;
    char id[OGS_UUID_FORMATTED_LENGTH + 1];

    ogs_assert(stream);
    ogs_assert(recvmsg);

    SubscriptionData = recvmsg->SubscriptionData;
    if (!SubscriptionData) {
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No SubscriptionData", NULL));
        return false;
    }

    if (!SubscriptionData->nf_status_notification_uri) {
        ogs_assert(true ==
            ogs_sbi_server_send_error(
                stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No SubscriptionData", "NFStatusNotificationURL"));
        return false;
    }

    ogs_uuid_get(&uuid);
    ogs_uuid_format(id, &uuid);

    subscription_data = ogs_sbi_subscription_data_add();
    ogs_assert(subscription_data);
    ogs_sbi_subscription_data_set_id(subscription_data, id);
    ogs_assert(subscription_data->id);

    subscription_data->req_nf_type = SubscriptionData->req_nf_type;
    if (SubscriptionData->req_nf_instance_id) {
        subscription_data->req_nf_instance_id =
            ogs_strdup(SubscriptionData->req_nf_instance_id);
        if (!subscription_data->req_nf_instance_id) {
            ogs_error("ogs_strdup() failed");
            ogs_sbi_subscription_data_remove(subscription_data);
            return NULL;
        }
    }

    if (SubscriptionData->subscription_id) {
        ogs_warn("[%s] NF should not send SubscriptionID",
                SubscriptionData->subscription_id);
        ogs_free(SubscriptionData->subscription_id);
    }
    SubscriptionData->subscription_id = ogs_strdup(subscription_data->id);
    if (!SubscriptionData->subscription_id) {
        ogs_error("ogs_strdup() failed");
        ogs_sbi_subscription_data_remove(subscription_data);
        return NULL;
    }

    if (SubscriptionData->requester_features) {
        subscription_data->requester_features =
            ogs_uint64_from_string(SubscriptionData->requester_features);

        /* No need to send SubscriptionData->requester_features to the NF */
        ogs_free(SubscriptionData->requester_features);
        SubscriptionData->requester_features = NULL;
    } else {
        subscription_data->requester_features = 0;
    }

    OGS_SBI_FEATURES_SET(subscription_data->nrf_supported_features,
            OGS_SBI_NNRF_NFM_SERVICE_MAP);
    SubscriptionData->nrf_supported_features =
        ogs_uint64_to_string(subscription_data->nrf_supported_features);
    if (!SubscriptionData->nrf_supported_features) {
        ogs_error("ogs_strdup() failed");
        ogs_sbi_subscription_data_remove(subscription_data);
        return NULL;
    }

    SubscrCond = SubscriptionData->subscr_cond;
    if (SubscrCond) {
        subscription_data->subscr_cond.nf_type = SubscrCond->nf_type;
        if (SubscrCond->service_name)
            subscription_data->subscr_cond.service_name =
                ogs_strdup(SubscrCond->service_name);
    }

    subscription_data->notification_uri =
            ogs_strdup(SubscriptionData->nf_status_notification_uri);
    ogs_assert(subscription_data->notification_uri);

    rc = ogs_sbi_getaddr_from_uri(&scheme, &addr,
            subscription_data->notification_uri);
    if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
        ogs_assert(true ==
            ogs_sbi_server_send_error(
                stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "Invalid URI", subscription_data->notification_uri));
        ogs_sbi_subscription_data_remove(subscription_data);
        return false;
    }

    client = ogs_sbi_client_find(scheme, addr);
    if (!client) {
        client = ogs_sbi_client_add(scheme, addr);
        ogs_assert(client);
    }
    OGS_SBI_SETUP_CLIENT(subscription_data, client);
    ogs_freeaddrinfo(addr);

    if (subscription_data->time.validity_duration) {
        SubscriptionData->validity_time = ogs_sbi_localtime_string(
            ogs_time_now() + ogs_time_from_sec(
                subscription_data->time.validity_duration));
        ogs_assert(SubscriptionData->validity_time);

        subscription_data->t_validity = ogs_timer_add(ogs_app()->timer_mgr,
            nrf_timer_subscription_validity, subscription_data);
        ogs_assert(subscription_data->t_validity);
        ogs_timer_start(subscription_data->t_validity,
                ogs_time_from_sec(subscription_data->time.validity_duration));
    }

    recvmsg->http.location = recvmsg->h.uri;
    status = OGS_SBI_HTTP_STATUS_CREATED;

    response = ogs_sbi_build_response(recvmsg, status);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    return true;
}

bool nrf_nnrf_handle_nf_status_unsubscribe(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_sbi_subscription_data_t *subscription_data = NULL;
    ogs_assert(stream);
    ogs_assert(recvmsg);

    subscription_data = ogs_sbi_subscription_data_find(
            recvmsg->h.resource.component[1]);
    if (subscription_data) {
        ogs_sbi_response_t *response = NULL;
        ogs_sbi_subscription_data_remove(subscription_data);

        response = ogs_sbi_build_response(
                recvmsg, OGS_SBI_HTTP_STATUS_NO_CONTENT);
        ogs_assert(response);
        ogs_assert(true == ogs_sbi_server_send_response(stream, response));
    } else {
        ogs_error("Not found [%s]", recvmsg->h.resource.component[1]);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_NOT_FOUND,
                recvmsg, "Not found", recvmsg->h.resource.component[1]));
    }

    return true;
}

bool nrf_nnrf_handle_nf_list_retrieval(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_server_t *server = NULL;
    ogs_sbi_response_t *response = NULL;
    ogs_sbi_nf_instance_t *nf_instance = NULL;
    int i = 0;

    ogs_sbi_links_t *links = NULL;
    OpenAPI_lnode_t *node = NULL;

    ogs_assert(stream);
    server = ogs_sbi_server_from_stream(stream);
    ogs_assert(recvmsg);

    links = ogs_calloc(1, sizeof(*links));
    ogs_assert(links);

    links->items = OpenAPI_list_create();
    ogs_assert(links->items);

    links->self = ogs_sbi_server_uri(server, &recvmsg->h);

    i = 0;
    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance) {
        if (NF_INSTANCE_EXCLUDED_FROM_DISCOVERY(nf_instance))
            continue;

        if (recvmsg->param.nf_type &&
            recvmsg->param.nf_type != nf_instance->nf_type)
            continue;

        if (!recvmsg->param.limit ||
             (recvmsg->param.limit && i < recvmsg->param.limit)) {
            char *str = ogs_msprintf("%s/%s", links->self, nf_instance->id);
            ogs_assert(str);
            OpenAPI_list_add(links->items, str);
        }

        i++;
    }

    ogs_assert(links->self);

    memset(&sendmsg, 0, sizeof(sendmsg));
    sendmsg.links = links;
    sendmsg.http.content_type = (char *)OGS_SBI_CONTENT_3GPPHAL_TYPE;

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_OK);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    OpenAPI_list_for_each(links->items, node) {
        if (!node->data) continue;
        ogs_free(node->data);
    }
    OpenAPI_list_free(links->items);
    ogs_free(links->self);
    ogs_free(links);

    return true;
}

bool nrf_nnrf_handle_nf_profile_retrieval(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    ogs_assert(recvmsg->h.resource.component[1]);
    nf_instance = ogs_sbi_nf_instance_find(recvmsg->h.resource.component[1]);
    if (!nf_instance) {
        ogs_error("Not found [%s]", recvmsg->h.resource.component[1]);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_NOT_FOUND,
                recvmsg, "Not found", recvmsg->h.resource.component[1]));
        return false;
    }

    memset(&sendmsg, 0, sizeof(sendmsg));

    sendmsg.NFProfile = ogs_nnrf_nfm_build_nf_profile(
            nf_instance, NULL, NULL, true);
    if (!sendmsg.NFProfile) {
        ogs_error("ogs_nnrf_nfm_build_nf_profile() failed");
        return false;
    }

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_OK);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    ogs_nnrf_nfm_free_nf_profile(sendmsg.NFProfile);

    return true;
}

bool nrf_nnrf_handle_nf_discover(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;
    ogs_sbi_nf_instance_t *nf_instance = NULL;
    ogs_sbi_discovery_option_t *discovery_option = NULL;

    OpenAPI_search_result_t *SearchResult = NULL;
    OpenAPI_nf_profile_t *NFProfile = NULL;
    OpenAPI_lnode_t *node = NULL;
    int i;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (!recvmsg->param.target_nf_type) {
        ogs_error("No target-nf-type [%s]", recvmsg->h.uri);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No target-nf-type", NULL));
        return false;
    }
    if (!recvmsg->param.requester_nf_type) {
        ogs_error("No requester-nf-type [%s]", recvmsg->h.uri);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No requester-nf-type", NULL));
        return false;
    }

    ogs_debug("NF-Discover : Requester[%s] Target[%s]",
            OpenAPI_nf_type_ToString(recvmsg->param.requester_nf_type),
            OpenAPI_nf_type_ToString(recvmsg->param.target_nf_type));

    SearchResult = ogs_calloc(1, sizeof(*SearchResult));
    ogs_assert(SearchResult);

    SearchResult->is_validity_period = true;
    SearchResult->validity_period =
        ogs_app()->time.nf_instance.validity_duration;
    ogs_assert(SearchResult->validity_period);

    SearchResult->nf_instances = OpenAPI_list_create();
    ogs_assert(SearchResult->nf_instances);

    if (recvmsg->param.discovery_option)
        discovery_option = recvmsg->param.discovery_option;

    if (discovery_option) {
        if (discovery_option->target_nf_instance_id) {
            ogs_debug("target-nf-instance-id[%s]",
                discovery_option->target_nf_instance_id);
        }
        if (discovery_option->requester_nf_instance_id) {
            ogs_debug("requester-nf-instance-id[%s]",
                discovery_option->requester_nf_instance_id);
        }
        if (discovery_option->num_of_service_names) {
            for (i = 0; i < discovery_option->num_of_service_names; i++)
                ogs_debug("[%d] service-names[%s]", i,
                    discovery_option->service_names[i]);
        }
        if (discovery_option->requester_features) {
            ogs_debug("requester-features[0x%llx]",
                (long long)discovery_option->requester_features);
        }
    }

    i = 0;
    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance) {
        if (NF_INSTANCE_EXCLUDED_FROM_DISCOVERY(nf_instance))
            continue;

        if (nf_instance->nf_type != recvmsg->param.target_nf_type)
            continue;

        if (ogs_sbi_nf_instance_is_allowed_nf_type(
                nf_instance, recvmsg->param.requester_nf_type) == false)
            continue;

        if (discovery_option &&
            ogs_sbi_discovery_option_is_matched(
                nf_instance,
                recvmsg->param.requester_nf_type,
                discovery_option) == false)
            continue;

        if (recvmsg->param.limit && i >= recvmsg->param.limit)
            break;

        ogs_debug("[%s:%d] NF-Discovered [NF-Type:%s,NF-Status:%s,"
                "IPv4:%d,IPv6:%d]", nf_instance->id, i,
                OpenAPI_nf_type_ToString(nf_instance->nf_type),
                OpenAPI_nf_status_ToString(nf_instance->nf_status),
                nf_instance->num_of_ipv4, nf_instance->num_of_ipv6);

        NFProfile = ogs_nnrf_nfm_build_nf_profile(
                nf_instance, NULL, discovery_option,
                discovery_option &&
                OGS_SBI_FEATURES_IS_SET(
                    discovery_option->requester_features,
                    OGS_SBI_NNRF_DISC_SERVICE_MAP) ? true : false);
        OpenAPI_list_add(SearchResult->nf_instances, NFProfile);

        i++;
    }

    if (recvmsg->param.limit) SearchResult->num_nf_inst_complete = i;

    memset(&sendmsg, 0, sizeof(sendmsg));
    sendmsg.SearchResult = SearchResult;
    sendmsg.http.cache_control =
        ogs_msprintf("max-age=%d", SearchResult->validity_period);
    ogs_assert(sendmsg.http.cache_control);

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_OK);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    OpenAPI_list_for_each(SearchResult->nf_instances, node) {
        NFProfile = node->data;
        if (NFProfile) ogs_nnrf_nfm_free_nf_profile(NFProfile);
    }
    OpenAPI_list_free(SearchResult->nf_instances);

    if (sendmsg.http.cache_control)
        ogs_free(sendmsg.http.cache_control);
    ogs_free(SearchResult);

    return true;
}
