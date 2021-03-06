//******************************************************************
//
// Copyright 2015 Intel Mobile Communications GmbH All Rights Reserved.
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

#include <string.h>
#include "ocstack.h"
#include "logger.h"
#include "cainterface.h"
#include "resourcemanager.h"
#include "credresource.h"
#include "policyengine.h"
#include "srmutility.h"
#include "oic_string.h"
#include "oic_malloc.h"
#include "securevirtualresourcetypes.h"
#include "secureresourcemanager.h"
#include "srmresourcestrings.h"
#include "ocresourcehandler.h"

#if defined( __WITH_TLS__) || defined(__WITH_DTLS__)
#include "pkix_interface.h"
#endif //__WITH_TLS__ or __WITH_DTLS__
#define TAG  "OIC_SRM"

//Request Callback handler
static CARequestCallback gRequestHandler = NULL;
//Response Callback handler
static CAResponseCallback gResponseHandler = NULL;
//Error Callback handler
static CAErrorCallback gErrorHandler = NULL;
//Provisioning response callback
static SPResponseCallback gSPResponseHandler = NULL;

/**
 * A single global Request context will suffice as long
 * as SRM is single-threaded.
 */
SRMRequestContext_t g_requestContext;

/**
 * Function to register provisoning API's response callback.
 * @param respHandler response handler callback.
 */
void SRMRegisterProvisioningResponseHandler(SPResponseCallback respHandler)
{
    gSPResponseHandler = respHandler;
}

void SetRequestedResourceType(SRMRequestContext_t *context)
{
    context->resourceType = GetSvrTypeFromUri(context->resourceUri);
}

// Send the response (context->responseInfo) to the requester
// (context->endPoint).
static void SRMSendResponse(SRMRequestContext_t *context)
{
    if (NULL != context
        && NULL != context->requestInfo
        && NULL != context->endPoint)
    {

        if (CA_STATUS_OK == CASendResponse(context->endPoint,
            &(context->responseInfo)))
        {
            OIC_LOG_V(DEBUG, TAG, "SRM response sent.");
            context->responseSent = true;
        }
        else
        {
            OIC_LOG_V(ERROR, TAG, "SRM response failed.");
        }
    }
    else
    {
        OIC_LOG_V(ERROR, TAG, "%s : NULL Parameter(s)",__func__);
    }

    return;
}

// Based on the context->responseVal, either call the entity handler for the
// request (which must send the response), or send an ACCESS_DENIED response.
void SRMGenerateResponse(SRMRequestContext_t *context)
{
    OIC_LOG_V(INFO, TAG, "%s : entering function.", __func__);

    // If Access Granted, validate parameters and then pass request
    // on to resource endpoint.
    if (IsAccessGranted(context->responseVal))
    {
        if(NULL != gRequestHandler
            && NULL != context->endPoint
            && NULL != context->requestInfo)
        {
            OIC_LOG_V(INFO, TAG, "%s : Access granted, passing req to endpoint.",
             __func__);
            gRequestHandler(context->endPoint, context->requestInfo);
            context->responseSent = true; // SRM counts on the endpoint to send
                                          // a response.
        }
        else // error condition; log relevant msg then send DENIED response
        {
            OIC_LOG_V(ERROR, TAG, "%s : Null values in context.", __func__);
            context->responseVal = ACCESS_DENIED_POLICY_ENGINE_ERROR;
            context->responseInfo.result = CA_INTERNAL_SERVER_ERROR;
            SRMSendResponse(context);
        }
    }
    else // Access Denied
    {
        OIC_LOG_V(INFO, TAG, "%s : Access Denied; sending CA_UNAUTHORIZED_REQ.",
         __func__);
        // TODO: in future version, differentiate between types of DENIED.
        // See JIRA issue 1796 (https://jira.iotivity.org/browse/IOT-1796)
        context->responseInfo.result = CA_UNAUTHORIZED_REQ;
        SRMSendResponse(context);
    }
    return;
}

// Set the value of context->resourceUri, based on the context->requestInfo.
void SetResourceUriAndType(SRMRequestContext_t *context)
{
    char *uri = strstr(context->requestInfo->info.resourceUri, "?");
    size_t position = 0;

    if (uri)
    {
        //Skip query and pass the resource uri
        position = uri - context->requestInfo->info.resourceUri;
    }
    else
    {
        position = strlen(context->requestInfo->info.resourceUri);
    }
    if (MAX_URI_LENGTH < position  || 0 > position)
    {
        OIC_LOG_V(ERROR, TAG, "Incorrect URI length.");
        return;
    }
    OICStrcpyPartial(context->resourceUri, MAX_URI_LENGTH + 1,
        context->requestInfo->info.resourceUri, position);

    // Set the resource type.
    context->resourceType = GetSvrTypeFromUri(context->resourceUri);

    return;
}

