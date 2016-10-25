/*
 * //******************************************************************
 * //
 * // Copyright 2016 Samsung Electronics All Rights Reserved.
 * //
 * //-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 * //
 * // Licensed under the Apache License, Version 2.0 (the "License");
 * // you may not use this file except in compliance with the License.
 * // You may obtain a copy of the License at
 * //
 * //      http://www.apache.org/licenses/LICENSE-2.0
 * //
 * // Unless required by applicable law or agreed to in writing, software
 * // distributed under the License is distributed on an "AS IS" BASIS,
 * // WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * // See the License for the specific language governing permissions and
 * // limitations under the License.
 * //
 * //-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */
package org.iotivity.cloud.ciserver;

import java.util.HashMap;
import java.util.Iterator;

import org.iotivity.cloud.base.OICConstants;
import org.iotivity.cloud.base.ServerSystem;
import org.iotivity.cloud.base.connector.ConnectorPool;
import org.iotivity.cloud.base.device.CoapDevice;
import org.iotivity.cloud.base.device.Device;
import org.iotivity.cloud.base.device.HttpDevice;
import org.iotivity.cloud.base.device.IRequestChannel;
import org.iotivity.cloud.base.exception.ClientException;
import org.iotivity.cloud.base.exception.ServerException;
import org.iotivity.cloud.base.exception.ServerException.BadRequestException;
import org.iotivity.cloud.base.exception.ServerException.InternalServerErrorException;
import org.iotivity.cloud.base.exception.ServerException.UnAuthorizedException;
import org.iotivity.cloud.base.protocols.IRequest;
import org.iotivity.cloud.base.protocols.IResponse;
import org.iotivity.cloud.base.protocols.MessageBuilder;
import org.iotivity.cloud.base.protocols.coap.CoapRequest;
import org.iotivity.cloud.base.protocols.coap.CoapResponse;
import org.iotivity.cloud.base.protocols.enums.ContentFormat;
import org.iotivity.cloud.base.protocols.enums.RequestMethod;
import org.iotivity.cloud.base.protocols.enums.ResponseStatus;
import org.iotivity.cloud.base.protocols.http.HCProxyProcessor;
import org.iotivity.cloud.base.server.CoapServer;
import org.iotivity.cloud.base.server.HttpServer;
import org.iotivity.cloud.base.server.Server;
import org.iotivity.cloud.util.Bytes;
import org.iotivity.cloud.util.Cbor;
import org.iotivity.cloud.util.Log;

import io.netty.channel.ChannelDuplexHandler;
import io.netty.channel.ChannelHandler.Sharable;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelPromise;

/**
 *
 * This class provides a set of APIs to manage all of request
 *
 */

public class DeviceServerSystem extends ServerSystem {

    IRequestChannel mRDServer = null;

    public DeviceServerSystem() {
        mRDServer = ConnectorPool.getConnection("rd");
    }

    /**
     *
     * This class provides a set of APIs to manage device pool.
     *
     */
    public class CoapDevicePool {
        HashMap<String, Device> mMapDevice = new HashMap<>();

        /**
         * API for adding device information into pool.
         * 
         * @param device
         *            device to be added
         */
        public void addDevice(Device device) {
            String deviceId = ((CoapDevice) device).getDeviceId();
            synchronized (mMapDevice) {
                mMapDevice.put(deviceId, device);
            }
        }

        /**
         * API for removing device information into pool.
         * 
         * @param device
         *            device to be removed
         */
        public void removeDevice(Device device) throws ClientException {
            String deviceId = ((CoapDevice) device).getDeviceId();
            synchronized (mMapDevice) {
                if (mMapDevice.get(deviceId) == device) {
                    mMapDevice.remove(deviceId);
                }
            }
            removeObserveDevice(device);
        }

        private void removeObserveDevice(Device device) throws ClientException {
            Iterator<String> iterator = mMapDevice.keySet().iterator();
            while (iterator.hasNext()) {
                String deviceId = iterator.next();
                CoapDevice getDevice = (CoapDevice) mDevicePool
                        .queryDevice(deviceId);
                getDevice.removeObserveChannel(
                        ((CoapDevice) device).getRequestChannel());
            }
        }

        /**
         * API for getting device information.
         * 
         * @param deviceId
         *            device id to get device
         */
        public Device queryDevice(String deviceId) {
            Device device = null;
            synchronized (mMapDevice) {
                device = mMapDevice.get(deviceId);
            }
            return device;
        }
    }

    CoapDevicePool mDevicePool = new CoapDevicePool();

