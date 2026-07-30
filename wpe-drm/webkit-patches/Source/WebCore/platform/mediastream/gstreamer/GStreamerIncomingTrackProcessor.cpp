/*
 *  Copyright (C) 2024 Igalia S.L.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "GStreamerIncomingTrackProcessor.h"

#if USE(GSTREAMER_WEBRTC)

#include "GStreamerCommon.h"
#include "GStreamerQuirks.h"
#include "GStreamerRegistryScanner.h"
#include "VideoFrameMetadataGStreamer.h"
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/GMallocString.h>

GST_DEBUG_CATEGORY(webkit_webrtc_incoming_track_processor_debug);
#define GST_CAT_DEFAULT webkit_webrtc_incoming_track_processor_debug

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerIncomingTrackProcessor);

static bool requireRockchipMppH264AccessUnits()
{
    const char* value = g_getenv("WEBKIT_GST_MPP_REQUIRE_H264_AU");
    return value && !g_strcmp0(value, "1");
}

static bool useDirectRemoteAudioSink()
{
    const char* value = g_getenv("WEBKIT_GST_WEBRTC_DIRECT_REMOTE_AUDIO");
    return value && !g_strcmp0(value, "1");
}

static void configureRockchipEarlyVideoRotation(GstElement* element)
{
    const char* quirk = g_getenv("WEBKIT_GST_HOLE_PUNCH_QUIRK");
    if (g_strcmp0(quirk, "rockchip"))
        return;

    auto* factory = gst_element_get_factory(element);
    if (!factory || g_strcmp0(GST_OBJECT_NAME(factory), "mppvideodec")
        || !gstObjectHasProperty(element, "rotation"_s))
        return;

    const char* rotation = g_getenv("WPE_VIDEO_OVERLAY_ROTATION");
    if (!rotation || !*rotation)
        rotation = g_getenv("WPE_DRM_ROTATION");
    if (!rotation || (!g_str_equal(rotation, "90") && !g_str_equal(rotation, "180") && !g_str_equal(rotation, "270")))
        return;

    gst_util_set_object_arg(G_OBJECT(element), "rotation", rotation);
    GST_WARNING_OBJECT(element, "Applying Rockchip overlay rotation %s to early WebRTC decoder", rotation);
}

static void configureLeakyAudioQueue(GstElement* queue)
{
    g_object_set(queue,
        "max-size-buffers", guint(4),
        "max-size-bytes", guint(0),
        "max-size-time", guint64(0),
        "leaky", gint(2),
        "silent", TRUE,
        nullptr);
}

static void discardFloatingElement(GstElement* element)
{
    if (!element)
        return;
    gst_object_ref_sink(element);
    gst_object_unref(element);
}

static void constrainH264ParserToAccessUnits(GstElement* parser)
{
    if (!requireRockchipMppH264AccessUnits()
        || g_object_get_data(G_OBJECT(parser), "webkit-mpp-early-h264-au-probe"))
        return;

    auto srcPad = adoptGRef(gst_element_get_static_pad(parser, "src"));
    if (!srcPad)
        return;

    gst_pad_add_probe(srcPad.get(), GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
        +[](GstPad*, GstPadProbeInfo* info, gpointer) -> GstPadProbeReturn {
            auto* query = GST_PAD_PROBE_INFO_QUERY(info);
            if (!query)
                return GST_PAD_PROBE_OK;

            if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
                GstCaps* caps = nullptr;
                gst_query_parse_accept_caps(query, &caps);
                if (!caps || !doCapsHaveType(caps, "video/x-h264"_s))
                    return GST_PAD_PROBE_OK;

                bool accepted = true;
                for (unsigned i = 0; i < gst_caps_get_size(caps); ++i) {
                    auto* structure = gst_caps_get_structure(caps, i);
                    const char* alignment = gst_structure_get_string(structure, "alignment");
                    const char* streamFormat = gst_structure_get_string(structure, "stream-format");
                    gboolean parsed = FALSE;
                    accepted &= alignment && !g_strcmp0(alignment, "au");
                    accepted &= streamFormat && !g_strcmp0(streamFormat, "byte-stream");
                    accepted &= gst_structure_get_boolean(structure, "parsed", &parsed) && parsed;
                }
                gst_query_set_accept_caps_result(query, accepted);
                return GST_PAD_PROBE_HANDLED;
            }

            if (GST_QUERY_TYPE(query) != GST_QUERY_CAPS)
                return GST_PAD_PROBE_OK;

            GstCaps* filter = nullptr;
            gst_query_parse_caps(query, &filter);
            if (filter && !gst_caps_is_any(filter) && !doCapsHaveType(filter, "video/x-h264"_s))
                return GST_PAD_PROBE_OK;

            auto requiredCaps = adoptGRef(gst_caps_new_simple("video/x-h264",
                "parsed", G_TYPE_BOOLEAN, TRUE,
                "stream-format", G_TYPE_STRING, "byte-stream",
                "alignment", G_TYPE_STRING, "au", nullptr));
            if (filter)
                requiredCaps = adoptGRef(gst_caps_intersect_full(filter, requiredCaps.get(), GST_CAPS_INTERSECT_FIRST));
            gst_query_set_caps_result(query, requiredCaps.get());
            return GST_PAD_PROBE_HANDLED;
        }, nullptr, nullptr);

    g_object_set_data(G_OBJECT(parser), "webkit-mpp-early-h264-au-probe", GINT_TO_POINTER(1));
    GST_WARNING_OBJECT(parser, "Requiring H.264 access units for early Rockchip MPP decoding");
}

GStreamerIncomingTrackProcessor::GStreamerIncomingTrackProcessor()
{
    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_webrtc_incoming_track_processor_debug, "webkitwebrtcincomingtrackprocessor", 0, "WebKit WebRTC Incoming Track Processor");
    });
    m_ntpCaps = adoptGRef(gst_caps_new_empty_simple("timestamp/x-ntp"));
}

void GStreamerIncomingTrackProcessor::configure(ThreadSafeWeakPtr<GStreamerMediaEndpoint>&& endPoint, GRefPtr<GstPad>&& pad)
{
    m_endPoint = WTF::move(endPoint);
    m_pad = WTF::move(pad);

    auto caps = adoptGRef(gst_pad_get_current_caps(m_pad.get()));
    if (!caps)
        caps = adoptGRef(gst_pad_query_caps(m_pad.get(), nullptr));

    ASCIILiteral typeName;
    if (doCapsHaveType(caps.get(), "audio"_s)) {
        typeName = "audio"_s;
        m_data.type = RealtimeMediaSource::Type::Audio;
    } else {
        typeName = "video"_s;
        m_data.type = RealtimeMediaSource::Type::Video;
    }
    m_data.caps = WTF::move(caps);

    GST_DEBUG_OBJECT(m_bin.get(), "Processing track with caps %" GST_PTR_FORMAT, m_data.caps.get());
    auto structure = gst_caps_get_structure(m_data.caps.get(), 0);
    if (auto ssrc = gstStructureGet<unsigned>(structure, "ssrc"_s)) {
        m_data.ssrc = *ssrc;
        auto msIdAttributeName = makeString("ssrc-"_s, *ssrc, "-msid"_s);
        if (auto msIdAttribute = gstStructureGetString(structure, CStringView::unsafeFromUTF8(msIdAttributeName.utf8().data()))) {
            auto components = String(msIdAttribute.span()).split(' ');
            if (components.size() == 2)
                m_sdpMsIdAndTrackId = { WTF::move(components[0]), WTF::move(components[1]) };
        }
    }

    if (auto mid = gstStructureGetString(structure, "a-mid"_s))
        m_data.mid = mid.span();

    m_data.mediaStreamBinName = makeString("incoming-"_s, typeName, "-track-"_s, m_data.ssrc, '-', unsafeSpan(GST_OBJECT_NAME(m_pad.get())));
    m_bin = gst_bin_new(m_data.mediaStreamBinName.ascii().data());

    g_object_get(m_pad.get(), "transceiver", &m_data.transceiver.outPtr(), nullptr);

    auto msIdAttribute = gstStructureGetString(structure, "a-msid"_s);
    if (!msIdAttribute.isEmpty()) {
        if (startsWith(msIdAttribute.span(), " "_s))
            m_sdpMsIdAndTrackId = { emptyString(), msIdAttribute.span().subspan(1) };
        else {
            auto components = String(msIdAttribute.span()).split(' ');
            if (components.size() == 2)
                m_sdpMsIdAndTrackId = { components[0], components[1] };
        }
    }

    if (m_sdpMsIdAndTrackId.second.isEmpty())
        retrieveMediaStreamAndTrackIdFromSDP();

    m_data.mediaStreamId = mediaStreamIdFromPad();

    if (!m_sdpMsIdAndTrackId.second.isEmpty())
        m_data.trackId = m_sdpMsIdAndTrackId.second;

    bool synchronizeIncomingSamples = g_strcmp0(g_getenv("WEBKIT_GST_WEBRTC_DISABLE_INCOMING_SYNC"), "1");
    m_sink = gst_element_factory_make("fakesink", "sink");
    bool asynchronousStateChanges = m_data.type != RealtimeMediaSource::Type::Audio;
    g_object_set(m_sink.get(), "sync", synchronizeIncomingSamples, "async", asynchronousStateChanges,
        "enable-last-sample", FALSE, "qos", TRUE, nullptr);
    GST_INFO_OBJECT(m_sink.get(), "Intermediate incoming-track clock synchronization: %s, async state changes: %s",
        synchronizeIncomingSamples ? "enabled" : "disabled", asynchronousStateChanges ? "enabled" : "disabled");

    auto trackProcessor = incomingTrackProcessor();
    auto queue = gst_element_factory_make("queue", "queue");
    bool directRemoteAudio = m_data.type == RealtimeMediaSource::Type::Audio
        && m_isDecoding && useDirectRemoteAudioSink();
    GstElement* tee = nullptr;
    GstElement* directAudioQueue = nullptr;
    GstElement* directAudioSink = nullptr;
    bool trackProcessorLinked = false;

    if (directRemoteAudio) {
        tee = gst_element_factory_make("tee", "direct-remote-audio-tee");
        directAudioQueue = gst_element_factory_make("queue", "direct-remote-audio-queue");
        if (tee && directAudioQueue)
            directAudioSink = createPlatformAudioSink("video"_s);
        if (!tee || !directAudioQueue || !directAudioSink) {
            GST_ERROR_OBJECT(m_bin.get(), "Direct remote audio elements unavailable; using MediaStream player audio");
            discardFloatingElement(tee);
            discardFloatingElement(directAudioQueue);
            discardFloatingElement(directAudioSink);
            tee = nullptr;
            directAudioQueue = nullptr;
            directAudioSink = nullptr;
            directRemoteAudio = false;
        }
    }

    if (directRemoteAudio) {
        configureLeakyAudioQueue(queue);
        configureLeakyAudioQueue(directAudioQueue);
        g_object_set(m_sink.get(), "sync", FALSE, "async", FALSE, nullptr);
        if (gstObjectHasProperty(directAudioSink, "sync"_s))
            g_object_set(directAudioSink, "sync", FALSE, nullptr);
        if (gstObjectHasProperty(directAudioSink, "async"_s))
            g_object_set(directAudioSink, "async", FALSE, nullptr);

        gst_bin_add_many(GST_BIN_CAST(m_bin.get()), trackProcessor.get(), tee, queue, m_sink.get(),
            directAudioQueue, directAudioSink, nullptr);
        bool linked = gst_element_link(trackProcessor.get(), tee)
            && gst_element_link(tee, queue)
            && gst_element_link(queue, m_sink.get())
            && gst_element_link(tee, directAudioQueue)
            && gst_element_link(directAudioQueue, directAudioSink);
        RELEASE_ASSERT(linked);
        trackProcessorLinked = true;
        GST_WARNING_OBJECT(m_bin.get(), "Using direct remote audio sink with isolated leaky branches");
    } else {
        gst_bin_add_many(GST_BIN_CAST(m_bin.get()), trackProcessor.get(), queue, m_sink.get(), nullptr);
        gst_element_link(queue, m_sink.get());
    }

    auto trackProcessorSrcPad = adoptGRef(gst_element_get_static_pad(trackProcessor.get(), "src"));
    if (trackProcessorSrcPad) {
        if (!trackProcessorLinked) {
            auto queueSinkPad = adoptGRef(gst_element_get_static_pad(queue, "sink"));
            trackProcessorLinked = gst_pad_link(trackProcessorSrcPad.get(), queueSinkPad.get()) == GST_PAD_LINK_OK;
        }
        if (!trackProcessorLinked)
            GST_ERROR_OBJECT(m_bin.get(), "Failed to link static incoming track processor to queue");
        else if (directRemoteAudio)
            GST_INFO_OBJECT(m_bin.get(), "Direct remote audio stays in the endpoint pipeline and is not exposed to RealtimeIncomingSource");
        else if (m_data.type == RealtimeMediaSource::Type::Audio && m_isDecoding
            && g_strcmp0(g_getenv("WEBKIT_GST_WEBRTC_DEFER_STATIC_AUDIO_READY"), "0")) {
            /*
             * connectPad() adds and links this bin before setting it PAUSED.
             * Install the incoming source callbacks on the main thread in that
             * pre-start window, so setBin() never races an active streaming
             * thread touching the same fakesink and pad.
             */
            m_staticAudioNeedsPreStartConnection = true;
            GST_INFO_OBJECT(m_bin.get(), "Deferring static decoded audio track ready to pre-start connection window");
        } else
            trackReady();
    }

    auto sinkPad = adoptGRef(gst_element_get_static_pad(trackProcessor.get(), "sink"));
    gst_element_add_pad(m_bin.get(), gst_ghost_pad_new("sink", sinkPad.get()));

    if (m_data.type != RealtimeMediaSource::Type::Video || !m_isDecoding)
        return;

    auto sinkSinkPad = adoptGRef(gst_element_get_static_pad(m_sink.get(), "sink"));
    gst_pad_add_probe(sinkSinkPad.get(), GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, reinterpret_cast<GstPadProbeCallback>(+[](GstPad*, GstPadProbeInfo* info, gpointer) -> GstPadProbeReturn {
        auto query = GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION)
            return GST_PAD_PROBE_OK;

        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
        return GST_PAD_PROBE_HANDLED;
    }), nullptr, nullptr);
}

