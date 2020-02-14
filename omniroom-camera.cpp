// Copyright 2019 Nicolas Ballet

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <exception>

#include "json.hpp"

using std::cout;
using std::endl;
using std::string;
using std::vector;
using std::map;
using std::exception;
using json = nlohmann::json;

enum AppState {
    APP_STATE_UNKNOWN = 0,
    APP_STATE_ERROR = 1, /* generic error */
    SERVER_CONNECTING = 1000,
    SERVER_CONNECTION_ERROR,
    SERVER_CONNECTED, /* Ready to register */
    SERVER_REGISTERING = 2000,
    SERVER_REGISTRATION_ERROR,
    SERVER_REGISTERED, /* Ready to call a peer */
    SERVER_CLOSED, /* server connection closed by us or the server */
    ROOM_JOINING = 3000,
    ROOM_JOIN_ERROR,
    ROOM_JOINED,
    ROOM_CALL_NEGOTIATING = 4000, /* negotiating with some or all peers */
    ROOM_CALL_OFFERING, /* when we're the one sending the offer */
    ROOM_CALL_ANSWERING, /* when we're the one answering an offer */
    ROOM_CALL_STARTED, /* in a call with some or all peers */
    ROOM_CALL_STOPPING,
    ROOM_CALL_STOPPED,
    ROOM_CALL_ERROR,
};

static GMainLoop *loop;
static GstElement *pipeline;
static vector<string> peers;

typedef void (*callback)(json data);
static map<string, callback> commandsMapping;

static string local_id;
static string server_address = "127.0.0.1";
static int server_port = 8000;
static string input_stream = "videotestsrc ! x264enc";
static string payload_stream = "rtph264pay ! application/x-rtp,media=video,encoding-name=H264,payload=96";

static SoupWebsocketConnection *ws_conn = NULL;
static enum AppState app_state = APP_STATE_UNKNOWN;

static bool cleanup_and_quit_loop(string msg, enum AppState state) {
    if (!msg.empty())
        cout << msg << endl;

    if (state > 0)
        app_state = state;

    if (ws_conn) {
        if (soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN)
            /* This will call us again */
            soup_websocket_connection_close(ws_conn, 1000, "");
        else
            g_object_unref(ws_conn);
    }

    if (loop) {
        g_main_loop_quit(loop);
        loop = NULL;
    }

    /* To allow usage as a GSourceFunc */
    return G_SOURCE_REMOVE;
}


static void sendICECandidate(GstElement* webrtc G_GNUC_UNUSED, guint mlineindex, gchar* candidate, gpointer peer_id) {
    if (app_state < ROOM_CALL_OFFERING) {
        cleanup_and_quit_loop("Can't send ICE, not in call", APP_STATE_ERROR);
        return;
    }

    std::string* identifier = static_cast<std::string*>(peer_id);

    json ice;
    ice["candidate"] = candidate;
    ice["sdpMLineIndex"] = mlineindex;

    json sdp;
    sdp["command"] = "ICE_CANDIDATE";
    sdp["identifier"] = peers.back();
    sdp["ice"] = ice;
    soup_websocket_connection_send_text(ws_conn, sdp.dump().c_str());
}


static void sendSDPOffer(GstWebRTCSessionDescription* desc, string peer_id) {
    g_assert_cmpint(app_state, >=, ROOM_CALL_OFFERING);

    string text = gst_sdp_message_as_text(desc->sdp);
    cout << "Sending sdp offer to " << peer_id << endl << text << endl;

    json sdp;
    sdp["command"] = "SDP_OFFER";
    sdp["identifier"] = peer_id;
    sdp["offer"]["type"] = "offer";
    sdp["offer"]["sdp"] = text;
    soup_websocket_connection_send_text(ws_conn, sdp.dump().c_str());
}


/* Offer created by our pipeline, to be sent to the peer */
static void onOfferCreated(GstPromise* promise, gpointer peer_id) {
    GstElement *webrtc;
    GstWebRTCSessionDescription *offer;
    const GstStructure *reply;

    std::string* identifier = static_cast<std::string*>(peer_id);

    cout << "Offer created" << endl;

    g_assert_cmpint(app_state, ==, ROOM_CALL_OFFERING);

    g_assert_cmpint(gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), identifier->c_str());
    g_assert_nonnull(webrtc);
    g_signal_emit_by_name(webrtc, "set-local-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send offer to peer */
    sendSDPOffer(offer, *identifier);
    gst_webrtc_session_description_free(offer);
}


