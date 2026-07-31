#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <stdio.h>

static gboolean wait_for_promise(GstPromise* promise, const char* operation)
{
    GstPromiseResult result = gst_promise_wait(promise);

    if (result == GST_PROMISE_RESULT_REPLIED)
        return TRUE;

    fprintf(stderr, "%s failed: promise result=%d\n", operation, result);
    return FALSE;
}

static void on_new_transceiver(
    GstElement* webrtc, GstWebRTCRTPTransceiver* transceiver, gpointer user_data)
{
    (void)webrtc;
    (void)user_data;
    g_object_set(transceiver, "do-nack", TRUE, NULL);
}

int main(int argc, char** argv)
{
    GstElement* webrtc;
    GstPromise* promise;
    GstSDPMessage* sdp = NULL;
    GstWebRTCSessionDescription* offer = NULL;
    GstWebRTCSessionDescription* answer = NULL;
    const GstStructure* reply;
    gchar* offer_text = NULL;
    gchar* answer_text = NULL;
    gsize offer_length = 0;
    GError* error = NULL;
    int status = 1;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <offer.sdp>\n", argv[0]);
        return 2;
    }

    gst_init(&argc, &argv);
    if (!g_file_get_contents(argv[1], &offer_text, &offer_length, &error)) {
        fprintf(stderr, "read %s: %s\n", argv[1], error->message);
        g_clear_error(&error);
        return 1;
    }

    if (gst_sdp_message_new(&sdp) != GST_SDP_OK
        || gst_sdp_message_parse_buffer(
               (const guint8*)offer_text, offer_length, sdp)
            != GST_SDP_OK) {
        fprintf(stderr, "failed to parse SDP offer\n");
        goto done;
    }

    webrtc = gst_element_factory_make("webrtcbin", "answer-probe");
    if (!webrtc) {
        fprintf(stderr, "failed to create webrtcbin\n");
        goto done;
    }
    g_signal_connect(
        webrtc, "on-new-transceiver", G_CALLBACK(on_new_transceiver), NULL);
    if (gst_element_set_state(webrtc, GST_STATE_READY)
        == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "failed to set webrtcbin READY\n");
        gst_object_unref(webrtc);
        goto done;
    }

    offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    sdp = NULL;
    promise = gst_promise_new();
    g_signal_emit_by_name(
        webrtc, "set-remote-description", offer, promise);
    if (!wait_for_promise(promise, "set-remote-description")) {
        gst_promise_unref(promise);
        goto stop;
    }
    gst_promise_unref(promise);

    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc, "create-answer", NULL, promise);
    if (!wait_for_promise(promise, "create-answer")) {
        gst_promise_unref(promise);
        goto stop;
    }

    reply = gst_promise_get_reply(promise);
    if (!reply
        || !gst_structure_get(reply, "answer",
            GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL)) {
        fprintf(stderr, "create-answer returned no answer\n");
        gst_promise_unref(promise);
        goto stop;
    }
    gst_promise_unref(promise);

    answer_text = gst_sdp_message_as_text(answer->sdp);
    if (!answer_text) {
        fprintf(stderr, "failed to serialize SDP answer\n");
        goto stop;
    }
    fputs(answer_text, stdout);
    status = 0;

stop:
    gst_element_set_state(webrtc, GST_STATE_NULL);
    gst_object_unref(webrtc);
done:
    g_free(answer_text);
    if (answer)
        gst_webrtc_session_description_free(answer);
    if (offer)
        gst_webrtc_session_description_free(offer);
    if (sdp)
        gst_sdp_message_free(sdp);
    g_free(offer_text);
    gst_deinit();
    return status;
}