String GStreamerIncomingTrackProcessor::mediaStreamIdFromPad()
{
    // Look-up the mediastream ID, using the msid attribute, fall back to pad name if there is no msid.
    String mediaStreamId;
    if (gstObjectHasProperty(m_pad.get(), "msid"_s)) {
        GUniqueOutPtr<char> msidChars;
        g_object_get(m_pad.get(), "msid", &msidChars.outPtr(), nullptr);
        auto msid = GMallocString::unsafeAdoptFromUTF8(WTF::move(msidChars));
        if (msid) {
            mediaStreamId = String(msid.span());
            GST_DEBUG_OBJECT(m_bin.get(), "msid set from pad msid property: %s", msid.utf8());
        }
    }

    if (!mediaStreamId.isEmpty())
        return mediaStreamId;

    if (!m_sdpMsIdAndTrackId.first.isEmpty()) {
        GST_DEBUG_OBJECT(m_bin.get(), "msid set from SDP media msid attribute: '%s'", m_sdpMsIdAndTrackId.first.utf8().data());
        return m_sdpMsIdAndTrackId.first;
    }

    auto name = GMallocString::unsafeAdoptFromUTF8(gst_pad_get_name(m_pad.get()));
    mediaStreamId = name.span();
    GST_DEBUG_OBJECT(m_bin.get(), "msid set from webrtcbin src pad name: %s", name.utf8());
    return mediaStreamId;
}