static void onNegotiationNeeded(GstElement* webrtc, gpointer peer_id) {
    GstPromise *promise;

    std::string* identifier = static_cast<std::string*>(peer_id);

    cout << "Negotiation needed" << endl;

    app_state = ROOM_CALL_OFFERING;
    promise = gst_promise_new_with_change_func((GstPromiseChangeFunc) onOfferCreated, peer_id, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}


static void remove_peer_from_pipeline(string peer_id) {
    gchar *qname;
    GstPad *srcpad, *sinkpad;
    GstElement *webrtc, *q, *tee;

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), peer_id.c_str());
    if (!webrtc)
        return;

    gst_bin_remove(GST_BIN(pipeline), webrtc);
    gst_object_unref(webrtc);

    qname = g_strdup_printf("queue-%s", peer_id);
    q = gst_bin_get_by_name(GST_BIN(pipeline), qname);
    g_free(qname);

    sinkpad = gst_element_get_static_pad(q, "sink");
    g_assert_nonnull(sinkpad);
    srcpad = gst_pad_get_peer(sinkpad);
    g_assert_nonnull(srcpad);
    gst_object_unref(sinkpad);

    gst_bin_remove(GST_BIN(pipeline), q);
    gst_object_unref(q);

    tee = gst_bin_get_by_name(GST_BIN(pipeline), "videotee");
    g_assert_nonnull(tee);
    gst_element_release_request_pad(tee, srcpad);
    gst_object_unref(srcpad);
    gst_object_unref(tee);
}


static void add_peer_to_pipeline(string peer_id, gboolean offer) {
    int ret;
    GstElement *tee, *webrtc, *q;
    GstPad *srcpad, *sinkpad;

    string name = "queue-" + peer_id;
    q = gst_element_factory_make("queue", name.c_str());
    g_assert_nonnull(q);
    cout << "Created webrtcbin: " << peer_id << endl;
    webrtc = gst_element_factory_make("webrtcbin", peer_id.c_str());
    g_assert_nonnull(webrtc);

    g_assert_nonnull(pipeline);
    gst_bin_add_many(GST_BIN(pipeline), q, webrtc, NULL);

    srcpad = gst_element_get_static_pad(q, "src");
    g_assert_nonnull(srcpad);
    sinkpad = gst_element_get_request_pad(webrtc, "sink_%u");
    g_assert_nonnull(sinkpad);
    ret = gst_pad_link(srcpad, sinkpad);
    g_assert_cmpint(ret, ==, GST_PAD_LINK_OK);
    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);

    tee = gst_bin_get_by_name(GST_BIN(pipeline), "videotee");
    g_assert_nonnull(tee);
    srcpad = gst_element_get_request_pad(tee, "src_%u");
    g_assert_nonnull(srcpad);
    gst_object_unref(tee);
    sinkpad = gst_element_get_static_pad(q, "sink");
    g_assert_nonnull(sinkpad);
    ret = gst_pad_link(srcpad, sinkpad);
    g_assert_cmpint(ret, ==, GST_PAD_LINK_OK);
    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);

    /* This is the gstwebrtc entry point where we create the offer and so on. It
     * will be called when the pipeline goes to PLAYING.
     * XXX: We must connect this after webrtcbin has been linked to a source via
     * get_request_pad() and before we go from NULL->READY otherwise webrtcbin
     * will create an SDP offer with no media lines in it. */
    peers.push_back(peer_id);
    if (offer) {
        cout << "Offer" << endl;
        g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(onNegotiationNeeded), static_cast<gpointer>(&(peers.back())));
    } else {
        cout << "No offer" << endl;
    }

    /* We need to transmit this ICE candidate to the browser via the websockets
     * signalling server. Incoming ice candidates from the browser need to be
     * added by us too, see on_server_message() */
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(sendICECandidate), static_cast<gpointer>(&(peers.back())));

    /* Set to pipeline branch to PLAYING */
    ret = gst_element_sync_state_with_parent(q);
    g_assert_true(ret);
    ret = gst_element_sync_state_with_parent(webrtc);
    g_assert_true(ret);
}