    /**
     *
     * This class provides a set of APIs to manage life cycle of coap message.
     *
     */
    @Sharable
    class CoapLifecycleHandler extends ChannelDuplexHandler {
        @Override
        public void channelRead(ChannelHandlerContext ctx, Object msg) {

            if (msg instanceof CoapRequest) {
                try {
                    CoapDevice coapDevice = (CoapDevice) ctx.channel()
                            .attr(ServerSystem.keyDevice).get();

                    if (coapDevice.isExpiredTime()) {
                        throw new UnAuthorizedException("token is expired");
                    }

                    CoapRequest coapRequest = (CoapRequest) msg;
                    IRequestChannel targetChannel = null;
                    if (coapRequest.getUriPathSegments()
                            .contains(Constants.REQ_DEVICE_ID)) {
                        CoapDevice targetDevice = (CoapDevice) mDevicePool
                                .queryDevice(coapRequest.getUriPathSegments()
                                        .get(1));
                        targetChannel = targetDevice.getRequestChannel();
                    }
                    switch (coapRequest.getObserve()) {
                        case SUBSCRIBE:
                            coapDevice.addObserveRequest(
                                    Bytes.bytesToLong(coapRequest.getToken()),
                                    coapRequest);
                            coapDevice.addObserveChannel(targetChannel);
                            break;
                        case UNSUBSCRIBE:
                            coapDevice.removeObserveChannel(targetChannel);
                            coapDevice.removeObserveRequest(
                                    Bytes.bytesToLong(coapRequest.getToken()));
                            break;
                        default:
                            break;
                    }

                } catch (Throwable t) {
                    Log.f(ctx.channel(), t);
                    ResponseStatus responseStatus = t instanceof ServerException
                            ? ((ServerException) t).getErrorResponse()
                            : ResponseStatus.INTERNAL_SERVER_ERROR;
                    ctx.writeAndFlush(MessageBuilder
                            .createResponse((CoapRequest) msg, responseStatus));
                    ctx.close();
                }
            }

            ctx.fireChannelRead(msg);
        }

        @Override
        public void channelActive(ChannelHandlerContext ctx) {
            Device device = ctx.channel().attr(ServerSystem.keyDevice).get();
            // Authenticated device connected

            sendDevicePresence(device.getDeviceId(), "on");
            mDevicePool.addDevice(device);

            device.onConnected();
        }

        @Override
        public void channelInactive(ChannelHandlerContext ctx)
                throws ClientException {
            Device device = ctx.channel().attr(ServerSystem.keyDevice).get();
            // Some cases, this event occurs after new device connected using
            // same di.
            // So compare actual value, and remove if same.
            if (device != null) {
                sendDevicePresence(device.getDeviceId(), "off");

                device.onDisconnected();

                mDevicePool.removeDevice(device);
                ctx.channel().attr(ServerSystem.keyDevice).remove();

            }
        }

        /**
         * API for sending state to resource directory
         * 
         * @param deviceId
         *            device id to be sent to resource directory
         * @param state
         *            device state to be sent to resource directory
         */
        public void sendDevicePresence(String deviceId, String state) {

            Cbor<HashMap<String, Object>> cbor = new Cbor<>();
            HashMap<String, Object> payload = new HashMap<String, Object>();
            payload.put(Constants.REQ_DEVICE_ID, deviceId);
            payload.put(Constants.PRESENCE_STATE, state);
            StringBuffer uriPath = new StringBuffer();
            uriPath.append("/" + Constants.PREFIX_OIC);
            uriPath.append("/" + Constants.DEVICE_PRESENCE_URI);
            mRDServer.sendRequest(MessageBuilder.createRequest(
                    RequestMethod.POST, uriPath.toString(), null,
                    ContentFormat.APPLICATION_CBOR,
                    cbor.encodingPayloadToCbor(payload)), null);
        }
    }

    CoapLifecycleHandler mLifeCycleHandler = new CoapLifecycleHandler();

    @Sharable
    class CoapAuthHandler extends ChannelDuplexHandler {
        private Cbor<HashMap<String, Object>> mCbor = new Cbor<HashMap<String, Object>>();

        @Override
        public void channelActive(ChannelHandlerContext ctx) {
            // Actual channel active should decided after authentication.
        }

