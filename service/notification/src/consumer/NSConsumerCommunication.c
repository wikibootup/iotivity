//******************************************************************
//
// Copyright 2016 Samsung Electronics All Rights Reserved.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

#include "NSConstants.h"
#include "NSConsumerCommon.h"
#include "NSConsumerCommunication.h"
#include "oic_malloc.h"
#include "oic_string.h"
#include "ocpayload.h"

#define NS_SYNC_URI "/notification/sync"

unsigned long NS_MESSAGE_ACCEPTANCE = 1;

NSMessage * NSCreateMessage_internal(uint64_t msgId, const char * providerId);
NSSyncInfo * NSCreateSyncInfo_consumer(uint64_t msgId, const char * providerId, NSSyncType state);

NSMessage * NSGetMessage(OCClientResponse * clientResponse);
NSSyncInfo * NSGetSyncInfoc(OCClientResponse * clientResponse);

char * NSGetCloudUri(const char * providerId, char * uri);

NSResult NSConsumerSubscribeProvider(NSProvider * provider)
{
    NSProvider_internal * provider_internal = (NSProvider_internal *) provider;
    NS_VERIFY_NOT_NULL(provider_internal, NS_ERROR);

    NSProviderConnectionInfo * connections = provider_internal->connection;
    while(connections)
    {
        if (connections->isSubscribing == true)
        {
            connections = connections->next;
            continue;
        }

        char * msgUri = OICStrdup(provider_internal->messageUri);
        NS_VERIFY_NOT_NULL(msgUri, NS_ERROR);
        char * syncUri = OICStrdup(provider_internal->syncUri);
        NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(syncUri, NS_ERROR, NSOICFree(msgUri));

        OCConnectivityType type = CT_DEFAULT;
        if (connections->addr->adapter == OC_ADAPTER_TCP)
        {
            type = CT_ADAPTER_TCP;
            if (connections->isCloudConnection == true)
            {
                msgUri = NSGetCloudUri(provider_internal->providerId, msgUri);
                NS_VERIFY_NOT_NULL(msgUri, NS_ERROR);
                syncUri = NSGetCloudUri(provider_internal->providerId, syncUri);
                NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(syncUri, NS_ERROR, NSOICFree(msgUri));
            }
        }

        NS_LOG_V(DEBUG, "subscribe to %s:%d", connections->addr->addr, connections->addr->port);

        NS_LOG(DEBUG, "get subscribe message query");
        char * query = NULL;
        query = NSMakeRequestUriWithConsumerId(msgUri);
        NS_VERIFY_NOT_NULL(query, NS_ERROR);

        NS_LOG(DEBUG, "subscribe message");
        NS_LOG_V(DEBUG, "subscribe query : %s", query);
        OCStackResult ret = NSInvokeRequest(&(connections->messageHandle),
                              OC_REST_OBSERVE, connections->addr, query, NULL,
                              NSConsumerMessageListener, NULL, type);
        NS_VERIFY_STACK_SUCCESS_WITH_POST_CLEANING(
                NSOCResultToSuccess(ret), NS_ERROR, NSOICFree(query));
        NSOICFree(query);
        NSOICFree(msgUri);

        NS_LOG(DEBUG, "get subscribe sync query");
        query = NSMakeRequestUriWithConsumerId(syncUri);
        NS_VERIFY_NOT_NULL(query, NS_ERROR);

        NS_LOG(DEBUG, "subscribe sync");
        NS_LOG_V(DEBUG, "subscribe query : %s", query);
        ret = NSInvokeRequest(&(connections->syncHandle),
                              OC_REST_OBSERVE, connections->addr, query, NULL,
                              NSConsumerSyncInfoListener, NULL, type);
        NS_VERIFY_STACK_SUCCESS_WITH_POST_CLEANING(
                NSOCResultToSuccess(ret), NS_ERROR, NSOICFree(query));
        NSOICFree(query);
        NSOICFree(syncUri);

        connections->isSubscribing = true;

        connections = connections->next;
    }

    return NS_OK;
}

