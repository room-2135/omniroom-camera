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

static SoupWebsocketConnection *ws_conn = NULL;
static enum AppState app_state = APP_STATE_UNKNOWN;
static string server_url = "ws://127.0.0.1:8000";
static string local_id = "test";
static bool strict_ssl = false;

static const gchar* find_peer_from_list(const gchar* peer_id) {
    //return (g_list_find_custom(peers, peer_id, compare_str_glist))->data;
    return "";
}


static bool cleanup_and_quit_loop(string msg, enum AppState state) {
    if(!msg.empty())
        cout << msg << endl;

    if(state > 0)
        app_state = state;

    if(ws_conn) {
        if (soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN)
            /* This will call us again */
            soup_websocket_connection_close(ws_conn, 1000, "");
        else
            g_object_unref(ws_conn);
    }

    if(loop) {
        g_main_loop_quit(loop);
        loop = NULL;
    }

    /* To allow usage as a GSourceFunc */
    return G_SOURCE_REMOVE;
}


static gchar* get_string_from_json_object(JsonObject * object) {
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
}


static void handle_media_stream(GstPad* pad, GstElement* pipe, const char* convert_name, const char* sink_name) {
    GstPad *qpad;
    GstElement *q, *conv, *sink;
    GstPadLinkReturn ret;

    q = gst_element_factory_make("queue", NULL);
    g_assert_nonnull(q);
    conv = gst_element_factory_make(convert_name, NULL);
    g_assert_nonnull(conv);
    sink = gst_element_factory_make(sink_name, NULL);
    g_assert_nonnull(sink);
    gst_bin_add_many(GST_BIN (pipe), q, conv, sink, NULL);
    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(q, conv, sink, NULL);

    qpad = gst_element_get_static_pad(q, "sink");

    ret = gst_pad_link(pad, qpad);
    g_assert_cmpint(ret, ==, GST_PAD_LINK_OK);
}


static void on_incoming_decodebin_stream(GstElement* decodebin, GstPad* pad, GstElement* pipe) {
    GstCaps* caps;
    const gchar* name;

    if (!gst_pad_has_current_caps(pad)) {
        g_printerr("Pad '%s' has no caps, can't do anything, ignoring\n", GST_PAD_NAME(pad));
        return;
    }

    caps = gst_pad_get_current_caps(pad);
    name = gst_structure_get_name(gst_caps_get_structure(caps, 0));

    if(g_str_has_prefix(name, "video")) {
        handle_media_stream(pad, pipe, "videoconvert", "autovideosink");
    } else if(g_str_has_prefix(name, "audio")) {
        handle_media_stream(pad, pipe, "audioconvert", "autoaudiosink");
    } else {
        g_printerr("Unknown pad %s, ignoring", GST_PAD_NAME(pad));
    }
}


static void on_incoming_stream(GstElement* webrtc, GstPad* pad, GstElement* pipe) {
    GstElement *decodebin;
    GstPad* sinkpad;

    if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
        return;

    decodebin = gst_element_factory_make("decodebin", NULL);
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_incoming_decodebin_stream), pipe);
    gst_bin_add(GST_BIN(pipe), decodebin);
    gst_element_sync_state_with_parent(decodebin);

    sinkpad = gst_element_get_static_pad(decodebin, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}


static void send_room_peer_msg(const gchar* text, const gchar* peer_id) {
    gchar* msg;

    msg = g_strdup_printf("ROOM_PEER_MSG %s %s", peer_id, text);
    soup_websocket_connection_send_text(ws_conn, msg);
    g_free(msg);
}


static void send_ice_candidate_message(GstElement* webrtc G_GNUC_UNUSED, guint mlineindex, gchar* candidate, const gchar* peer_id) {
    gchar* text;
    JsonObject *ice, *msg;

    if(app_state < ROOM_CALL_OFFERING) {
        cleanup_and_quit_loop("Can't send ICE, not in call", APP_STATE_ERROR);
        return;
    }

    ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
    msg = json_object_new();
    json_object_set_object_member(msg, "ice", ice);
    text = get_string_from_json_object(msg);
    json_object_unref(msg);

    json sdp;
    sdp["command"] = "ICE_CANDITATE";
    sdp["identifier"] = peer_id;
    sdp["canditate"] = "offer";
    sdp["sdpMLineIndex"] = text;
    soup_websocket_connection_send_text(ws_conn, sdp.dump().c_str());

    g_free(text);
}