        @Override
        public void write(ChannelHandlerContext ctx, Object msg,
                ChannelPromise promise) {

            try {

                if (!(msg instanceof CoapResponse)) {
                    throw new BadRequestException(
                            "this msg type is not CoapResponse");
                }
                // This is CoapResponse
                // Once the response is valid, add this to deviceList
                CoapResponse response = (CoapResponse) msg;

                switch (response.getUriPath()) {

                    case OICConstants.ACCOUNT_SESSION_FULL_URI:
                        HashMap<String, Object> payloadData = mCbor
                                .parsePayloadFromCbor(response.getPayload(),
                                        HashMap.class);

                        if (response.getStatus() != ResponseStatus.CHANGED) {
                            throw new UnAuthorizedException();
                        }

                        if (payloadData == null) {
                            throw new BadRequestException("payload is empty");
                        }
                        int remainTime = (int) payloadData
                                .get(Constants.EXPIRES_IN);

                        Device device = ctx.channel()
                                .attr(ServerSystem.keyDevice).get();
                        ((CoapDevice) device).setExpiredPolicy(remainTime);

                        // Remove current auth handler and replace to
                        // LifeCycleHandler
                        ctx.channel().pipeline().replace(this,
                                "LifeCycleHandler", mLifeCycleHandler);

                        // Raise event that we have Authenticated device
                        ctx.fireChannelActive();

                        break;
                }

                ctx.writeAndFlush(msg);

            } catch (Throwable t) {
                Log.f(ctx.channel(), t);
                ctx.writeAndFlush(msg);
                ctx.close();
            }
        }

        @Override
        public void channelRead(ChannelHandlerContext ctx, Object msg) {

            try {
                if (!(msg instanceof CoapRequest)) {
                    throw new BadRequestException(
                            "this msg type is not CoapRequest");
                }

                // And check first response is VALID then add or cut
                CoapRequest request = (CoapRequest) msg;

                switch (request.getUriPath()) {
                    // Check whether request is about account
                    case OICConstants.ACCOUNT_FULL_URI:
                    case OICConstants.ACCOUNT_TOKENREFRESH_FULL_URI:

                        if (ctx.channel().attr(ServerSystem.keyDevice)
                                .get() == null) {
                            // Create device first and pass to upperlayer
                            Device device = new CoapDevice(ctx);
                            ctx.channel().attr(ServerSystem.keyDevice)
                                    .set(device);
                        }

                        break;

                    case OICConstants.ACCOUNT_SESSION_FULL_URI:

                        HashMap<String, Object> authPayload = mCbor
                                .parsePayloadFromCbor(request.getPayload(),
                                        HashMap.class);

                        Device device = ctx.channel()
                                .attr(ServerSystem.keyDevice).get();

                        if (device == null) {
                            device = new CoapDevice(ctx);
                            ctx.channel().attr(ServerSystem.keyDevice)
                                    .set(device);
                        }

                        if (authPayload == null) {
                            throw new BadRequestException("payload is empty");
                        }

                        ((CoapDevice) device).updateDevice(
                                (String) authPayload.get(Constants.DEVICE_ID),
                                (String) authPayload.get(Constants.USER_ID),
                                (String) authPayload
                                        .get(Constants.ACCESS_TOKEN));

                        break;

                    case OICConstants.KEEP_ALIVE_FULL_URI:
                        // TODO: Pass ping request to upper layer
                        break;

                    default:
                        throw new UnAuthorizedException(
                                "authentication required first");
                }

                ctx.fireChannelRead(msg);

            } catch (Throwable t) {
                ResponseStatus responseStatus = t instanceof ServerException
                        ? ((ServerException) t).getErrorResponse()
                        : ResponseStatus.UNAUTHORIZED;
                ctx.writeAndFlush(MessageBuilder
                        .createResponse((CoapRequest) msg, responseStatus));
                Log.f(ctx.channel(), t);
            }
        }
    }

    @Sharable
    class HttpAuthHandler extends ChannelDuplexHandler {

        private Cbor<HashMap<String, Object>> mCbor = new Cbor<HashMap<String, Object>>();