OCStackApplicationResult NSConsumerCheckPostResult(
        void * ctx, OCDoHandle handle, OCClientResponse * clientResponse)
{
    (void) ctx;
    (void) handle;

    NS_VERIFY_NOT_NULL(clientResponse, OC_STACK_KEEP_TRANSACTION);
    NS_VERIFY_STACK_SUCCESS(
            NSOCResultToSuccess(clientResponse->result), OC_STACK_KEEP_TRANSACTION);

    return OC_STACK_KEEP_TRANSACTION;
}

void NSRemoveSyncInfoObj(NSSyncInfo * sync)
{
    NSOICFree(sync);
}

OCStackApplicationResult NSConsumerSyncInfoListener(
        void * ctx, OCDoHandle handle, OCClientResponse * clientResponse)
{
    (void) ctx;
    (void) handle;

    NS_VERIFY_NOT_NULL(clientResponse, OC_STACK_KEEP_TRANSACTION);
    NS_VERIFY_STACK_SUCCESS(
            NSOCResultToSuccess(clientResponse->result), OC_STACK_KEEP_TRANSACTION);

    NS_LOG(DEBUG, "get NSSyncInfo");
    NSSyncInfo * newSync = NSGetSyncInfoc(clientResponse);
    NS_VERIFY_NOT_NULL(newSync, OC_STACK_KEEP_TRANSACTION);

    NSTaskType taskType = TASK_RECV_SYNCINFO;

    NS_LOG(DEBUG, "build NSTask");
    NSTask * task = NSMakeTask(taskType, (void *) newSync);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(task,
               OC_STACK_KEEP_TRANSACTION, NSRemoveSyncInfoObj(newSync));

    NSConsumerPushEvent(task);

    return OC_STACK_KEEP_TRANSACTION;
}

OCStackApplicationResult NSConsumerMessageListener(
        void * ctx, OCDoHandle handle, OCClientResponse * clientResponse)
{
    (void) ctx;
    (void) handle;

    NS_VERIFY_NOT_NULL(clientResponse, OC_STACK_KEEP_TRANSACTION);
    NS_VERIFY_STACK_SUCCESS(NSOCResultToSuccess(clientResponse->result), OC_STACK_KEEP_TRANSACTION);

    NS_LOG(DEBUG, "build NSMessage");
    NSMessage * newNoti = NSGetMessage(clientResponse);
    NS_VERIFY_NOT_NULL(newNoti, OC_STACK_KEEP_TRANSACTION);

    NSTaskType type = TASK_CONSUMER_RECV_MESSAGE;

    if (newNoti->messageId == NS_MESSAGE_ACCEPTANCE || newNoti->messageId == NS_DENY)
    {
        NS_LOG(DEBUG, "Receive subscribe result");
        type = TASK_CONSUMER_RECV_PROVIDER_CHANGED;
    }
    else if (newNoti->messageId == NS_TOPIC)
    {
        NS_LOG(DEBUG, "Receive Topic change");
        type = TASK_CONSUMER_REQ_TOPIC_URI;
    }
    else
    {
        NS_LOG(DEBUG, "Receive new message");
    }

    NS_LOG(DEBUG, "build NSTask");
    NSTask * task = NSMakeTask(type, (void *) newNoti);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(task, NS_ERROR, NSRemoveMessage(newNoti));

    NSConsumerPushEvent(task);

    return OC_STACK_KEEP_TRANSACTION;
}

void NSGetMessagePostClean(char * pId, OCDevAddr * addr)
{
    NSOICFree(pId);
    NSOICFree(addr);
}