static void callPeer(json data) {
    cout << "Calling peer... " << data["identifier"].get<string>() << endl;
    add_peer_to_pipeline(data["identifier"].get<string>(), true);
}

static gboolean start_pipeline(void) {
    GstStateChangeReturn ret;
    GError *error = NULL;

    /* NOTE: webrtcbin currently does not support dynamic addition/removal of
     * streams, so we use a separate webrtcbin for each peer, but all of them are
     * inside the same pipeline. We start by connecting it to a fakesink so that
     * we can preroll early. */
    const string pipeline_stream = input_stream + " ! " + payload_stream + " ! queue ! tee name=videotee ! queue ! fakesink";
    cout << "Pipeline: " << pipeline_stream << endl;
    pipeline = gst_parse_launch(pipeline_stream.c_str(), &error);

    if (error) {
        g_printerr("Failed to parse launch: %s\n", error->message);
        g_error_free(error);
        goto err;
    }

    g_print("Starting pipeline, not transmitting yet\n");
    ret = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
        goto err;

    return true;

err:
    g_print("State change failure\n");
    if (pipeline)
        g_clear_object(&pipeline);
    return false;
}


static bool join() {
    if (soup_websocket_connection_get_state(ws_conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return false;

    cout << "Registering id " << local_id << " with server" << endl;
    app_state = SERVER_REGISTERING;

    string m = "{\"command\": \"JOIN_CAMERA\", \"identifier\": \"" + local_id + "\"}";
    soup_websocket_connection_send_text(ws_conn, m.c_str());
    return true;
}


static void doRegistration(json data) {
    app_state = SERVER_REGISTERED;
    if (!start_pipeline())
          cleanup_and_quit_loop("ERROR: failed to start pipeline", ROOM_CALL_ERROR);
    cout << "Registered with server" << endl;
}


static void notMapped(json data) {
}


static void onSDPAnswer(json data) {
    int ret;
    GstPromise *promise;
    GstElement *webrtc;
    GstSDPMessage *sdp;
    GstWebRTCSessionDescription *answer;

    g_assert_cmpint(app_state, >=, ROOM_CALL_OFFERING);

    cout << "Received SDP answer: " << endl << data["offer"] << endl;

    ret = gst_sdp_message_new(&sdp);
    g_assert_cmpint(ret, ==, GST_SDP_OK);

    std::regex reg("\\\\r\\\\n");
    string sdp_message = std::regex_replace(data["offer"]["sdp"].dump(), reg, "\r\n");

    std::vector<unsigned char> temp_sdp_message(sdp_message.data(), sdp_message.data() + sdp_message.length() + 1);
    ret = gst_sdp_message_parse_buffer(temp_sdp_message.data(), sdp_message.length(), sdp);
    g_assert_cmpint(ret, ==, GST_SDP_OK);

    answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    g_assert_nonnull(answer);

    string identifier = data["identifier"];

    /* Set remote description on our pipeline */
    promise = gst_promise_new();
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), identifier.c_str());
    g_assert_nonnull(webrtc);
    g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
    gst_object_unref(webrtc);
    /* We don't want to be notified when the action is done */
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
}


static void onICEAnswer(json data) {
    cout << "Received ICE Answer" << endl;
    string identifier = data["identifier"];
    string candidate = data["ice"]["candidate"];
    gint sdpmlineindex = data["ice"]["sdpMLineIndex"];

    /* Add ice candidate sent by remote peer */
    GstElement* webrtc = gst_bin_get_by_name(GST_BIN(pipeline), identifier.c_str());
    g_assert_nonnull(webrtc);
    g_signal_emit_by_name(webrtc, "add-ice-candidate", sdpmlineindex, candidate.c_str());
    gst_object_unref(webrtc);
}


static void onClose(SoupWebsocketConnection* conn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    app_state = SERVER_CLOSED;
    cleanup_and_quit_loop("Server connection closed", APP_STATE_UNKNOWN);
}


static void onMessage(SoupWebsocketConnection* conn, SoupWebsocketDataType type, GBytes* message, gpointer user_data) {
    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        gsize size;
        string raw_data = static_cast<const char*>(g_bytes_get_data(message, &size));
        json data = json::parse(raw_data);

        if (commandsMapping.find(data["command"].get<string>()) != commandsMapping.end()) {
            commandsMapping[data["command"].get<string>()](data);
        } else {
            cout << "Command not found: " << data << endl;
        }
    } else {
        cout << "Received unknown binary message, ignoring" << endl;
    }
}