// Check if this request is asking to access a "sec" = true resource
// over an unsecure channel.  This type of request is forbidden with
// the exception of a few SVRs (see Security Specification).
void CheckRequestForSecResourceOverUnsecureChannel(SRMRequestContext_t *context)
{
    // if request is over unsecure channel, check resource type
    if(false == context->secureChannel)
    {
        OCResource *resPtr = FindResourceByUri(context->resourceUri);
        if (NULL != resPtr)
        {
            // All vertical secure resources and SVR resources other than
            // DOXM & PSTAT should reject requests over unsecure channel.
            if ((((resPtr->resourceProperties) & OC_SECURE)
                && (context->resourceType == NOT_A_SVR_RESOURCE))
                || ((context->resourceType < OIC_SEC_SVR_TYPE_COUNT)
                    && (context->resourceType != OIC_R_DOXM_TYPE)
                    && (context->resourceType != OIC_R_PSTAT_TYPE)))
            {
                // Reject all the requests over coap for secure resource.
                context->responseVal = ACCESS_DENIED_SEC_RESOURCE_OVER_UNSECURE_CHANNEL;
                context->responseInfo.result = CA_FORBIDDEN_REQ;
                SRMSendResponse(context);
            }
        }
    }

    return;
}

void ClearRequestContext(SRMRequestContext_t *context)
{
    if (NULL == context)
    {

        OIC_LOG(ERROR, TAG, "Null context.");
    }
    else
    {
        // Clear context variables.
        context->endPoint = NULL;
        context->resourceType = OIC_RESOURCE_TYPE_ERROR;
        memset(&context->resourceUri, 0, sizeof(context->resourceUri));
        context->requestedPermission = PERMISSION_ERROR;
        memset(&context->responseInfo, 0, sizeof(context->responseInfo));
        context->responseSent = false;
        context->responseVal = ACCESS_DENIED_POLICY_ENGINE_ERROR;
        context->requestInfo = NULL;
        context->secureChannel = false;
        context->slowResponseSent = false;
        context->subjectIdType = SUBJECT_ID_TYPE_ERROR;
        memset(&context->subjectUuid, 0, sizeof(context->subjectUuid));
#ifdef MULTIPLE_OWNER
        memset(&context->payload, 0, context->payloadSize); // TODO Samsung reviewer: please confirm
        context->payloadSize = 0; // TODO Samsung reviewer: please confirm
#endif //MULTIPLE_OWNER
    }

    return;
}

// Returns true iff Request arrived over secure channel
bool isRequestOverSecureChannel(SRMRequestContext_t *context)
{
    OicUuid_t nullSubjectId = {.id = {0}};

    // if flag set, return true
    if(context->endPoint->flags & CA_SECURE)
    {
        return true;
    }
    // a null subject ID indicates CoAP, so if non-null, also return true
    else if(memcmp(context->requestInfo->info.identity.id,
        nullSubjectId.id, sizeof(context->requestInfo->info.identity.id)) != 0)
    {
        return true;
    }

    return false;
}

/**
 * Entry point into SRM, called by lower layer to determine whether an incoming
 * request should be GRANTED or DENIED.
 *
 * @param endPoint object from which the response is received.
 * @param requestInfo contains information for the request.
 */