NSMessage * NSGetMessage(OCClientResponse * clientResponse)
{
    NS_VERIFY_NOT_NULL(clientResponse->payload, NULL);
    OCRepPayload * payload = (OCRepPayload *)clientResponse->payload;

    NS_LOG(DEBUG, "get msg id");
    uint64_t id = NULL;
    bool getResult = OCRepPayloadGetPropInt(payload, NS_ATTRIBUTE_MESSAGE_ID, (int64_t *)&id);
    NS_VERIFY_NOT_NULL(getResult == true ? (void *) 1 : NULL, NULL);

    NS_LOG(DEBUG, "get provider id");
    char * pId = NULL;
    getResult = OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_PROVIDER_ID, &pId);
    NS_LOG_V (DEBUG, "provider id: %s", pId);
    NS_VERIFY_NOT_NULL(getResult == true ? (void *) 1 : NULL, NULL);

    NS_LOG(DEBUG, "create NSMessage");
    NSMessage * retMsg = NSCreateMessage_internal(id, pId);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(retMsg, NULL, NSOICFree(pId));
    NSOICFree(pId);

    NS_LOG(DEBUG, "get msg optional field");
    OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_TITLE, &retMsg->title);
    OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_TEXT, &retMsg->contentText);
    OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_SOURCE, &retMsg->sourceName);

    OCRepPayloadGetPropInt(payload, NS_ATTRIBUTE_TYPE, (int64_t *)&retMsg->type);
    OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_DATETIME, &retMsg->dateTime);
    OCRepPayloadGetPropInt(payload, NS_ATTRIBUTE_TTL, (int64_t *)&retMsg->ttl);

    NS_LOG_V(DEBUG, "Msg ID      : %lld", (long long int)retMsg->messageId);
    NS_LOG_V(DEBUG, "Msg Title   : %s", retMsg->title);
    NS_LOG_V(DEBUG, "Msg Content : %s", retMsg->contentText);
    NS_LOG_V(DEBUG, "Msg Source  : %s", retMsg->sourceName);
    NS_LOG_V(DEBUG, "Msg Type    : %d", retMsg->type);
    NS_LOG_V(DEBUG, "Msg Date    : %s", retMsg->dateTime);
    NS_LOG_V(DEBUG, "Msg ttl     : %lld", (long long int)retMsg->ttl);

    return retMsg;
}

NSSyncInfo * NSGetSyncInfoc(OCClientResponse * clientResponse)
{
    NS_VERIFY_NOT_NULL(clientResponse->payload, NULL);

    OCRepPayload * payload = (OCRepPayload *)clientResponse->payload;

    NS_LOG(DEBUG, "get msg id");
    uint64_t id = NULL;
    bool getResult = OCRepPayloadGetPropInt(payload, NS_ATTRIBUTE_MESSAGE_ID, (int64_t *)&id);
    NS_VERIFY_NOT_NULL(getResult == true ? (void *) 1 : NULL, NULL);

    NS_LOG(DEBUG, "get provider id");
    char * pId = NULL;
    getResult = OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_PROVIDER_ID, &pId);
    NS_VERIFY_NOT_NULL(getResult == true ? (void *) 1 : NULL, NULL);

    NS_LOG(DEBUG, "get state");
    int64_t state = 0;
    getResult = OCRepPayloadGetPropInt(payload, NS_ATTRIBUTE_STATE, & state);
    NS_VERIFY_NOT_NULL(getResult == true ? (void *) 1 : NULL, NULL);

    NS_LOG(DEBUG, "create NSSyncInfo");
    NSSyncInfo * retSync = NSCreateSyncInfo_consumer(id, pId, (NSSyncType)state);
    NS_VERIFY_NOT_NULL(retSync, NULL);

    NS_LOG_V(DEBUG, "Sync ID : %lld", (long long int)retSync->messageId);
    NS_LOG_V(DEBUG, "Sync State : %d", (int) retSync->state);
    NS_LOG_V(DEBUG, "Sync Provider ID : %s", retSync->providerId);

    return retSync;
}