void GStreamerIncomingTrackProcessor::retrieveMediaStreamAndTrackIdFromSDP()
{
    auto endPoint = m_endPoint.get();
    if (!endPoint)
        return;

    GUniqueOutPtr<GstWebRTCSessionDescription> description;
    g_object_get(endPoint->webrtcBin(), "remote-description", &description.outPtr(), nullptr);

    unsigned mLineIndex;
    g_object_get(m_data.transceiver.get(), "mlineindex", &mLineIndex, nullptr);
    const auto media = gst_sdp_message_get_media(description->sdp, mLineIndex);
    if (!media) [[unlikely]]
        return;

    auto msidAttribute = CStringView::unsafeFromUTF8(gst_sdp_media_get_attribute_val(media, "msid"));
    if (!msidAttribute)
        return;

    GST_LOG_OBJECT(m_bin.get(), "SDP media msid attribute value: %s", msidAttribute.utf8());
    auto components = String(msidAttribute.span()).split(' ');
    if (components.size() != 2)
        return;

    m_sdpMsIdAndTrackId = { components[0], components[1] };
}

GRefPtr<GstElement> GStreamerIncomingTrackProcessor::incomingTrackProcessor()
{
    if (m_data.type == RealtimeMediaSource::Type::Audio) {
        bool forceEarlyAudioDecoding = !g_strcmp0(g_getenv("WEBKIT_GST_WEBRTC_FORCE_EARLY_AUDIO_DECODING"), "1");
        if (!forceEarlyAudioDecoding)
            return createParser();

        auto structure = gst_caps_get_structure(m_data.caps.get(), 0);
        const char* encodingName = gst_structure_get_string(structure, "encoding-name");
        if (!encodingName || g_ascii_strcasecmp(encodingName, "OPUS"))
            return createParser();

        GST_WARNING_OBJECT(m_bin.get(), "Using static rtpopusdepay -> opusdec WebRTC audio chain");
        auto* processor = gst_bin_new("webkit-opus-decoder");
        gst_object_ref_sink(processor);
        auto* depayloader = gst_element_factory_make("rtpopusdepay", nullptr);
        auto* decoder = gst_element_factory_make("opusdec", nullptr);
        auto* converter = gst_element_factory_make("audioconvert", nullptr);
        auto* resampler = gst_element_factory_make("audioresample", nullptr);

        RELEASE_ASSERT(depayloader && decoder && converter && resampler);
        configureMediaStreamAudioDecoder(decoder);
        gst_bin_add_many(GST_BIN_CAST(processor), depayloader, decoder, converter, resampler, nullptr);
        if (!gst_element_link_many(depayloader, decoder, converter, resampler, nullptr)) {
            GST_ERROR_OBJECT(m_bin.get(), "Failed to link static WebRTC Opus decoder chain");
            gst_object_unref(processor);
            return createParser();
        }

        auto depayloaderSinkPad = adoptGRef(gst_element_get_static_pad(depayloader, "sink"));
        installRtpBufferPadProbe(depayloaderSinkPad);
        gst_element_add_pad(processor, gst_ghost_pad_new("sink", depayloaderSinkPad.get()));

        auto resamplerSrcPad = adoptGRef(gst_element_get_static_pad(resampler, "src"));
        gst_element_add_pad(processor, gst_ghost_pad_new("src", resamplerSrcPad.get()));
        m_isDecoding = true;
        return adoptGRef(processor);
    }

    if (m_data.type == RealtimeMediaSource::Type::Video) {
        GST_DEBUG_OBJECT(m_bin.get(), "Requesting a key-frame");
        gst_pad_send_event(m_pad.get(), gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 1));
    }

    bool forceEarlyVideoDecoding = !g_strcmp0(g_getenv("WEBKIT_GST_WEBRTC_FORCE_EARLY_VIDEO_DECODING"), "1");
    GST_DEBUG_OBJECT(m_bin.get(), "Configuring for input caps: %" GST_PTR_FORMAT "%s", m_data.caps.get(), forceEarlyVideoDecoding ? " and early decoding" : "");
    if (!forceEarlyVideoDecoding) {
        auto structure = gst_caps_get_structure(m_data.caps.get(), 0);
        ASSERT(gst_structure_has_name(structure, "application/x-rtp"));
        auto encodingName = gstStructureGetString(structure, "encoding-name"_s);
        auto mediaType = makeString("video/x-"_s, String(encodingName.span()).convertToASCIILowercase());
        auto codecCaps = adoptGRef(gst_caps_new_empty_simple(mediaType.ascii().data()));

        auto& scanner = GStreamerRegistryScanner::singleton();
        if (scanner.areCapsSupported(GStreamerRegistryScanner::Configuration::Decoding, codecCaps, true)) {
            GST_DEBUG_OBJECT(m_bin.get(), "Hardware video decoder detected, deferring decoding to the source client");
            return createParser();
        }
    }

    GST_DEBUG_OBJECT(m_bin.get(), "Preparing video decoder for depayloaded RTP packets");
    GRefPtr<GstElement> decodebin = makeGStreamerElement("decodebin3"_s);
    m_isDecoding = true;

    g_signal_connect_data(decodebin.get(), "deep-element-added", G_CALLBACK(+[](GstBin*, GstBin*, GstElement* element, gpointer userData) {
        auto* factory = gst_element_get_factory(element);
        if (factory && !g_strcmp0(GST_OBJECT_NAME(factory), "h264parse"))
            constrainH264ParserToAccessUnits(element);

        String elementClass = unsafeSpan(gst_element_get_metadata(element, GST_ELEMENT_METADATA_KLASS));
        auto classifiers = elementClass.split('/');
        if (!classifiers.contains("Depayloader"_s))
            return;

        RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
        if (!self)
            return;

        configureVideoRTPDepayloader(element);
        auto pad = adoptGRef(gst_element_get_static_pad(element, "sink"));
        self->installRtpBufferPadProbe(pad);
    }), new ThreadSafeWeakPtr { *this }, [](gpointer data, GClosure*) {
        delete static_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(data);
    }, static_cast<GConnectFlags>(0));

    g_signal_connect_data(decodebin.get(), "element-added", G_CALLBACK(+[](GstBin*, GstElement* element, gpointer userData) {
        String elementClass = unsafeSpan(gst_element_get_metadata(element, GST_ELEMENT_METADATA_KLASS));
        auto classifiers = elementClass.split('/');
        if (!classifiers.contains("Decoder"_s) || !classifiers.contains("Video"_s))
            return;

        RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
        if (!self)
            return;

        self->m_videoDecoderName = configureMediaStreamVideoDecoder(element);
        configureRockchipEarlyVideoRotation(element);
        webkitGstTraceProcessingTimeForElement(element);

        auto sinkPad = adoptGRef(gst_element_get_static_pad(element, "sink"));
        gst_pad_add_probe(sinkPad.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER), [](GstPad*, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
            RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
            if (!self)
                return GST_PAD_PROBE_REMOVE;
            auto buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            if (!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
                self->m_decodedKeyFrames++;
            self->m_framesReceived++;
            return GST_PAD_PROBE_OK;
        }, userData, nullptr);

        auto pad = adoptGRef(gst_element_get_static_pad(element, "src"));
        gst_pad_add_probe(pad.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), [](GstPad* pad, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
            RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
            if (!self)
                return GST_PAD_PROBE_REMOVE;

            if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
                auto event = GST_PAD_PROBE_INFO_EVENT(info);
                if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
                    GstCaps* caps;
                    gst_event_parse_caps(event, &caps);

                    GstVideoInfo info;
                    gst_video_info_init(&info);
                    if (!gst_video_info_from_caps(&info, caps))
                        return GST_PAD_PROBE_OK;

                    self->m_videoSize = { GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info) };
                    gst_util_fraction_to_double(GST_VIDEO_INFO_FPS_N(&info), GST_VIDEO_INFO_FPS_D(&info), &self->m_frameRate);
                }
                return GST_PAD_PROBE_OK;
            }

            self->m_decodedVideoFrames++;

            auto decoder = adoptGRef(gst_pad_get_parent_element(pad));
            auto processingTime = webkitGstBufferGetProcessingTime(gst_pad_probe_info_get_buffer(info), decoder.get());
            if (processingTime.isInvalid())
                return GST_PAD_PROBE_OK;

            self->m_totalVideoDecodeTime += processingTime;
            return GST_PAD_PROBE_OK;
        }, userData, nullptr);
    }), new ThreadSafeWeakPtr { *this }, [](gpointer data, GClosure*) {
        delete static_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(data);
    }, static_cast<GConnectFlags>(0));

    g_signal_connect_data(decodebin.get(), "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer userData) {
        RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
        if (!self)
            return;

        auto queue = adoptGRef(gst_bin_get_by_name(GST_BIN_CAST(self->m_bin.get()), "queue"));
        auto sinkPad = adoptGRef(gst_element_get_static_pad(queue.get(), "sink"));
        gst_pad_link(pad, sinkPad.get());
        self->trackReady();
    }), new ThreadSafeWeakPtr { *this }, [](gpointer data, GClosure*) {
        delete static_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(data);
    }, static_cast<GConnectFlags>(0));

    return decodebin;
}