static void send_room_peer_sdp(GstWebRTCSessionDescription* desc, string peer_id) {
    string text, sdptype, sdptext;

    g_assert_cmpint(app_state, >=, ROOM_CALL_OFFERING);

    if (desc->type == GST_WEBRTC_SDP_TYPE_OFFER)
        sdptype = "offer";
    else
        g_assert_not_reached();

    cout << "Sending sdp offer to " << peer_id << endl << text << endl;

    text = gst_sdp_message_as_text(desc->sdp);
    json sdp;
    sdp["command"] = "SDP_OFFER";
    sdp["identifier"] = peer_id;
    sdp["offer"]["type"] = "offer";
    sdp["offer"]["offer"] = text;
    soup_websocket_connection_send_text(ws_conn, sdp.dump().c_str());
}


/* Offer created by our pipeline, to be sent to the peer */
static void on_offer_created(GstPromise* promise, string* peer_id) {
    GstElement *webrtc;
    GstWebRTCSessionDescription *offer;
    const GstStructure *reply;

    g_assert_cmpint (app_state, ==, ROOM_CALL_OFFERING);

    g_assert_cmpint (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply (promise);
    gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref (promise);

    promise = gst_promise_new();
    webrtc = gst_bin_get_by_name(GST_BIN (pipeline), peer_id->c_str());
    g_assert_nonnull(webrtc);
    g_signal_emit_by_name(webrtc, "set-local-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send offer to peer */
    send_room_peer_sdp(offer, *peer_id);
    gst_webrtc_session_description_free(offer);
}


static void on_negotiation_needed(GstElement* webrtc, gpointer peer_id) {
    GstPromise *promise;

    app_state = ROOM_CALL_OFFERING;
    promise = gst_promise_new_with_change_func ((GstPromiseChangeFunc) on_offer_created, peer_id, NULL);
    g_signal_emit_by_name (webrtc, "create-offer", NULL, promise);
}


static void remove_peer_from_pipeline(string peer_id) {
    gchar *qname;
    GstPad *srcpad, *sinkpad;
    GstElement *webrtc, *q, *tee;

    webrtc = gst_bin_get_by_name (GST_BIN (pipeline), peer_id.c_str());
    if (!webrtc)
        return;

    gst_bin_remove (GST_BIN (pipeline), webrtc);
    gst_object_unref (webrtc);

    qname = g_strdup_printf ("queue-%s", peer_id);
    q = gst_bin_get_by_name (GST_BIN (pipeline), qname);
    g_free (qname);

    sinkpad = gst_element_get_static_pad (q, "sink");
    g_assert_nonnull (sinkpad);
    srcpad = gst_pad_get_peer (sinkpad);
    g_assert_nonnull (srcpad);
    gst_object_unref (sinkpad);

    gst_bin_remove (GST_BIN (pipeline), q);
    gst_object_unref (q);

    tee = gst_bin_get_by_name (GST_BIN (pipeline), "videotee");
    g_assert_nonnull (tee);
    gst_element_release_request_pad (tee, srcpad);
    gst_object_unref (srcpad);
    gst_object_unref (tee);
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
    gst_bin_add_many(GST_BIN (pipeline), q, webrtc, NULL);

    srcpad = gst_element_get_static_pad(q, "src");
    g_assert_nonnull(srcpad);
    sinkpad = gst_element_get_request_pad(webrtc, "sink_%u");
    g_assert_nonnull(sinkpad);
    ret = gst_pad_link(srcpad, sinkpad);
    g_assert_cmpint(ret, ==, GST_PAD_LINK_OK);
    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);

    tee = gst_bin_get_by_name(GST_BIN (pipeline), "videotee");
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
    cout << "Size: " << peers.size() << endl;
    vector<string>::iterator it = std::find(peers.begin(), peers.end(), peer_id);
    cout << "Test: " << (*it) << endl;
    string* test = &(*it);
    cout << "Test: " << *test << endl;
    if (offer) {
        g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK (on_negotiation_needed), (gpointer) test);
    }

    /* We need to transmit this ICE candidate to the browser via the websockets
     * signalling server. Incoming ice candidates from the browser need to be
     * added by us too, see on_server_message() */
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK (send_ice_candidate_message), (gpointer) peer_id.c_str());
    /* Incoming streams will be exposed via this signal */
    //g_signal_connect (webrtc, "pad-added", G_CALLBACK (on_incoming_stream),
    //        pipeline);

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