NSMessage * NSCreateMessage_internal(uint64_t id, const char * providerId)
{
    NSMessage * retMsg = (NSMessage *)OICMalloc(sizeof(NSMessage));
    NS_VERIFY_NOT_NULL(retMsg, NULL);

    retMsg->messageId = id;
    OICStrcpy(retMsg->providerId, sizeof(char) * NS_DEVICE_ID_LENGTH, providerId);
    retMsg->title = NULL;
    retMsg->contentText = NULL;
    retMsg->sourceName = NULL;
    retMsg->type = NS_MESSAGE_INFO;
    retMsg->dateTime = NULL;
    retMsg->ttl = 0;
    retMsg->mediaContents = NULL;

    return retMsg;
}

NSSyncInfo * NSCreateSyncInfo_consumer(uint64_t msgId, const char * providerId, NSSyncType state)
{
    NS_VERIFY_NOT_NULL(providerId, NULL);

    NSSyncInfo * retSync = (NSSyncInfo *)OICMalloc(sizeof(NSSyncInfo));
    NS_VERIFY_NOT_NULL(retSync, NULL);

    retSync->messageId = msgId;
    retSync->state = state;
    OICStrcpy(retSync->providerId, sizeof(char) * NS_DEVICE_ID_LENGTH, providerId);

    return retSync;
}

OCStackResult NSSendSyncInfo(NSSyncInfo * syncInfo, OCDevAddr * addr)
{
    NS_VERIFY_NOT_NULL(syncInfo, OC_STACK_ERROR);
    NS_VERIFY_NOT_NULL(addr, OC_STACK_ERROR);

    OCRepPayload * payload = OCRepPayloadCreate();
    NS_VERIFY_NOT_NULL(payload, OC_STACK_ERROR);

    OCRepPayloadSetPropInt(payload, NS_ATTRIBUTE_MESSAGE_ID, (int64_t)syncInfo->messageId);
    OCRepPayloadSetPropInt(payload, NS_ATTRIBUTE_STATE, syncInfo->state);
    OCRepPayloadSetPropString(payload, NS_ATTRIBUTE_PROVIDER_ID, syncInfo->providerId);

    char * uri = (char*)OICStrdup(NS_SYNC_URI);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(uri, OC_STACK_ERROR, OCRepPayloadDestroy(payload));

    OCConnectivityType type = CT_DEFAULT;
    if(addr->adapter == OC_ADAPTER_TCP)
    {
        type = CT_ADAPTER_TCP;
        uri = NSGetCloudUri(syncInfo->providerId, uri);
        NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(uri, OC_STACK_ERROR, OCRepPayloadDestroy(payload));
    }

    OCStackResult ret = NSInvokeRequest(NULL, OC_REST_POST, addr,
                            uri, (OCPayload*)payload,
                            NSConsumerCheckPostResult, NULL, type);
    NSOICFree(uri);

    return ret;
}

char * NSGetCloudUri(const char * providerId, char * uri)
{
    size_t uriLen = NS_DEVICE_ID_LENGTH + 1 + strlen(uri) + 1;
    char * retUri = (char *)OICMalloc(uriLen);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(retUri, NULL, NSOICFree(uri));

    snprintf(retUri, uriLen, "/%s%s", providerId, uri);
    NSOICFree(uri);
    NS_LOG_V(DEBUG, "Cloud uri : %s", retUri);

    return retUri;
}