GRefPtr<GstElement> GStreamerIncomingTrackProcessor::createParser()
{
    GRefPtr<GstElement> parsebin = makeGStreamerElement("parsebin"_s);
    g_signal_connect_data(parsebin.get(), "element-added", G_CALLBACK(+[](GstBin*, GstElement* element, gpointer userData) {
        String elementClass = unsafeSpan(gst_element_get_metadata(element, GST_ELEMENT_METADATA_KLASS));
        auto classifiers = elementClass.split('/');
        if (!classifiers.contains("Depayloader"_s))
            return;

        RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
        if (!self)
            return;

        configureVideoRTPDepayloader(element);
        auto pad = adoptGRef(gst_element_get_static_pad(element, "sink"));
        self->installRtpBufferPadProbe(pad);
    }), new ThreadSafeWeakPtr { *this }, [](gpointer data, GClosure*) {
        delete static_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(data);
    }, static_cast<GConnectFlags>(0));

    auto& quirksManager = GStreamerQuirksManager::singleton();
    if (quirksManager.isEnabled()) {
        // Prevent auto-plugging of hardware-accelerated elements. Those will be used in the playback pipeline.
        g_signal_connect(parsebin.get(), "autoplug-select", G_CALLBACK(+[](GstElement*, GstPad*, GstCaps*, GstElementFactory* factory, gpointer) -> unsigned {
            static auto skipAutoPlug = gstGetAutoplugSelectResult("skip"_s);
            static auto tryAutoPlug = gstGetAutoplugSelectResult("try"_s);
            RELEASE_ASSERT(skipAutoPlug);
            RELEASE_ASSERT(tryAutoPlug);
            auto& quirksManager = GStreamerQuirksManager::singleton();
            auto isHardwareAccelerated = quirksManager.isHardwareAccelerated(factory).value_or(false);
            if (isHardwareAccelerated)
                return *skipAutoPlug;
            return *tryAutoPlug;
        }), nullptr);
    }

    g_signal_connect_data(parsebin.get(), "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer userData) {
        RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
        if (!self)
            return;
        auto queue = adoptGRef(gst_bin_get_by_name(GST_BIN_CAST(self->m_bin.get()), "queue"));
        auto sinkPad = adoptGRef(gst_element_get_static_pad(queue.get(), "sink"));
        gst_pad_link(pad, sinkPad.get());
        self->trackReady();
    }), new ThreadSafeWeakPtr { *this }, [](gpointer data, GClosure*) {
        delete static_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(data);
    }, static_cast<GConnectFlags>(0));
    return parsebin;
}