#define STR(x) #x
#define RTP_CAPS_OPUS(x) "application/x-rtp,media=audio,encoding-name=OPUS,payload=" STR(x)
#define RTP_CAPS_VP8(x) "application/x-rtp,media=video,encoding-name=VP8,payload=" STR(x)


static gboolean start_pipeline(void) {
    GstStateChangeReturn ret;
    GError *error = NULL;

    /* NOTE: webrtcbin currently does not support dynamic addition/removal of
     * streams, so we use a separate webrtcbin for each peer, but all of them are
     * inside the same pipeline. We start by connecting it to a fakesink so that
     * we can preroll early. */
    pipeline = gst_parse_launch ("tee name=videotee ! queue ! fakesink "
            "videotestsrc ! videoconvert ! videoscale ! video/x-raw,width=800,height=600,framerate=30/1 ! queue ! vp8enc deadline=1 ! rtpvp8pay ! "
            "queue ! " RTP_CAPS_VP8(96) " ! videotee. ",
            &error);

    if (error) {
        g_printerr ("Failed to parse launch: %s\n", error->message);
        g_error_free (error);
        goto err;
    }

    g_print ("Starting pipeline, not transmitting yet\n");
    ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
        goto err;

    return true;

err:
    g_print ("State change failure\n");
    if (pipeline)
        g_clear_object (&pipeline);
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
    if (!start_pipeline ())
          cleanup_and_quit_loop ("ERROR: failed to start pipeline", ROOM_CALL_ERROR);
    cout << "Registered with server" << endl;
}


static void notMapped(json data) {
}


static void handle_error_message(string msg) {
    cout << "Server error: " << msg << endl;
    switch (app_state) {
        case SERVER_CONNECTING:
            app_state = SERVER_CONNECTION_ERROR;
            break;
        case SERVER_REGISTERING:
            app_state = SERVER_REGISTRATION_ERROR;
            break;
        case ROOM_JOINING:
            app_state = ROOM_JOIN_ERROR;
            break;
        case ROOM_JOINED:
        case ROOM_CALL_NEGOTIATING:
        case ROOM_CALL_OFFERING:
        case ROOM_CALL_ANSWERING:
            app_state = ROOM_CALL_ERROR;
            break;
        case ROOM_CALL_STARTED:
        case ROOM_CALL_STOPPING:
        case ROOM_CALL_STOPPED:
            app_state = ROOM_CALL_ERROR;
            break;
        default:
            app_state = APP_STATE_ERROR;
    }
    cleanup_and_quit_loop (msg, APP_STATE_UNKNOWN);
}