void NSConsumerCommunicationTaskProcessing(NSTask * task)
{
    NS_VERIFY_NOT_NULL_V(task);

    NS_LOG_V(DEBUG, "Receive Event : %d", (int)task->taskType);
    if (task->taskType == TASK_CONSUMER_REQ_SUBSCRIBE)
    {
        NS_VERIFY_NOT_NULL_V(task->taskData);
        NS_LOG(DEBUG, "Request Subscribe");
        NSResult ret = NSConsumerSubscribeProvider((NSProvider *)task->taskData);
        NS_VERIFY_NOT_NULL_V(ret == NS_OK ? (void *)1 : NULL);
    }
    else if (task->taskType == TASK_SEND_SYNCINFO)
    {
        NS_VERIFY_NOT_NULL_V(task->taskData);
        NSSyncInfo_internal * syncInfo = (NSSyncInfo_internal *)task->taskData;
        NSProviderConnectionInfo * info = syncInfo->connection;

        while(info)
        {
            OCStackResult ret = NSSendSyncInfo((NSSyncInfo *)(task->taskData), info->addr);
            if (ret != OC_STACK_OK)
            {
                NS_LOG_V(ERROR, "send sync info fail : %d", info->addr->adapter);
            }

            info = info->next;
        }

        NSRemoveConnections(syncInfo->connection);
        NSOICFree(syncInfo);
    }
    else if (task->taskType == TASK_CONSUMER_REQ_SUBSCRIBE_CANCEL)
    {
        NSProvider_internal * provider = (NSProvider_internal *)task->taskData;

        NSProviderConnectionInfo * connections = provider->connection;
        while(connections)
        {
            if (connections->isSubscribing == false)
            {
                NS_LOG_V(DEBUG, "unsubscribed to %s:%d",
                     connections->addr->addr, connections->addr->port);
                connections = connections->next;
                continue;
            }
            NS_LOG_V(DEBUG, "cancel subscribe to %s:%d",
                     connections->addr->addr, connections->addr->port);
            OCCancel(connections->messageHandle, NS_QOS, NULL, 0);
            OCCancel(connections->syncHandle, NS_QOS, NULL, 0);
            connections->messageHandle = NULL;
            connections->syncHandle = NULL;
            connections->isSubscribing = false;
            connections = connections->next;
        }
    }
    /* TODO next commit, modify code.
    else if (task->taskType == TASK_CONSUMER_REQ_TOPIC_LIST)
    {
        NSProvider_internal * provider = (NSProvider_internal *)task->taskData;

        NSProviderConnectionInfo * connections = provider->connection;
        NS_VERIFY_NOT_NULL_V(connections);

        char * topicUri = OICStrdup(provider->topicUri);

        OCConnectivityType type = CT_DEFAULT;
        if (connections->addr->adapter == OC_ADAPTER_TCP)
        {
            type = CT_ADAPTER_TCP;
            if (connections->isCloudConnection == true)
            {
                topicUri = NSGetCloudUri(provider->providerId, topicUri);
            }
        }

        OCStackResult ret = NSInvokeRequest(NULL, OC_REST_GET, connections->addr,
                                topicUri, NULL, NSIntrospectTopic, (void *) provider, type);
        NS_VERIFY_STACK_SUCCESS_V(NSOCResultToSuccess(ret));
        NSOICFree(topicUri);
    }
    else if (task->taskType == TASK_CONSUMER_GET_TOPIC_LIST)
    {
        NSProvider_internal * provider = (NSProvider_internal *)task->taskData;

        NSProviderConnectionInfo * connections = provider->connection;
        NS_VERIFY_NOT_NULL_V(connections);

        char * topicUri = OICStrdup(provider->topicUri);

        OCConnectivityType type = CT_DEFAULT;
        if (connections->addr->adapter == OC_ADAPTER_TCP)
        {
            type = CT_ADAPTER_TCP;
            if (connections->isCloudConnection == true)
            {
                topicUri = NSGetCloudUri(provider->providerId, topicUri);
            }
        }

        NS_LOG(DEBUG, "get topic query");
        char * query = NULL;
        query = NSMakeRequestUriWithConsumerId(topicUri);
        NS_VERIFY_NOT_NULL_V(query);
        NS_LOG_V(DEBUG, "topic query : %s", query);

        OCStackResult ret = NSInvokeRequest(NULL, OC_REST_GET, connections->addr,
                                query, NULL, NSIntrospectTopic, NULL, type);
        NS_VERIFY_STACK_SUCCESS_V(NSOCResultToSuccess(ret));
        NSOICFree(query);
        NSOICFree(topicUri);
    }
    else if (task->taskType == TASK_CONSUMER_SELECT_TOPIC_LIST)
    {
        NSProvider_internal * provider = (NSProvider_internal *)task->taskData;

        NSProviderConnectionInfo * connections = provider->connection;
        NS_VERIFY_NOT_NULL_V(connections);

        OCRepPayload * payload = OCRepPayloadCreate();
        NS_VERIFY_NOT_NULL_V(payload);
        OCRepPayload ** topicPayload = (OCRepPayload **) OICMalloc(
                                        sizeof(OCRepPayload *)*provider->topicListSize);
        NS_VERIFY_NOT_NULL_V(topicPayload);

        OCRepPayloadSetPropString(payload, NS_ATTRIBUTE_CONSUMER_ID, *NSGetConsumerId());

        NSTopic ** topic = provider->topicList->topics;

        for (int i = 0; i < (int)provider->topicListSize; i++)
        {
            topicPayload[i] = OCRepPayloadCreate();
            OCRepPayloadSetPropString(topicPayload[i], NS_ATTRIBUTE_TOPIC_NAME, topic[i]->topicName);
            OCRepPayloadSetPropInt(topicPayload[i], NS_ATTRIBUTE_TOPIC_SELECTION, topic[i]->state);
        }

        size_t dimensions[3] = {provider->topicListSize, 0, 0};
        OCRepPayloadSetPropObjectArray(payload, NS_ATTRIBUTE_TOPIC_LIST, (const OCRepPayload **)topicPayload, dimensions);

        char * topicUri = OICStrdup(provider->topicUri);

        OCConnectivityType type = CT_DEFAULT;
        if (connections->addr->adapter == OC_ADAPTER_TCP)
        {
            type = CT_ADAPTER_TCP;
            if (connections->isCloudConnection == true)
            {
                topicUri = NSGetCloudUri(provider->providerId, topicUri);
            }
        }

        NS_LOG(DEBUG, "get topic query");
        char * query = NULL;
        query = NSMakeRequestUriWithConsumerId(topicUri);
        NS_VERIFY_NOT_NULL_V(query);
        NS_LOG_V(DEBUG, "topic query : %s", query);

        OCStackResult ret = NSInvokeRequest(NULL, OC_REST_GET, connections->addr,
                                query, (OCPayload*)payload, NSConsumerCheckPostResult, NULL, type);
        NS_VERIFY_STACK_SUCCESS_V(NSOCResultToSuccess(ret));
        NSOICFree(query);
        NSOICFree(topicUri);
    }*/
    else
    {
        NS_LOG(ERROR, "Unknown type message");
    }

    NSOICFree(task);
}