void SRMRequestHandler(const CAEndpoint_t *endPoint, const CARequestInfo_t *requestInfo)
{
    OIC_LOG(DEBUG, TAG, "Received request from remote device");

    SRMRequestContext_t *ctx = &g_requestContext; // Always use our single ctx for now.

    ClearRequestContext(ctx);

    if (!endPoint || !requestInfo)
    {
        OIC_LOG(ERROR, TAG, "Invalid endPoint or requestInfo; can't process.");
    }
    else
    {
        // Store the endpoint and requestinfo params.
        ctx->endPoint = endPoint;
        ctx->requestInfo = requestInfo;

        // Copy the subjectID.
        memcpy(ctx->subjectUuid.id,
            requestInfo->info.identity.id, sizeof(ctx->subjectUuid.id));
        ctx->subjectIdType = SUBJECT_ID_TYPE_UUID; // only supported type for now

        // Set secure channel boolean.
        ctx->secureChannel = isRequestOverSecureChannel(ctx);

        // Set resource URI and type.
        SetResourceUriAndType(ctx);

        // Initialize responseInfo.
        memcpy(&(ctx->responseInfo.info), &(requestInfo->info),
            sizeof(ctx->responseInfo.info));
        ctx->responseInfo.info.payload = NULL;
        ctx->responseInfo.result = CA_INTERNAL_SERVER_ERROR;
        ctx->responseInfo.info.dataType = CA_RESPONSE_DATA;

        // Before consulting ACL, check if this is a forbidden request type.
        CheckRequestForSecResourceOverUnsecureChannel(ctx);

        // If DENIED response wasn't sent already, then it's time to check ACL.
        if(false == ctx->responseSent)
        {
#ifdef MULTIPLE_OWNER // TODO Samsung: please verify that these two calls belong
                      // here inside this conditional statement.
            // In case of ACL and CRED, The payload required to verify the payload.
            // Payload information will be used for subowner's permission verification.
            ctx->payload = (uint8_t*)requestInfo->info.payload;
            ctx->payloadSize = requestInfo->info.payloadSize;
#endif //MULTIPLE_OWNER

            OIC_LOG_V(DEBUG, TAG, "Processing request with uri, %s for method, %d",
                ctx->requestInfo->info.resourceUri, ctx->requestInfo->method);
            CheckPermission(ctx);
            OIC_LOG_V(DEBUG, TAG, "Request for permission %d received responseVal %d.",
                ctx->requestedPermission, ctx->responseVal);

            // Now that we have determined the correct response and set responseVal,
            // we generate and send the response to the requester.
            SRMGenerateResponse(ctx);
        }
    }

    if(false == ctx->responseSent)
    {
        OIC_LOG(ERROR, TAG, "Exiting SRM without responding to requester!");
    }

    return;
}

/**
 * Handle the response from the SRM.
 *
 * @param endPoint points to the remote endpoint.
 * @param responseInfo contains response information from the endpoint.
 */
void SRMResponseHandler(const CAEndpoint_t *endPoint, const CAResponseInfo_t *responseInfo)
{
    OIC_LOG(DEBUG, TAG, "Received response from remote device");

    // isProvResponse flag is to check whether response is catered by provisioning APIs or not.
    // When token sent by CA response matches with token generated by provisioning request,
    // gSPResponseHandler returns true and response is not sent to RI layer. In case
    // gSPResponseHandler is null and isProvResponse is false response then the response is for
    // RI layer.
    bool isProvResponse = false;

    if (gSPResponseHandler)
    {
        isProvResponse = gSPResponseHandler(endPoint, responseInfo);
    }
    if (!isProvResponse && gResponseHandler)
    {
        gResponseHandler(endPoint, responseInfo);
    }
}

/**
 * Handle the error from the SRM.
 *
 * @param endPoint is the remote endpoint.
 * @param errorInfo contains error information from the endpoint.
 */
void SRMErrorHandler(const CAEndpoint_t *endPoint, const CAErrorInfo_t *errorInfo)
{
    OIC_LOG_V(INFO, TAG, "Received error from remote device with result, %d for request uri, %s",
        errorInfo->result, errorInfo->info.resourceUri);
    if (gErrorHandler)
    {
        gErrorHandler(endPoint, errorInfo);
    }
}

OCStackResult SRMRegisterHandler(CARequestCallback reqHandler,
    CAResponseCallback respHandler, CAErrorCallback errHandler)
{
    OIC_LOG(DEBUG, TAG, "SRMRegisterHandler !!");
    if( !reqHandler || !respHandler || !errHandler)
    {
        OIC_LOG(ERROR, TAG, "Callback handlers are invalid");
        return OC_STACK_INVALID_PARAM;
    }
    gRequestHandler = reqHandler;
    gResponseHandler = respHandler;
    gErrorHandler = errHandler;


#if defined(__WITH_DTLS__) || defined(__WITH_TLS__)
    CARegisterHandler(SRMRequestHandler, SRMResponseHandler, SRMErrorHandler);
#else
    CARegisterHandler(reqHandler, respHandler, errHandler);
#endif /* __WITH_DTLS__ */
    return OC_STACK_OK;
}