        @Override
        public void channelRead(ChannelHandlerContext ctx, Object msg) {

            /*
             * The parameter msg has been translated from HTTP request to CoAP request
             * according to https://tools.ietf.org/html/draft-ietf-core-http-mapping-13
             * by the previous handler: HCProxyHandler.
             */

            try {
                if (msg instanceof IRequest) {

                    IRequest request = (IRequest) msg;

                    switch (request.getUriPath()) {
                        // In case of Sign-up and Access Token Refresh
                        case OICConstants.ACCOUNT_FULL_URI:
                        case OICConstants.ACCOUNT_TOKENREFRESH_FULL_URI:

                            if (ctx.channel().attr(ServerSystem.keyDevice)
                                    .get() == null) {
                                // Create a device and pass to next handler
                                Device device = new HttpDevice(ctx);
                                ctx.channel().attr(ServerSystem.keyDevice)
                                        .set(device);
                            }

                            break;

                        // In case of Sign-in
                        case OICConstants.ACCOUNT_SESSION_FULL_URI:

                            HashMap<String, Object> authPayload = mCbor
                                    .parsePayloadFromCbor(request.getPayload(),
                                            HashMap.class);

                            Device device = ctx.channel()
                                    .attr(ServerSystem.keyDevice).get();

                            if (device == null) {
                                device = new HttpDevice(ctx);
                                ctx.channel().attr(ServerSystem.keyDevice)
                                        .set(device);
                            }

                            if (authPayload == null) {
                                throw new BadRequestException(
                                        "Payload is empty.");
                            }

                            ((HttpDevice) device).updateDevice(
                                    (String) authPayload.get(Constants.DEVICE_ID),
                                    (String) authPayload.get(Constants.USER_ID),
                                    (String) authPayload
                                            .get(Constants.ACCESS_TOKEN));

                            break;

                        // In case of other requests (No Sign-up/in request)
                        default:

                            // Check Session-ID
                            boolean isValidSid = false;
                            boolean isExpiredSid = false;
                            String sessionId = ctx.channel()
                                    .attr(HCProxyProcessor.ctxSessionId).get();

                            HttpDevice httpDevice = null;
                            if (sessionId != null) {
                                isValidSid = HttpServer.httpDeviceMap
                                        .containsKey(sessionId);
                                httpDevice = HttpServer.httpDeviceMap
                                        .get(sessionId);

                                if (isValidSid && httpDevice != null) {
                                    // if the session timeout is passed,
                                    // then remove the session
                                    if (httpDevice.getSessionExpiredTime()
                                            <= System.currentTimeMillis()) {
                                        isExpiredSid = true;

                                        HttpServer.httpDeviceMap
                                                .remove(sessionId);

                                        Log.v(ctx.channel().id().asLongText()
                                                .substring(26)
                                                + " HTTP Session-ID Deleted: "
                                                + sessionId);
                                    }
                                }
                            }

                            if (sessionId == null) {
                                throw new UnAuthorizedException(
                                        "Authentication is required.");
                            } else if (!isValidSid) {
                                throw new UnAuthorizedException(
                                        "Session-ID is not valid.");
                            } else if (isExpiredSid) {
                                throw new UnAuthorizedException(
                                        "Session-ID is expired.");
                            } else if (httpDevice == null) {
                                throw new InternalServerErrorException(
                                        "HttpDevice is not found.");
                            }

                            httpDevice.replaceCtx(ctx);
                            ctx.channel().attr(ServerSystem.keyDevice)
                                    .set(httpDevice);
                    }

                    ctx.fireChannelRead(msg);
                }
            } catch (Throwable t) {
                ResponseStatus responseStatus = t instanceof ServerException
                        ? ((ServerException) t).getErrorResponse()
                        : ResponseStatus.UNAUTHORIZED;
                /*
                 * The CoAP response will be translated into HTTP response
                 * by the next handler: HCProxyHandler.
                 */
                ctx.writeAndFlush(MessageBuilder
                        .createResponse((CoapRequest) msg, responseStatus));
                Log.f(ctx.channel(), t);
            }
        }

        @Override
        public void write(ChannelHandlerContext ctx, Object msg,
                ChannelPromise promise) {

            try {
                if (msg instanceof IResponse) {

                    IResponse response = (IResponse) msg;

                    switch (response.getUriPath()) {

                        // In case of Sign-in
                        case OICConstants.ACCOUNT_SESSION_FULL_URI:
                            HashMap<String, Object> payloadData = mCbor
                                    .parsePayloadFromCbor(response.getPayload(),
                                            HashMap.class);

                            if (response.getStatus()
                                    != ResponseStatus.CHANGED) {
                                throw new UnAuthorizedException();
                            }

                            if (payloadData == null) {
                                throw new BadRequestException(
                                        "Payload is empty.");
                            }

                            int remainTime = (int) payloadData
                                    .get(Constants.EXPIRES_IN);

                            Device device = ctx.channel()
                                    .attr(ServerSystem.keyDevice).get();
                            ((HttpDevice) device).setExpiredTime(remainTime);

                            break;
                    }

                    ctx.writeAndFlush(msg);
                }
            } catch (Throwable t) {
                Log.f(ctx.channel(), t);
                ctx.writeAndFlush(msg);
                ctx.close();
            }
        }
    }

    @Override
    public void addServer(Server server) {
        if (server instanceof CoapServer) {
            server.addHandler(new CoapAuthHandler());
        } else if (server instanceof HttpServer) {
            server.addHandler(new HttpAuthHandler());
        }

        super.addServer(server);
    }

    public CoapDevicePool getDevicePool() {
        return mDevicePool;
    }
}