void NSGetTopicPostClean(
        char * cId, NSTopicList * tList, size_t dSize)
{
    /* TODO next commit, modify code.
    NSOICFree(cId);
    NSRemoveProviderTopicList(tList, dSize);
    */
}

NSTopicList * NSGetTopic(OCClientResponse * clientResponse, size_t * topicListSize)
{
    /* TODO next commit, modify code.
    NS_LOG(DEBUG, "create NSTopic");
    NS_VERIFY_NOT_NULL(clientResponse->payload, NULL);

    OCRepPayload * payload = (OCRepPayload *)clientResponse->payload;
    while (payload)
    {
        NS_LOG_V(DEBUG, "Payload Key : %s", payload->values->name);
        payload = payload->next;
    }

    payload = (OCRepPayload *)clientResponse->payload;

    char * consumerId = NULL;
    OCRepPayload ** topicListPayload = NULL;
    NSTopicList * topicList = (NSTopicList *) OICMalloc(sizeof(NSTopicList));
    NS_VERIFY_NOT_NULL(topicList, NULL);

    NS_LOG(DEBUG, "get information of consumerId");
    bool getResult = OCRepPayloadGetPropString(payload, NS_ATTRIBUTE_CONSUMER_ID, & consumerId); // is NULL possible? (initial getting)
    NS_VERIFY_NOT_NULL(getResult == true ? (void *) 1 : NULL, NULL);

    OICStrcpy(topicList->consumerId, NS_DEVICE_ID_LENGTH, consumerId);

    OCRepPayloadValue * payloadValue = NULL;
    payloadValue = NSPayloadFindValue(payload, NS_ATTRIBUTE_TOPIC_LIST);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(payloadValue, NULL, NSOICFree(consumerId));

    size_t dimensionSize = calcDimTotal(payloadValue->arr.dimensions);
    size_t dimensions[3] = {dimensionSize, 0, 0};
    *topicListSize = dimensionSize;

    NS_LOG(DEBUG, "get information of topicList(OCRepPayload)");
    getResult = OCRepPayloadGetPropObjectArray(payload, NS_ATTRIBUTE_TOPIC_LIST, 
            & topicListPayload, dimensions);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(getResult == true ? (void *) 1 : NULL, 
            NULL, NSOICFree(consumerId));

    topicList->topics = (NSTopic **) OICMalloc(sizeof(NSTopic *)*dimensionSize);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(topicList->topics,
            NULL, NSGetTopicPostClean(consumerId, topicList, -1));

    for (int i = 0; i < (int)dimensionSize; i++)
    {
        char * topicName = NULL;
        int64_t state = 0;

        topicList->topics[i] = (NSTopic *) OICMalloc(sizeof(NSTopic));
        NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(topicList->topics[i],
                NULL, NSGetTopicPostClean(consumerId, topicList, i));

        NS_LOG(DEBUG, "get topic name");
        getResult = OCRepPayloadGetPropString(topicListPayload[i], NS_ATTRIBUTE_TOPIC_NAME, &topicName);
        NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(getResult == true ? (void *) 1 : NULL,
                NULL, NSGetTopicPostClean(consumerId, topicList, i));


        NS_LOG(DEBUG, "get topic selection");
        getResult = OCRepPayloadGetPropInt(topicListPayload[i], NS_ATTRIBUTE_TOPIC_SELECTION, &state);
        NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(getResult == true ? (void *) 1 : NULL,
                NULL, NSGetTopicPostClean(consumerId, topicList, i));

        topicList->topics[i]->topicName = topicName;
        topicList->topics[i]->state = state;
    }

    NSOICFree(consumerId);

    return topicList;*/
}

