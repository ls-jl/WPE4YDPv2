/*
 * Copyright (C) 2019 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebSocketTaskSoup.h"

#include "NetworkProcess.h"
#include "NetworkSession.h"
#include "NetworkSocketChannel.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/ThreadableWebSocketChannel.h>
#include <cstring>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/GSpanExtras.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {

static inline bool isConnectionError(GError* error)
{
    // If not a SOUP_WEBSOCKET_ERROR_NOT_WEBSOCKET, then it's a connection error.
    return error && !g_error_matches(error, SOUP_WEBSOCKET_ERROR, SOUP_WEBSOCKET_ERROR_NOT_WEBSOCKET);
}

static bool cloudWebSocketDiagnosticsEnabled()
{
    static bool enabled = !g_strcmp0(g_getenv("WEBKIT_CLOUD_WEBSOCKET_DIAGNOSTICS"), "1");
    return enabled;
}

static void logCloudWebSocketPayload(SoupWebsocketConnection* connection, const char* direction, const char* type, size_t byteCount)
{
    if (!cloudWebSocketDiagnosticsEnabled())
        return;

    GUri* uri = connection ? soup_websocket_connection_get_uri(connection) : nullptr;
    g_message("WebSocket %s: host=%s port=%d type=%s bytes=%zu",
        direction,
        uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : "(unknown)",
        uri ? g_uri_get_port(uri) : -1,
        type,
        byteCount);
}

static void logCloudWebSocketEnvelope(SoupWebsocketConnection* connection, const char* direction, std::span<const uint8_t> data)
{
    if (!cloudWebSocketDiagnosticsEnabled() || data.size() < 8)
        return;

    uint32_t envelopeType = static_cast<uint32_t>(data[0]) << 24
        | static_cast<uint32_t>(data[1]) << 16
        | static_cast<uint32_t>(data[2]) << 8
        | static_cast<uint32_t>(data[3]);
    uint32_t declaredLength = static_cast<uint32_t>(data[4]) << 24
        | static_cast<uint32_t>(data[5]) << 16
        | static_cast<uint32_t>(data[6]) << 8
        | static_cast<uint32_t>(data[7]);
    if (envelopeType < 1 || envelopeType > 4 || declaredLength != data.size() - 8)
        return;

    GUri* uri = connection ? soup_websocket_connection_get_uri(connection) : nullptr;
    g_message("WebSocket envelope: host=%s port=%d direction=%s envelope_type=%u declared_bytes=%u total_bytes=%zu",
        uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : "(unknown)",
        uri ? g_uri_get_port(uri) : -1,
        direction,
        envelopeType,
        declaredLength,
        data.size());

    if (envelopeType != 2 || declaredLength < 12)
        return;

    std::span proxyPacket = data.subspan(8);
    if (proxyPacket[0] != 0x45 || proxyPacket[1] != 0x67
        || proxyPacket[proxyPacket.size() - 2] != 0x89
        || proxyPacket[proxyPacket.size() - 1] != 0xab)
        return;

    uint16_t commandID = static_cast<uint16_t>(proxyPacket[2]) << 8
        | static_cast<uint16_t>(proxyPacket[3]);
    uint16_t headerLength = static_cast<uint16_t>(proxyPacket[4]) << 8
        | static_cast<uint16_t>(proxyPacket[5]);
    uint32_t messageLength = static_cast<uint32_t>(proxyPacket[6]) << 24
        | static_cast<uint32_t>(proxyPacket[7]) << 16
        | static_cast<uint32_t>(proxyPacket[8]) << 8
        | static_cast<uint32_t>(proxyPacket[9]);
    if (12ULL + headerLength + messageLength != proxyPacket.size())
        return;

    g_message("WebSocket proxy packet: host=%s port=%d direction=%s command_id=%u header_bytes=%u message_bytes=%u",
        uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : "(unknown)",
        uri ? g_uri_get_port(uri) : -1,
        direction,
        commandID,
        headerLength,
        messageLength);
}

static void logCloudWebSocketRequestMetadata(SoupMessageHeaders* headers)
{
    if (!cloudWebSocketDiagnosticsEnabled() || !headers)
        return;

    auto present = [headers](const char* name) {
        return !!soup_message_headers_get_one(headers, name);
    };
    g_message("WebSocket request metadata: host-header=%d extensions=%d fetch-site=%d fetch-mode=%d fetch-dest=%d client-hints=%d accept-language=%d accept-encoding=%d",
        present("Host"),
        present("Sec-WebSocket-Extensions"),
        present("Sec-Fetch-Site"),
        present("Sec-Fetch-Mode"),
        present("Sec-Fetch-Dest"),
        present("Sec-CH-UA"),
        present("Accept-Language"),
        present("Accept-Encoding"));
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebSocketTask);

Ref<WebSocketTask> WebSocketTask::create(NetworkSocketChannel& channel, const WebCore::ResourceRequest& request, SoupSession* session, SoupMessage* msg, const String& protocol)
{
    return adoptRef(*new WebSocketTask(channel, request, session, msg, protocol));
}

WebSocketTask::WebSocketTask(NetworkSocketChannel& channel, const WebCore::ResourceRequest& request, SoupSession* session, SoupMessage* msg, const String& protocol)
    : m_channel(channel)
    , m_request(request)
    , m_handshakeMessage(msg)
    , m_cancellable(adoptGRef(g_cancellable_new()))
    , m_delayFailTimer(RunLoop::mainSingleton(), "WebSocketTask::DelayFailTimer"_s, this, &WebSocketTask::delayFailTimerFired)
{
    auto protocolList = protocol.split(',');
    GUniquePtr<char*> protocols;
    if (!protocolList.isEmpty()) {
        protocols.reset(static_cast<char**>(g_new0(char*, protocolList.size() + 1)));
        auto protocolsSpan = unsafeMakeSpan(protocols.get(), protocolList.size());
        unsigned i = 0;
        for (auto& subprotocol : protocolList)
            protocolsSpan[i++] = g_strdup(subprotocol.trim(isASCIIWhitespaceWithoutFF<char16_t>).utf8().data());
    }

    {
        // No need to subscribe to the "request-certificate" signal, just set the client certificate upfront.
        auto protectionSpace = WebCore::AuthenticationChallenge::protectionSpaceForClientCertificate(WebCore::soupURIToURL(soup_message_get_uri(msg)));
        auto certificate = protect(channel.session()->networkStorageSession())->credentialStorage().get(m_request.cachePartition(), protectionSpace).certificate();
        soup_message_set_tls_client_certificate(msg, certificate);
    }

    g_signal_connect(msg, "request-certificate-password", G_CALLBACK(+[](SoupMessage* msg, GTlsPassword* tlsPassword, WebSocketTask* task) -> gboolean {
        auto protectionSpace = WebCore::AuthenticationChallenge::protectionSpaceForClientCertificatePassword(WebCore::soupURIToURL(soup_message_get_uri(msg)), tlsPassword);
        auto password = protect(protect(task->m_channel)->session()->networkStorageSession())->credentialStorage().get(task->m_request.cachePartition(), protectionSpace).password().utf8();
        g_tls_password_set_value(tlsPassword, reinterpret_cast<const unsigned char*>(password.data()), password.length());
        soup_message_tls_client_certificate_password_request_complete(msg);
        return TRUE;
    }), this);

    soup_session_websocket_connect_async(session, msg, nullptr, protocols.get(), RunLoopSourcePriority::AsyncIONetwork, m_cancellable.get(),
        [] (GObject* session, GAsyncResult* result, gpointer userData) {
            GUniqueOutPtr<GError> error;
            GRefPtr<SoupWebsocketConnection> connection = adoptGRef(soup_session_websocket_connect_finish(SOUP_SESSION(session), result, &error.outPtr()));
            if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED))
                return;
            auto* task = static_cast<WebSocketTask*>(userData);
            if (isConnectionError(error.get())) {
                task->m_delayErrorMessage = String::fromUTF8(error->message);
                task->m_delayFailTimer.startOneShot(NetworkProcess::randomClosedPortDelay());
                return;
            }
            if (connection) {
                SoupMessage* message = task->m_handshakeMessage.get();
                GUri* uri = message ? soup_message_get_uri(message) : nullptr;
                SoupMessageHeaders* requestHeaders = message ? soup_message_get_request_headers(message) : nullptr;
                const char* protocol = soup_websocket_connection_get_protocol(connection.get());
                const char* cookie = requestHeaders ? soup_message_headers_get_one(requestHeaders, "Cookie") : nullptr;
                if (cloudWebSocketDiagnosticsEnabled())
                    g_message("WebSocket handshake connected: host=%s port=%d protocol=%s cookie-present=%d",
                        uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : "(unknown)",
                        uri ? g_uri_get_port(uri) : -1,
                        protocol ? protocol : "(none)",
                        !!cookie);
                task->didConnect(WTF::move(connection));
            } else {
                SoupMessage* message = task->m_handshakeMessage.get();
                GUri* uri = message ? soup_message_get_uri(message) : nullptr;
                unsigned status = message ? soup_message_get_status(message) : 0;
                const char* reason = message ? soup_message_get_reason_phrase(message) : nullptr;
                SoupMessageHeaders* requestHeaders = message ? soup_message_get_request_headers(message) : nullptr;
                SoupMessageHeaders* responseHeaders = message ? soup_message_get_response_headers(message) : nullptr;
                const char* origin = requestHeaders ? soup_message_headers_get_one(requestHeaders, "Origin") : nullptr;
                const char* requestedProtocol = requestHeaders ? soup_message_headers_get_one(requestHeaders, "Sec-WebSocket-Protocol") : nullptr;
                const char* userAgent = requestHeaders ? soup_message_headers_get_one(requestHeaders, "User-Agent") : nullptr;
                const char* cookie = requestHeaders ? soup_message_headers_get_one(requestHeaders, "Cookie") : nullptr;
                const char* authorization = requestHeaders ? soup_message_headers_get_one(requestHeaders, "Authorization") : nullptr;
                const char* server = responseHeaders ? soup_message_headers_get_one(responseHeaders, "Server") : nullptr;
                const char* contentType = responseHeaders ? soup_message_headers_get_content_type(responseHeaders, nullptr) : nullptr;
                const char* setCookie = responseHeaders ? soup_message_headers_get_one(responseHeaders, "Set-Cookie") : nullptr;
                g_warning("WebSocket handshake failed: host=%s port=%d status=%u reason=%s error-domain=%s error-code=%d error=%s",
                    uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : "(unknown)",
                    uri ? g_uri_get_port(uri) : -1,
                    status,
                    reason ? reason : "(none)",
                    error.get() ? g_quark_to_string(error->domain) : "(none)",
                    error.get() ? error->code : 0,
                    error.get() ? error->message : "(none)");
                g_warning("WebSocket handshake request metadata: origin=%s protocol=%s user-agent=%s cookie-present=%d cookie-bytes=%zu authorization-present=%d",
                    origin ? origin : "(none)",
                    requestedProtocol ? requestedProtocol : "(none)",
                    userAgent ? userAgent : "(none)",
                    !!cookie,
                    cookie ? strlen(cookie) : 0,
                    !!authorization);
                g_warning("WebSocket handshake response metadata: server=%s content-type=%s content-length=%" G_GOFFSET_FORMAT " set-cookie-present=%d",
                    server ? server : "(none)",
                    contentType ? contentType : "(none)",
                    responseHeaders ? soup_message_headers_get_content_length(responseHeaders) : static_cast<goffset>(-1),
                    !!setCookie);
                logCloudWebSocketRequestMetadata(requestHeaders);
                task->didFail(String::fromUTF8(error->message));
            }
        }, this);

    g_signal_connect(msg, "starting", G_CALLBACK(+[](SoupMessage* msg, WebSocketTask* task) {
        task->m_request.updateFromSoupMessageHeaders(soup_message_get_request_headers(msg));
        protect(task->m_channel)->didSendHandshakeRequest(WTF::move(task->m_request));
    }), this);
}

WebSocketTask::~WebSocketTask()
{
    if (m_handshakeMessage)
        g_signal_handlers_disconnect_by_data(m_handshakeMessage.get(), this);

    cancel();
}

String WebSocketTask::acceptedExtensions() const
{
    StringBuilder result;
    GList* extensions = soup_websocket_connection_get_extensions(m_connection.get());
    for (auto* it = extensions; it; it = g_list_next(it)) {
        auto* extension = SOUP_WEBSOCKET_EXTENSION(it->data);

        if (!result.isEmpty())
            result.append(", "_s);
        result.append(String::fromUTF8(SOUP_WEBSOCKET_EXTENSION_GET_CLASS(extension)->name));

        GUniquePtr<char> params(soup_websocket_extension_get_response_params(extension));
        if (params)
            result.append(String::fromUTF8(params.get()));
    }
    return result.toStringPreserveCapacity();
}

void WebSocketTask::didConnect(GRefPtr<SoupWebsocketConnection>&& connection)
{
    m_connection = WTF::move(connection);

    // Use the same maximum payload length as WebKit internal implementation for backwards compatibility.
    static const uint64_t maxPayloadLength = UINT64_C(0x7FFFFFFFFFFFFFFF);
    soup_websocket_connection_set_max_incoming_payload_size(m_connection.get(), maxPayloadLength);

    g_signal_connect_swapped(m_connection.get(), "message", reinterpret_cast<GCallback>(didReceiveMessageCallback), this);
    g_signal_connect_swapped(m_connection.get(), "error", reinterpret_cast<GCallback>(didReceiveErrorCallback), this);
    g_signal_connect_swapped(m_connection.get(), "closed", reinterpret_cast<GCallback>(didCloseCallback), this);

    if (RefPtr channel = m_channel.get()) {
        channel->didConnect(String::fromLatin1(soup_websocket_connection_get_protocol(m_connection.get())), acceptedExtensions());
        channel->didReceiveHandshakeResponse(m_handshakeMessage.get());
    }
    g_signal_handlers_disconnect_by_data(m_handshakeMessage.get(), this);
    m_handshakeMessage = nullptr;
}

void WebSocketTask::didReceiveMessageCallback(WebSocketTask* task, SoupWebsocketDataType dataType, GBytes* message)
{
    if (g_cancellable_is_cancelled(task->m_cancellable.get()))
        return;

    std::span data = span(message);
    logCloudWebSocketPayload(task->m_connection.get(), "inbound",
        dataType == SOUP_WEBSOCKET_DATA_TEXT ? "text" : "binary", data.size());
    if (dataType == SOUP_WEBSOCKET_DATA_BINARY)
        logCloudWebSocketEnvelope(task->m_connection.get(), "inbound", data);
    switch (dataType) {
    case SOUP_WEBSOCKET_DATA_TEXT:
        protect(task->m_channel)->didReceiveText(String::fromUTF8(data));
        break;
    case SOUP_WEBSOCKET_DATA_BINARY:
        protect(task->m_channel)->didReceiveBinaryData(data);
        break;
    }
}

void WebSocketTask::didReceiveErrorCallback(WebSocketTask* task, GError* error)
{
    if (g_cancellable_is_cancelled(task->m_cancellable.get()))
        return;

    g_warning("WebSocket connection error: domain=%s code=%d message=%s",
        error ? g_quark_to_string(error->domain) : "(none)",
        error ? error->code : 0,
        error ? error->message : "(none)");
    task->didFail(String::fromUTF8(error->message));
}

void WebSocketTask::didFail(String&& errorMessage)
{
    if (m_receivedDidFail)
        return;

    RefPtr channel = m_channel.get();
    if (!channel)
        return;

    m_receivedDidFail = true;
    if (m_handshakeMessage) {
        channel->didReceiveHandshakeResponse(m_handshakeMessage.get());
        g_signal_handlers_disconnect_by_data(m_handshakeMessage.get(), this);
        m_handshakeMessage = nullptr;
    }
    channel->didReceiveMessageError(WTF::move(errorMessage));
    if (!m_connection) {
        didClose(SOUP_WEBSOCKET_CLOSE_ABNORMAL, { });
        return;
    }

    if (soup_websocket_connection_get_state(m_connection.get()) == SOUP_WEBSOCKET_STATE_OPEN)
        didClose(WebCore::ThreadableWebSocketChannel::CloseEventCodeAbnormalClosure, { });
}

void WebSocketTask::didCloseCallback(WebSocketTask* task)
{
    auto code = soup_websocket_connection_get_close_code(task->m_connection.get());
    if (!code) {
        // The connection was closed but close frame was not received or sent.
        code = SOUP_WEBSOCKET_CLOSE_ABNORMAL;
    }
    auto reason = String::fromUTF8(soup_websocket_connection_get_close_data(task->m_connection.get()));
    if (cloudWebSocketDiagnosticsEnabled()) {
        GUri* uri = task->m_connection ? soup_websocket_connection_get_uri(task->m_connection.get()) : nullptr;
        g_message("WebSocket closed: host=%s port=%d code=%u reason_bytes=%u",
            uri && g_uri_get_host(uri) ? g_uri_get_host(uri) : "(unknown)",
            uri ? g_uri_get_port(uri) : -1,
            code, reason.sizeInBytes());
    }
    task->didClose(code, WTF::move(reason));
}

void WebSocketTask::didClose(unsigned short code, const String& reason)
{
    if (m_receivedDidClose)
        return;

    m_receivedDidClose = true;
    protect(m_channel)->didClose(code, reason);
}

void WebSocketTask::sendString(std::span<const uint8_t> utf8, CompletionHandler<void()>&& callback)
{
    if (m_connection && soup_websocket_connection_get_state(m_connection.get()) == SOUP_WEBSOCKET_STATE_OPEN) {
        logCloudWebSocketPayload(m_connection.get(), "outbound", "text", utf8.size());
        // Soup is going to copy the data immediately, so we can use g_bytes_new_static() here to avoid more data copies.
        GRefPtr<GBytes> bytes = adoptGRef(g_bytes_new_static(utf8.data(), utf8.size()));
        soup_websocket_connection_send_message(m_connection.get(), SOUP_WEBSOCKET_DATA_TEXT, bytes.get());
    }
    callback();
}

void WebSocketTask::sendData(std::span<const uint8_t> data, CompletionHandler<void()>&& callback)
{
    if (m_connection && soup_websocket_connection_get_state(m_connection.get()) == SOUP_WEBSOCKET_STATE_OPEN) {
        logCloudWebSocketPayload(m_connection.get(), "outbound", "binary", data.size());
        logCloudWebSocketEnvelope(m_connection.get(), "outbound", data);
        soup_websocket_connection_send_binary(m_connection.get(), data.data(), data.size());
    }
    callback();
}

void WebSocketTask::close(int32_t code, const String& reason)
{
    if (m_receivedDidClose)
        return;

    if (cloudWebSocketDiagnosticsEnabled())
        g_message("WebSocket local close requested: code=%d reason_bytes=%u connected=%d", code, reason.sizeInBytes(), !!m_connection);

    if (!m_connection) {
        g_cancellable_cancel(m_cancellable.get());
        didClose(code ? code : SOUP_WEBSOCKET_CLOSE_ABNORMAL, reason);
        return;
    }

    if (code == WebCore::ThreadableWebSocketChannel::CloseEventCodeNotSpecified)
        code = SOUP_WEBSOCKET_CLOSE_NO_STATUS;

    if (soup_websocket_connection_get_state(m_connection.get()) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(m_connection.get(), code, reason.utf8().data());
}

void WebSocketTask::cancel()
{
    g_cancellable_cancel(m_cancellable.get());

    if (m_connection) {
        g_signal_handlers_disconnect_matched(m_connection.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        m_connection = nullptr;
    }
}

void WebSocketTask::resume()
{
}

void WebSocketTask::delayFailTimerFired()
{
    didFail(WTF::move(m_delayErrorMessage));
}

} // namespace WebKit