OCStackResult SRMRegisterPersistentStorageHandler(OCPersistentStorage* persistentStorageHandler)
{
    OIC_LOG(DEBUG, TAG, "SRMRegisterPersistentStorageHandler !!");
    return OCRegisterPersistentStorageHandler(persistentStorageHandler);
}

OCPersistentStorage* SRMGetPersistentStorageHandler()
{
    return OCGetPersistentStorageHandler();
}

OCStackResult SRMInitSecureResources()
{
    // TODO: temporarily returning OC_STACK_OK every time until default
    // behavior (for when SVR DB is missing) is settled.
    InitSecureResources();
    OCStackResult ret = OC_STACK_OK;
#if defined(__WITH_DTLS__) || defined(__WITH_TLS__)
    if (CA_STATUS_OK != CAregisterPskCredentialsHandler(GetDtlsPskCredentials))
    {
        OIC_LOG(ERROR, TAG, "Failed to revert TLS credential handler.");
        ret = OC_STACK_ERROR;
    }
    CAregisterPkixInfoHandler(GetPkixInfo);
    CAregisterGetCredentialTypesHandler(InitCipherSuiteList);
#endif // __WITH_DTLS__ or __WITH_TLS__
    return ret;
}

void SRMDeInitSecureResources()
{
    DestroySecureResources();
}

bool SRMIsSecurityResourceURI(const char* uri)
{
    if (!uri)
    {
        return false;
    }

    const char *rsrcs[] = {
        OIC_RSRC_SVC_URI,
        OIC_RSRC_AMACL_URI,
        OIC_RSRC_CRL_URI,
        OIC_RSRC_CRED_URI,
        OIC_RSRC_ACL_URI,
        OIC_RSRC_DOXM_URI,
        OIC_RSRC_PSTAT_URI,
        OIC_RSRC_PCONF_URI,
        OIC_RSRC_DPAIRING_URI,
        OIC_RSRC_VER_URI,
        OC_RSRVD_PROV_CRL_URL
    };

    // Remove query from Uri for resource string comparison
    size_t uriLen = strlen(uri);
    char *query = strchr (uri, '?');
    if (query)
    {
        uriLen = query - uri;
    }

    for (size_t i = 0; i < sizeof(rsrcs)/sizeof(rsrcs[0]); i++)
    {
        size_t svrLen = strlen(rsrcs[i]);

        if ((uriLen == svrLen) &&
            (strncmp(uri, rsrcs[i], svrLen) == 0))
        {
            return true;
        }
    }

    return false;
}

/**
 * Get the Secure Virtual Resource (SVR) type from the URI.
 * @param   uri [IN] Pointer to URI in question.
 * @return  The OicSecSvrType_t of the URI passed (note: if not a Secure Virtual
            Resource, e.g. /a/light, will return "NOT_A_SVR_TYPE" enum value)
 */
static const char URI_QUERY_CHAR = '?';
OicSecSvrType_t GetSvrTypeFromUri(const char* uri)
{
    if (!uri)
    {
        return NOT_A_SVR_RESOURCE;
    }

    // Remove query from Uri for resource string comparison
    size_t uriLen = strlen(uri);
    char *query = strchr (uri, URI_QUERY_CHAR);
    if (query)
    {
        uriLen = query - uri;
    }

    size_t svrLen = 0;

    svrLen = strlen(OIC_RSRC_ACL_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_ACL_URI, svrLen))
        {
            return OIC_R_ACL_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_AMACL_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_AMACL_URI, svrLen))
        {
            return OIC_R_AMACL_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_CRED_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_CRED_URI, svrLen))
        {
            return OIC_R_CRED_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_CRL_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_CRL_URI, svrLen))
        {
            return OIC_R_CRL_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_DOXM_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_DOXM_URI, svrLen))
        {
            return OIC_R_DOXM_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_DPAIRING_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_DPAIRING_URI, svrLen))
        {
            return OIC_R_DPAIRING_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_PCONF_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_PCONF_URI, svrLen))
        {
            return OIC_R_PCONF_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_PSTAT_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_PSTAT_URI, svrLen))
        {
            return OIC_R_PSTAT_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_SVC_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_SVC_URI, svrLen))
        {
            return OIC_R_SVC_TYPE;
        }
    }

    svrLen = strlen(OIC_RSRC_SACL_URI);
    if(uriLen == svrLen)
    {
        if(0 == strncmp(uri, OIC_RSRC_SACL_URI, svrLen))
        {
            return OIC_R_SACL_TYPE;
        }
    }

    return NOT_A_SVR_RESOURCE;
}