void GStreamerIncomingTrackProcessor::installRtpBufferPadProbe(const GRefPtr<GstPad>& pad)
{
    gst_pad_add_probe(pad.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER), [](GstPad*, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
        RefPtr self = reinterpret_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(userData)->get();
        if (!self)
            return GST_PAD_PROBE_REMOVE;

        std::optional<VideoFrameTimeMetadata> videoFrameTimeMetadata;

        bool shouldNotifyFirstPacket = false;
        auto buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        {
            GstMappedRtpBuffer rtpBuffer(buffer, GST_MAP_READ);
            if (!rtpBuffer) [[unlikely]]
                return GST_PAD_PROBE_OK;

            if (!self->m_hasReceivedFirstPacket && gst_rtp_buffer_get_marker(rtpBuffer.mappedData())) {
                self->m_hasReceivedFirstPacket = true;
                shouldNotifyFirstPacket = true;
            }

            if (self->m_data.type == RealtimeMediaSource::Type::Video) {
                videoFrameTimeMetadata.emplace(VideoFrameTimeMetadata {
                    .processingDuration { },
                    .captureTime { },
                    .receiveTime { MonotonicTime::now().secondsSinceEpoch() },
                    .rtpTimestamp { gst_rtp_buffer_get_timestamp(rtpBuffer.mappedData()) }
                });
                if (auto referenceTimestampMeta = gst_buffer_get_reference_timestamp_meta(buffer, self->m_ntpCaps.get())) {
                    auto ntpTimestamp = referenceTimestampMeta->timestamp;
                    videoFrameTimeMetadata->captureTime = Seconds::fromNanoseconds(gst_rtcp_ntp_to_unix(ntpTimestamp));
                }
            }
        }

        if (videoFrameTimeMetadata) {
            auto modifiedBuffer = webkitGstBufferSetVideoFrameMetadata(GRefPtr(buffer), WTF::move(videoFrameTimeMetadata));
            gst_pad_probe_info_set_buffer(info, modifiedBuffer.leakRef());
        }
        if (shouldNotifyFirstPacket)
            self->trackHasReceivedFirstPacket();

        return GST_PAD_PROBE_OK;
    }, new ThreadSafeWeakPtr { *this }, reinterpret_cast<GDestroyNotify>(+[](gpointer data) {
        delete static_cast<ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor>*>(data);
    }));
}