OCStackApplicationResult NSIntrospectTopic(
        void * ctx, OCDoHandle handle, OCClientResponse * clientResponse)
{
/* TODO next commit, modify code.
    (void) handle;

    NS_VERIFY_NOT_NULL(clientResponse, OC_STACK_KEEP_TRANSACTION);
    NS_VERIFY_STACK_SUCCESS(NSOCResultToSuccess(clientResponse->result), OC_STACK_KEEP_TRANSACTION);

    NS_LOG_V(DEBUG, "GET response income : %s:%d",
            clientResponse->devAddr.addr, clientResponse->devAddr.port);
    NS_LOG_V(DEBUG, "GET response result : %d",
            clientResponse->result);
    NS_LOG_V(DEBUG, "GET response sequenceNum : %d",
            clientResponse->sequenceNumber);
    NS_LOG_V(DEBUG, "GET response resource uri : %s",
            clientResponse->resourceUri);
    NS_LOG_V(DEBUG, "GET response Transport Type : %d",
                    clientResponse->devAddr.adapter);

    size_t topicListSize = 0;
    NSTopicList * newTopicList = NSGetTopic(clientResponse, &topicListSize);
    NS_VERIFY_NOT_NULL(newTopicList, OC_STACK_KEEP_TRANSACTION);

    // TODO Call the callback function registered at the start
    NSProvider_internal * provider = (NSProvider_internal *) ctx;
    provider->topicList = NSCopyProviderTopicList(newTopicList, topicListSize);
    provider->topicListSize = topicListSize;

    NS_LOG(DEBUG, "build NSTask");
    NSTask * task = NSMakeTask(TASK_CONSUMER_RECV_TOPIC_LIST, (void *) provider);
    NS_VERIFY_NOT_NULL_WITH_POST_CLEANING(task, NS_ERROR, NSRemoveProvider(provider));

    NSConsumerPushEvent(task);
    NSRemoveProviderTopicList(newTopicList, topicListSize);

    return OC_STACK_KEEP_TRANSACTION;
    */
}