static void onOpen(SoupSession * session, GAsyncResult * res, SoupMessage *msg) {
    GError *error = NULL;

    ws_conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error) {
        cleanup_and_quit_loop(error->message, SERVER_CONNECTION_ERROR);
        g_error_free(error);
        return;
    }

    g_assert_nonnull(ws_conn);

    app_state = SERVER_CONNECTED;
    g_print("Connected to signalling server\n");

    g_signal_connect(ws_conn, "closed", G_CALLBACK(onClose), NULL);
    g_signal_connect(ws_conn, "message", G_CALLBACK(onMessage), NULL);
    join();
}


/*
 * Connect to the signalling server. This is the entrypoint for everything else.
 */
static void connect() {
    SoupSession* session = soup_session_new();
    SoupLogger* logger = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
    soup_session_add_feature(session, SOUP_SESSION_FEATURE(logger));
    g_object_unref(logger);

    string server_url = "ws://" + server_address + ":" + std::to_string(server_port);
    SoupMessage* message = soup_message_new(SOUP_METHOD_GET, server_url.c_str());

    cout << "Connecting to server..." << endl;

    /* Once connected, we will register */
    soup_session_websocket_connect_async(session, message, NULL, NULL, NULL, (GAsyncReadyCallback) onOpen, message);
    app_state = SERVER_CONNECTING;
}


static bool check_plugins(void) {
    int i;
    gboolean ret;
    GstPlugin *plugin;
    GstRegistry *registry;
    const vector<string> needed = {"opus", "nice", "webrtc", "dtls", "srtp", "rtpmanager", "audiotestsrc"};

    registry = gst_registry_get();
    ret = true;
    for (auto need : needed) {
        plugin = gst_registry_find_plugin(registry, need.c_str());
        if (!plugin) {
            cout << "Required gstreamer plugin " << need << " not found" << endl;
            ret = false;
            continue;
        }
        gst_object_unref(plugin);
    }
    return ret;
}


GOptionContext* createContext(int argc, char *argv[]) {
    GOptionContext* context;
    GError* error = NULL;

    gchar* g_local_id;
    gchar* g_server_address;
    int g_server_port;
    gchar* g_input_stream;
    gchar* g_payload_stream;

    GOptionEntry entries[] = {
      { "local-id", 'i', 0, G_OPTION_ARG_STRING, &g_local_id, "Camera identifier", "string" },
      { "server-address", 'a', 0, G_OPTION_ARG_STRING, &g_server_address, "Signalling server's address", "string" },
      { "server-port", 'p', 0, G_OPTION_ARG_INT, &g_server_port, "Signalling server's port", "int" },
      { "input-stream", 0, 0, G_OPTION_ARG_STRING, &g_input_stream, "Stream source and encoding", "string" },
      { "payload-stream", 0, 0, G_OPTION_ARG_STRING, &g_payload_stream, "Stream payload", "string" },
      { NULL },
    };

    context = g_option_context_new("- gstreamer webrtc sendrecv demo");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Error initializing: %s\n", error->message);
        return nullptr;
    }

    if(g_local_id) {
        local_id = string(g_local_id);
    } else {
        cout << "You must provide a local id" << endl;
        return nullptr;
    }

    if(g_server_address) {
        server_address = string(g_server_address);
    }

    if(g_server_port) {
        server_port = g_server_port;
    }

    if(g_input_stream) {
        input_stream = string(g_input_stream);
    }

    if(g_payload_stream) {
        payload_stream = string(g_payload_stream);
    }

    return context;
}


int main(int argc, char *argv[]) {
    GOptionContext* context = createContext(argc, argv);
    if(!context){
        return -1;
    }

    loop = g_main_loop_new(NULL, false);

    commandsMapping["JOINED_CAMERA"] = doRegistration;
    commandsMapping["UPDATE_CAMERAS"] = notMapped;
    commandsMapping["CALL"] = callPeer;
    commandsMapping["SDP_ANSWER"] = onSDPAnswer;
    commandsMapping["ICE_ANSWER"] = onICEAnswer;

    if (!check_plugins())
        return -1;
    connect();

    g_main_loop_run(loop);

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
    g_print("Pipeline stopped\n");

    gst_object_unref(pipeline);
    return 0;
}