bool GStreamerIncomingTrackProcessor::scheduleStaticAudioTrackBeforeStart()
{
    if (!std::exchange(m_staticAudioNeedsPreStartConnection, false))
        return false;

    auto endPoint = m_endPoint.get();
    if (!endPoint || endPoint->isStopped())
        return false;

    m_isReady = true;
    GST_INFO_OBJECT(m_bin.get(), "Scheduling static decoded audio track connection after pad-added unwinds");
    RunLoop::mainSingleton().dispatchAfter(Seconds::fromMilliseconds(50),
        [endPoint = protect(*endPoint), protectedThis = Ref { *this }] {
            if (endPoint->isStopped())
                return;
            endPoint->connectIncomingTrack(protectedThis->m_data);
            gst_element_sync_state_with_parent(protectedThis->m_bin.get());
            GST_INFO_OBJECT(protectedThis->m_bin.get(), "Static decoded audio track connected; bin synchronized with parent");
        });
    return true;
}

void GStreamerIncomingTrackProcessor::trackReady()
{
    auto endPoint = m_endPoint.get();
    if (!endPoint || endPoint->isStopped())
        return;

    bool wasReady = m_isReady;
    m_isReady = true;
    GST_DEBUG_OBJECT(m_bin.get(), "MediaStream %s track %s on pad %" GST_PTR_FORMAT " is ready", m_data.mediaStreamId.utf8().data(), m_data.trackId.utf8().data(), m_pad.get());
    if (!wasReady && m_data.type == RealtimeMediaSource::Type::Video && g_strcmp0(g_getenv("WEBKIT_GST_WEBRTC_LATE_KEYFRAME_RETRY"), "0")) {
        struct DelayedKeyFrameRequest {
            ThreadSafeWeakPtr<GStreamerIncomingTrackProcessor> processor;
            unsigned delayMilliseconds;
        };

        auto scheduleRequest = [this](unsigned delayMilliseconds) {
            auto* request = new DelayedKeyFrameRequest { ThreadSafeWeakPtr { *this }, delayMilliseconds };
            g_timeout_add_full(G_PRIORITY_DEFAULT, delayMilliseconds, +[](gpointer userData) -> gboolean {
                auto* request = static_cast<DelayedKeyFrameRequest*>(userData);
                RefPtr processor = request->processor.get();
                if (!processor || !processor->m_pad)
                    return G_SOURCE_REMOVE;

                auto requested = gst_pad_send_event(processor->m_pad.get(), gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 1));
                GST_INFO_OBJECT(processor->m_bin.get(), "Late consumer key-frame request +%ums: %s", request->delayMilliseconds, requested ? "sent" : "rejected");
                return G_SOURCE_REMOVE;
            }, request, +[](gpointer userData) {
                delete static_cast<DelayedKeyFrameRequest*>(userData);
            });
        };

        scheduleRequest(250);
        scheduleRequest(750);
        scheduleRequest(1500);
    }

    callOnMainThread([endPoint = protect(*endPoint), protectedThis = Ref { *this }] {
        if (!endPoint->isStopped())
            endPoint->connectIncomingTrack(protectedThis->m_data);
    });
}