static void on_answer_created(GstPromise * promise, string peer_id) {
    GstElement *webrtc;
    GstWebRTCSessionDescription *answer;
    const GstStructure *reply;

    g_assert_cmpint (app_state, ==, ROOM_CALL_ANSWERING);

    g_assert_cmpint (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply (promise);
    gst_structure_get (reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref (promise);

    promise = gst_promise_new ();
    webrtc = gst_bin_get_by_name (GST_BIN (pipeline), peer_id.c_str());
    g_assert_nonnull (webrtc);
    g_signal_emit_by_name (webrtc, "set-local-description", answer, promise);
    gst_promise_interrupt (promise);
    gst_promise_unref (promise);

    /* Send offer to peer */
    send_room_peer_sdp (answer, peer_id);
    gst_webrtc_session_description_free (answer);

    app_state = ROOM_CALL_STARTED;
}


static void handle_sdp_answer(string peer_id, string text) {
    int ret;
    GstPromise *promise;
    GstElement *webrtc;
    GstSDPMessage *sdp;
    GstWebRTCSessionDescription *answer;

    g_assert_cmpint(app_state, >=, ROOM_CALL_OFFERING);

    cout << "Received answer: " << endl << text << endl;

    ret = gst_sdp_message_new(&sdp);
    g_assert_cmpint(ret, ==, GST_SDP_OK);

    ret = gst_sdp_message_parse_buffer((guint8*)text.c_str(), text.length(), sdp);
    g_assert_cmpint (ret, ==, GST_SDP_OK);

    answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    g_assert_nonnull(answer);

    /* Set remote description on our pipeline */
    promise = gst_promise_new();
    webrtc = gst_bin_get_by_name(GST_BIN (pipeline), peer_id.c_str());
    g_assert_nonnull(webrtc);
    g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
    gst_object_unref(webrtc);
    /* We don't want to be notified when the action is done */
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
}


static gboolean handle_peer_message(string peer_id, string msg) {
    JsonNode *root;
    JsonObject *object, *child;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, msg.c_str(), -1, NULL)) {
        g_printerr("Unknown message '%s' from '%s', ignoring", msg, peer_id);
        g_object_unref(parser);
        return false;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_printerr ("Unknown json message '%s' from '%s', ignoring", msg, peer_id);
        g_object_unref(parser);
        return false;
    }

    g_print("Message from peer %s: %s\n", peer_id, msg);

    object = json_node_get_object(root);
    /* Check type of JSON message */
    if (json_object_has_member(object, "sdp")) {
        string text, sdp_type;

        g_assert_cmpint(app_state, >=, ROOM_JOINED);

        child = json_object_get_object_member (object, "sdp");

        if (!json_object_has_member (child, "type")) {
            cleanup_and_quit_loop ("ERROR: received SDP without 'type'", ROOM_CALL_ERROR);
            return false;
        }

        sdp_type = json_object_get_string_member(child, "type");
        text = json_object_get_string_member(child, "sdp");

        g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);
        handle_sdp_answer (peer_id, text);
        app_state = ROOM_CALL_STARTED;
    } else if (json_object_has_member (object, "ice")) {
        GstElement *webrtc;
        string candidate;
        gint sdpmlineindex;

        child = json_object_get_object_member (object, "ice");
        candidate = json_object_get_string_member (child, "candidate");
        sdpmlineindex = json_object_get_int_member (child, "sdpMLineIndex");

        /* Add ice candidate sent by remote peer */
        webrtc = gst_bin_get_by_name (GST_BIN (pipeline), peer_id.c_str());
        g_assert_nonnull (webrtc);
        g_signal_emit_by_name (webrtc, "add-ice-candidate", sdpmlineindex,
                candidate);
        gst_object_unref (webrtc);
    } else {
        g_printerr ("Ignoring unknown JSON message:\n%s\n", msg);
    }
    g_object_unref (parser);
    return true;
}


static void onClose(SoupWebsocketConnection* conn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    app_state = SERVER_CLOSED;
    cleanup_and_quit_loop("Server connection closed", APP_STATE_UNKNOWN);
}


static void onMessage(SoupWebsocketConnection* conn, SoupWebsocketDataType type, GBytes* message, gpointer user_data) {
    if(type == SOUP_WEBSOCKET_DATA_TEXT) {
        gsize size;
        string raw_data = (gchar*)g_bytes_get_data(message, &size);
        json data = json::parse(raw_data);

        if (commandsMapping.find(data["command"].get<string>()) != commandsMapping.end() ) {
            commandsMapping[data["command"].get<string>()](data);
        } else {
            cout << "Command not found: " << data << endl;
        }

        //if (data["command"]) {
            /*
            string text, *sdp_type;

            text = json_object_get_string_member(object, "command");

            } else if (g_str_has_prefix(text.c_str(), "CALL")) {
                const gchar* client_id = json_object_get_string_member(object, "identifier");
                g_print("Client %s is calling...\n", client_id);
                g_print("Negotiating with client %s\n", client_id);
                call_peer(client_id);
                //peers = g_list_prepend(peers, client_id);
            }
            */
        //}
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

    g_assert_nonnull (ws_conn);

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
    soup_session_add_feature(session, SOUP_SESSION_FEATURE (logger));
    g_object_unref(logger);

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
    for (auto need: needed) {
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


static GOptionEntry entries[] =
{
  { "local-id", 0, 0, G_OPTION_ARG_STRING, &local_id, "Camera identifier", "string" },
  { NULL },
};


int main(int argc, char *argv[]) {
    GOptionContext *context;
    GError *error = NULL;

    context = g_option_context_new ("- gstreamer webrtc sendrecv demo");
    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_add_group (context, gst_init_get_option_group ());
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("Error initializing: %s\n", error->message);
        return -1;
    }

    loop = g_main_loop_new(NULL, false);

    commandsMapping["JOINED_CAMERA"] = doRegistration;
    commandsMapping["UPDATE_CAMERAS"] = notMapped;
    commandsMapping["CALL"] = callPeer;

    if (!check_plugins ())
        return -1;
    connect();

    g_main_loop_run(loop);

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
    g_print("Pipeline stopped\n");

    gst_object_unref(pipeline);
    return 0;
}