void GStreamerIncomingTrackProcessor::trackHasReceivedFirstPacket()
{
    auto endPoint = m_endPoint.get();
    if (!endPoint || endPoint->isStopped())
        return;

    GST_DEBUG_OBJECT(m_bin.get(), "MediaStream %s track %s on pad %" GST_PTR_FORMAT " has received its first packet", m_data.mediaStreamId.utf8().data(), m_data.trackId.utf8().data(), m_pad.get());
    callOnMainThread([endPoint = protect(*endPoint), this] {
        if (endPoint->isStopped())
            return;
        endPoint->notifyFirstPacketReceived(m_data);
    });
}

const GstStructure* GStreamerIncomingTrackProcessor::stats()
{
    if (m_data.type == RealtimeMediaSource::Type::Audio)
        return nullptr;

    if (!m_isDecoding)
        return nullptr;

    GUniqueOutPtr<GstStructure> stats;
    g_object_get(m_sink.get(), "stats", &stats.outPtr(), nullptr);

    auto droppedVideoFrames = gstStructureGet<uint64_t>(stats.get(), "dropped"_s).value_or(0);
    m_stats.reset(gst_structure_new("incoming-video-stats", "frames-decoded", G_TYPE_UINT64, m_decodedVideoFrames, "frames-dropped", G_TYPE_UINT64, droppedVideoFrames,
        "frames-received", G_TYPE_UINT64, m_framesReceived, "key-frames-decoded", G_TYPE_UINT64, m_decodedKeyFrames, nullptr));

    if (!m_videoSize.isZero())
        gst_structure_set(m_stats.get(), "frame-width", G_TYPE_UINT, static_cast<unsigned>(m_videoSize.width()), "frame-height", G_TYPE_UINT, static_cast<unsigned>(m_videoSize.height()), nullptr);

    auto averageRate = gstStructureGet<double>(stats.get(), "average-rate"_s);
    if (averageRate && m_frameRate)
        gst_structure_set(m_stats.get(), "frames-per-second", G_TYPE_DOUBLE, *averageRate * m_frameRate, nullptr);

    if (m_totalVideoDecodeTime.isValid())
        gst_structure_set(m_stats.get(), "total-decode-time", G_TYPE_DOUBLE, m_totalVideoDecodeTime.toDouble(), nullptr);

    gst_structure_set(m_stats.get(), "decoder-implementation", G_TYPE_STRING, m_videoDecoderName.utf8().data(), nullptr);

    return m_stats.get();
}

} // namespace WebCore

#undef GST_CAT_DEFAULT

#endif // USE(GSTREAMER_WEBRTC)
