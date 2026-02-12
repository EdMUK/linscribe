#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <libsoup-3.0/libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <keybinder.h>
extern "C" {
#include <xdo.h>
}
#include <vector>
#include <cstdint>
#include <cmath>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <ctime>
#include <cstring>

static constexpr int SAMPLE_RATE = 44100;
static constexpr int NUM_CHANNELS = 1;
static constexpr int BITS_PER_SAMPLE = 16;
static constexpr double DECAY_FACTOR = 0.85;
static constexpr int WS_SAMPLE_RATE = 16000;

enum class TypingTool { NONE, XDO, WTYPE, YDOTOOL, XDOTOOL };

struct VoiceNote {
    std::string filepath;
    std::string display_name;
    double duration_seconds;
    std::string transcription;
    bool transcribing = false;
};

struct AppState {
    GtkWidget *window = nullptr;
    GtkWidget *label = nullptr;
    GtkWidget *record_button = nullptr;
    GtkWidget *level_bar = nullptr;

    // Save/Discard UI
    GtkWidget *save_discard_box = nullptr;
    GtkWidget *save_button = nullptr;
    GtkWidget *discard_button = nullptr;

    // Notes list UI
    GtkWidget *notes_list_box = nullptr;
    GtkWidget *notes_scroll = nullptr;

    // PulseAudio recording
    pa_glib_mainloop *pa_ml = nullptr;
    pa_context *pa_ctx = nullptr;
    pa_stream *stream = nullptr;

    // PulseAudio playback
    pa_stream *playback_stream = nullptr;
    bool playing = false;
    std::vector<int16_t> playback_buffer;
    size_t playback_offset = 0;
    int playing_note_index = -1;

    bool recording = false;
    bool pa_ready = false;
    double current_level = 0.0;

    std::vector<int16_t> audio_buffer;

    // Notes data
    std::vector<VoiceNote> notes;
    std::string data_dir;

    // Transcription service
    SoupSession *soup_session = nullptr;
    std::string api_key;
    bool transcription_available = false;

    // Real-time transcription (WebSocket)
    SoupWebsocketConnection *ws_conn = nullptr;
    bool ws_ready = false;
    std::string live_transcription;
    GtkWidget *live_transcription_label = nullptr;

    // Resampler state (44100→16000, preserved between PulseAudio chunks)
    double resample_phase = 0.0;

    // Dictation mode
    bool dictating = false;
    xdo_t *xdo = nullptr;
    TypingTool typing_tool = TypingTool::NONE;
    std::string dictation_buffer;
    guint dictation_flush_id = 0;
    AppIndicator *indicator = nullptr;
    std::string hotkey;
    GtkWidget *dictation_menu_item = nullptr;

    // Audio device selection
    std::vector<std::pair<std::string, std::string>> audio_sources;  // (pa_name, description)
    std::string audio_device;  // selected device pa_name, empty = default
};

// Forward declarations
static void stop_playback(AppState *state);
static void refresh_notes_list(AppState *state);
static void load_notes(AppState *state);
static void transcribe_note(AppState *state, int note_index);
static void ws_connect(AppState *state);
static void ws_disconnect(AppState *state);
static void ws_send_audio(AppState *state, const int16_t *samples, size_t count);
static void start_dictation(AppState *state);
static void stop_dictation(AppState *state);
static void update_dictation_menu_label(AppState *state);
static void type_text(AppState *state, const char *text);

struct TranscribeCallbackData {
    AppState *state;
    int note_index;
};

// --- Storage helpers ---

static void ensure_data_dir(AppState *state) {
    const char *data_base = g_get_user_data_dir();
    state->data_dir = std::string(data_base) + "/linscribe";
    std::filesystem::create_directories(state->data_dir);
}

static bool write_wav_file(const std::string &path,
                           const std::vector<int16_t> &samples) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;
    uint16_t block_align = NUM_CHANNELS * BITS_PER_SAMPLE / 8;
    uint32_t byte_rate = SAMPLE_RATE * block_align;

    // RIFF header
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char *>(&file_size), 4);
    out.write("WAVE", 4);

    // fmt chunk
    out.write("fmt ", 4);
    uint32_t fmt_size = 16;
    out.write(reinterpret_cast<const char *>(&fmt_size), 4);
    uint16_t audio_format = 1; // PCM
    out.write(reinterpret_cast<const char *>(&audio_format), 2);
    uint16_t channels = NUM_CHANNELS;
    out.write(reinterpret_cast<const char *>(&channels), 2);
    uint32_t sample_rate = SAMPLE_RATE;
    out.write(reinterpret_cast<const char *>(&sample_rate), 4);
    out.write(reinterpret_cast<const char *>(&byte_rate), 4);
    out.write(reinterpret_cast<const char *>(&block_align), 2);
    uint16_t bits = BITS_PER_SAMPLE;
    out.write(reinterpret_cast<const char *>(&bits), 2);

    // data chunk
    out.write("data", 4);
    out.write(reinterpret_cast<const char *>(&data_size), 4);
    out.write(reinterpret_cast<const char *>(samples.data()),
              static_cast<std::streamsize>(data_size));

    return out.good();
}

static bool read_wav_file(const std::string &path,
                          std::vector<int16_t> &samples) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    char riff[4], wave[4], fmt_id[4], data_id[4];
    uint32_t file_size, fmt_size, data_size;
    uint16_t audio_format, channels, bits_per_sample, block_align;
    uint32_t sample_rate, byte_rate;

    in.read(riff, 4);
    in.read(reinterpret_cast<char *>(&file_size), 4);
    in.read(wave, 4);

    if (std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(wave, "WAVE", 4) != 0)
        return false;

    in.read(fmt_id, 4);
    in.read(reinterpret_cast<char *>(&fmt_size), 4);
    if (std::memcmp(fmt_id, "fmt ", 4) != 0) return false;

    in.read(reinterpret_cast<char *>(&audio_format), 2);
    in.read(reinterpret_cast<char *>(&channels), 2);
    in.read(reinterpret_cast<char *>(&sample_rate), 4);
    in.read(reinterpret_cast<char *>(&byte_rate), 4);
    in.read(reinterpret_cast<char *>(&block_align), 2);
    in.read(reinterpret_cast<char *>(&bits_per_sample), 2);

    // Skip any extra fmt bytes
    if (fmt_size > 16) {
        in.seekg(static_cast<std::streamoff>(fmt_size - 16),
                 std::ios::cur);
    }

    in.read(data_id, 4);
    in.read(reinterpret_cast<char *>(&data_size), 4);
    if (std::memcmp(data_id, "data", 4) != 0) return false;

    size_t num_samples = data_size / sizeof(int16_t);
    samples.resize(num_samples);
    in.read(reinterpret_cast<char *>(samples.data()),
            static_cast<std::streamsize>(data_size));

    return in.good();
}

static double get_wav_duration(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0.0;

    // Skip to byte 24 for sample rate, then to byte 40 for data size
    char header[44];
    in.read(header, 44);
    if (!in.good()) return 0.0;

    uint32_t sample_rate;
    uint16_t channels, bits_per_sample;
    uint32_t data_size;

    std::memcpy(&sample_rate, header + 24, 4);
    std::memcpy(&channels, header + 22, 2);
    std::memcpy(&bits_per_sample, header + 34, 2);
    std::memcpy(&data_size, header + 40, 4);

    if (sample_rate == 0 || channels == 0 || bits_per_sample == 0)
        return 0.0;

    uint32_t bytes_per_sample = channels * bits_per_sample / 8;
    if (bytes_per_sample == 0) return 0.0;

    uint32_t total_samples = data_size / bytes_per_sample;
    return static_cast<double>(total_samples) / static_cast<double>(sample_rate);
}

static std::string generate_note_filename(const std::string &data_dir) {
    std::time_t now = std::time(nullptr);
    std::tm *tm = std::localtime(&now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "note_%Y-%m-%d_%H-%M-%S.wav", tm);
    return data_dir + "/" + buf;
}

static void load_notes(AppState *state) {
    state->notes.clear();

    if (!std::filesystem::exists(state->data_dir)) return;

    for (const auto &entry :
         std::filesystem::directory_iterator(state->data_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".wav") continue;

        VoiceNote note;
        note.filepath = entry.path().string();
        // Extract display name from filename: note_YYYY-MM-DD_HH-MM-SS.wav
        std::string stem = entry.path().stem().string();
        if (stem.rfind("note_", 0) == 0 && stem.size() >= 24) {
            // Convert note_YYYY-MM-DD_HH-MM-SS to YYYY-MM-DD HH:MM:SS
            std::string date_part = stem.substr(5, 10);       // YYYY-MM-DD
            std::string time_part = stem.substr(16, 8);       // HH-MM-SS
            // Replace dashes with colons in time
            for (auto &c : time_part) {
                if (c == '-') c = ':';
            }
            note.display_name = date_part + " " + time_part;
        } else {
            note.display_name = stem;
        }
        note.duration_seconds = get_wav_duration(note.filepath);

        // Load transcription from .txt sidecar if it exists
        std::filesystem::path txt_path = entry.path();
        txt_path.replace_extension(".txt");
        if (std::filesystem::exists(txt_path)) {
            std::ifstream txt_file(txt_path);
            if (txt_file) {
                note.transcription = std::string(
                    std::istreambuf_iterator<char>(txt_file),
                    std::istreambuf_iterator<char>());
            }
        }

        state->notes.push_back(std::move(note));
    }

    // Sort newest-first
    std::sort(state->notes.begin(), state->notes.end(),
              [](const VoiceNote &a, const VoiceNote &b) {
                  return a.filepath > b.filepath;
              });
}

// --- Audio helpers ---

static double calculate_peak_level(const int16_t *data, size_t num_samples) {
    int16_t peak = 0;
    for (size_t i = 0; i < num_samples; i++) {
        auto abs_val = static_cast<int16_t>(std::abs(data[i]));
        if (abs_val > peak) {
            peak = abs_val;
        }
    }
    return static_cast<double>(peak) / 32768.0;
}

// --- PulseAudio playback ---

static void on_playback_drain_complete(pa_stream * /*s*/, int /*success*/,
                                       void *userdata) {
    auto *state = static_cast<AppState *>(userdata);
    stop_playback(state);
}

static void on_playback_write(pa_stream *s, size_t nbytes, void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    if (state->playback_offset >= state->playback_buffer.size()) {
        pa_operation *op = pa_stream_drain(s, on_playback_drain_complete, state);
        if (op != nullptr) pa_operation_unref(op);
        return;
    }

    size_t remaining_samples =
        state->playback_buffer.size() - state->playback_offset;
    size_t max_samples = nbytes / sizeof(int16_t);
    size_t to_write = std::min(remaining_samples, max_samples);
    size_t bytes = to_write * sizeof(int16_t);

    pa_stream_write(s,
                    state->playback_buffer.data() + state->playback_offset,
                    bytes, nullptr, 0, PA_SEEK_RELATIVE);
    state->playback_offset += to_write;
}

static void on_playback_stream_state(pa_stream *s, void *userdata) {
    auto *state = static_cast<AppState *>(userdata);
    if (pa_stream_get_state(s) == PA_STREAM_FAILED) {
        g_warning("Playback stream failed: %s",
                  pa_strerror(pa_context_errno(state->pa_ctx)));
        stop_playback(state);
    }
}

static void start_playback(AppState *state, int note_index) {
    if (note_index < 0 ||
        note_index >= static_cast<int>(state->notes.size()))
        return;
    if (!state->pa_ready) return;

    // Stop any current playback
    if (state->playing) {
        stop_playback(state);
    }

    const VoiceNote &note = state->notes[static_cast<size_t>(note_index)];
    if (!read_wav_file(note.filepath, state->playback_buffer)) {
        gtk_label_set_text(GTK_LABEL(state->label), "Failed to read WAV file");
        return;
    }

    state->playback_offset = 0;

    static const pa_sample_spec spec = {
        .format = PA_SAMPLE_S16LE,
        .rate = SAMPLE_RATE,
        .channels = NUM_CHANNELS,
    };

    state->playback_stream =
        pa_stream_new(state->pa_ctx, "linscribe-playback", &spec, nullptr);
    if (state->playback_stream == nullptr) {
        gtk_label_set_text(GTK_LABEL(state->label),
                           "Failed to create playback stream");
        return;
    }

    pa_stream_set_write_callback(state->playback_stream,
                                 on_playback_write, state);
    pa_stream_set_state_callback(state->playback_stream,
                                 on_playback_stream_state, state);

    if (pa_stream_connect_playback(state->playback_stream, nullptr, nullptr,
                                   PA_STREAM_NOFLAGS, nullptr, nullptr) < 0) {
        gtk_label_set_text(GTK_LABEL(state->label),
                           "Failed to connect playback");
        pa_stream_unref(state->playback_stream);
        state->playback_stream = nullptr;
        return;
    }

    state->playing = true;
    state->playing_note_index = note_index;

    char buf[128];
    g_snprintf(buf, sizeof(buf), "Playing: %s", note.display_name.c_str());
    gtk_label_set_text(GTK_LABEL(state->label), buf);

    refresh_notes_list(state);
}

static void stop_playback(AppState *state) {
    if (state->playback_stream != nullptr) {
        pa_stream_disconnect(state->playback_stream);
        pa_stream_unref(state->playback_stream);
        state->playback_stream = nullptr;
    }

    bool was_playing = state->playing;
    state->playing = false;
    state->playing_note_index = -1;
    state->playback_buffer.clear();
    state->playback_offset = 0;

    if (was_playing) {
        gtk_label_set_text(GTK_LABEL(state->label), "Ready");
        refresh_notes_list(state);
    }
}

// --- Audio resampler (44100→16000) ---

static std::vector<int16_t> resample_44100_to_16000(const int16_t *input,
                                                     size_t count,
                                                     double *phase) {
    static constexpr double STEP = static_cast<double>(SAMPLE_RATE) /
                                   static_cast<double>(WS_SAMPLE_RATE);
    std::vector<int16_t> output;
    output.reserve(count / 2); // Rough estimate

    double pos = *phase;
    while (pos < static_cast<double>(count) - 1.0) {
        auto idx = static_cast<size_t>(pos);
        double frac = pos - static_cast<double>(idx);
        double sample = static_cast<double>(input[idx]) * (1.0 - frac) +
                        static_cast<double>(input[idx + 1]) * frac;
        output.push_back(static_cast<int16_t>(sample));
        pos += STEP;
    }

    *phase = pos - static_cast<double>(count);
    return output;
}

// --- Real-time transcription (WebSocket) ---

static void on_ws_message(SoupWebsocketConnection * /*conn*/, gint /*type*/,
                          GBytes *message, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    gsize len = 0;
    const char *data = static_cast<const char *>(
        g_bytes_get_data(message, &len));

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, static_cast<gssize>(len),
                                     nullptr)) {
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    const char *msg_type = json_object_get_string_member(obj, "type");

    if (g_strcmp0(msg_type, "session.created") == 0) {
        // Send session.update with audio format
        const char *session_update =
            "{\"type\":\"session.update\",\"session\":"
            "{\"audio_format\":{\"encoding\":\"pcm_s16le\","
            "\"sample_rate\":16000}}}";
        soup_websocket_connection_send_text(state->ws_conn, session_update);

    } else if (g_strcmp0(msg_type, "session.updated") == 0) {
        state->ws_ready = true;

    } else if (g_strcmp0(msg_type, "transcription.text.delta") == 0) {
        if (json_object_has_member(obj, "text")) {
            const char *text = json_object_get_string_member(obj, "text");
            if (text != nullptr) {
                if (state->dictating) {
                    type_text(state, text);
                } else {
                    state->live_transcription += text;
                    if (state->live_transcription_label != nullptr) {
                        gtk_label_set_text(
                            GTK_LABEL(state->live_transcription_label),
                            state->live_transcription.c_str());
                    }
                }
            }
        }

    } else if (g_strcmp0(msg_type, "error") == 0) {
        const char *detail = "";
        if (json_object_has_member(obj, "message")) {
            detail = json_object_get_string_member(obj, "message");
        }
        g_warning("WebSocket transcription error: %s", detail);
        ws_disconnect(state);
    }

    g_object_unref(parser);
}

static void on_ws_closed(SoupWebsocketConnection * /*conn*/,
                         gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);
    if (state->ws_conn != nullptr) {
        g_object_unref(state->ws_conn);
        state->ws_conn = nullptr;
    }
    state->ws_ready = false;

    // If dictating, stop gracefully
    if (state->dictating) {
        stop_dictation(state);
    }
}

static void on_ws_connect_complete(GObject *source, GAsyncResult *result,
                                   gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    GError *error = nullptr;
    SoupWebsocketConnection *conn =
        soup_session_websocket_connect_finish(SOUP_SESSION(source), result,
                                              &error);
    if (error != nullptr) {
        g_warning("WebSocket connect failed: %s", error->message);
        g_error_free(error);
        if (state->dictating) {
            stop_dictation(state);
        }
        return;
    }

    state->ws_conn = conn;
    g_signal_connect(conn, "message", G_CALLBACK(on_ws_message), state);
    g_signal_connect(conn, "closed", G_CALLBACK(on_ws_closed), state);
}

static void ws_connect(AppState *state) {
    if (!state->transcription_available) return;
    if (state->ws_conn != nullptr) return;

    state->resample_phase = 0.0;
    state->live_transcription.clear();
    state->ws_ready = false;

    SoupMessage *msg = soup_message_new(
        "GET",
        "wss://api.mistral.ai/v1/audio/transcriptions/realtime"
        "?model=voxtral-mini-transcribe-realtime-2602");

    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    std::string auth = "Bearer " + state->api_key;
    soup_message_headers_replace(headers, "Authorization", auth.c_str());

    soup_session_websocket_connect_async(state->soup_session, msg, nullptr,
                                         nullptr, G_PRIORITY_DEFAULT, nullptr,
                                         on_ws_connect_complete, state);
    g_object_unref(msg);
}

static void ws_disconnect(AppState *state) {
    state->ws_ready = false;
    if (state->ws_conn != nullptr &&
        soup_websocket_connection_get_state(state->ws_conn) ==
            SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_close(state->ws_conn,
                                        SOUP_WEBSOCKET_CLOSE_NORMAL, nullptr);
    }
}

static void ws_send_audio(AppState *state, const int16_t *samples,
                          size_t count) {
    if (!state->ws_ready || state->ws_conn == nullptr) return;

    std::vector<int16_t> resampled =
        resample_44100_to_16000(samples, count, &state->resample_phase);

    if (resampled.empty()) return;

    gchar *b64 = g_base64_encode(
        reinterpret_cast<const guchar *>(resampled.data()),
        resampled.size() * sizeof(int16_t));

    gchar *json = g_strdup_printf(
        "{\"type\":\"input_audio.append\",\"audio\":\"%s\"}", b64);

    soup_websocket_connection_send_text(state->ws_conn, json);

    g_free(json);
    g_free(b64);
}

// --- Recording stream callbacks ---

static void on_stream_read(pa_stream *s, size_t /*nbytes*/, void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    const void *data;
    size_t length;

    while (pa_stream_peek(s, &data, &length) >= 0 && length > 0) {
        if (data != nullptr) {
            auto num_samples = length / sizeof(int16_t);
            auto *samples = static_cast<const int16_t *>(data);
            state->audio_buffer.insert(state->audio_buffer.end(),
                                       samples, samples + num_samples);

            ws_send_audio(state, samples, num_samples);

            double peak = calculate_peak_level(samples, num_samples);
            if (peak >= state->current_level) {
                state->current_level = peak;
            } else {
                state->current_level = state->current_level * DECAY_FACTOR +
                                       peak * (1.0 - DECAY_FACTOR);
            }
            gtk_level_bar_set_value(GTK_LEVEL_BAR(state->level_bar),
                                    state->current_level);
        }
        pa_stream_drop(s);
    }
}

static void on_stream_state(pa_stream *s, void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    if (pa_stream_get_state(s) == PA_STREAM_FAILED) {
        g_warning("PulseAudio stream failed: %s",
                  pa_strerror(pa_context_errno(state->pa_ctx)));
        state->recording = false;
        gtk_button_set_label(GTK_BUTTON(state->record_button), "Record");
        gtk_label_set_text(GTK_LABEL(state->label), "Stream error");
        gtk_level_bar_set_value(GTK_LEVEL_BAR(state->level_bar), 0.0);
        state->current_level = 0.0;
    }
}

// --- Recording control ---

static void start_recording(AppState *state) {
    static const pa_sample_spec spec = {
        .format = PA_SAMPLE_S16LE,
        .rate = SAMPLE_RATE,
        .channels = NUM_CHANNELS,
    };

    state->stream = pa_stream_new(state->pa_ctx, "linscribe-record",
                                     &spec, nullptr);
    if (state->stream == nullptr) {
        gtk_label_set_text(GTK_LABEL(state->label), "Failed to create stream");
        return;
    }

    pa_stream_set_read_callback(state->stream, on_stream_read, state);
    pa_stream_set_state_callback(state->stream, on_stream_state, state);

    pa_buffer_attr attr = {};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = 4410 * sizeof(int16_t); // ~50ms at 44100 Hz mono S16LE

    const char *dev = state->audio_device.empty()
                          ? nullptr
                          : state->audio_device.c_str();
    if (pa_stream_connect_record(state->stream, dev, &attr,
                                 PA_STREAM_ADJUST_LATENCY) < 0) {
        gtk_label_set_text(GTK_LABEL(state->label), "Failed to connect stream");
        pa_stream_unref(state->stream);
        state->stream = nullptr;
        return;
    }

    state->audio_buffer.clear();
    state->current_level = 0.0;
    state->recording = true;
    gtk_button_set_label(GTK_BUTTON(state->record_button), "Stop");
    gtk_label_set_text(GTK_LABEL(state->label), "Recording...");

    // Start real-time transcription
    state->live_transcription.clear();
    state->resample_phase = 0.0;
    if (state->live_transcription_label != nullptr) {
        gtk_label_set_text(GTK_LABEL(state->live_transcription_label), "");
        gtk_widget_set_no_show_all(state->live_transcription_label, FALSE);
        gtk_widget_show(state->live_transcription_label);
    }
    ws_connect(state);
}

static void stop_recording(AppState *state) {
    if (state->stream != nullptr) {
        pa_stream_disconnect(state->stream);
        pa_stream_unref(state->stream);
        state->stream = nullptr;
    }

    ws_disconnect(state);

    state->recording = false;
    state->current_level = 0.0;
    gtk_level_bar_set_value(GTK_LEVEL_BAR(state->level_bar), 0.0);
    gtk_button_set_label(GTK_BUTTON(state->record_button), "Record");

    double seconds =
        static_cast<double>(state->audio_buffer.size()) / SAMPLE_RATE;
    char buf[64];
    g_snprintf(buf, sizeof(buf), "Recorded %.1f seconds - save?", seconds);
    gtk_label_set_text(GTK_LABEL(state->label), buf);

    // Show save/discard buttons
    gtk_widget_set_no_show_all(state->save_discard_box, FALSE);
    gtk_widget_show_all(state->save_discard_box);
}

// --- UI callbacks ---

static void on_save_clicked(GtkWidget * /*button*/, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    if (state->audio_buffer.empty()) {
        gtk_label_set_text(GTK_LABEL(state->label), "Nothing to save");
        gtk_widget_hide(state->save_discard_box);
        gtk_widget_set_no_show_all(state->save_discard_box, TRUE);
        return;
    }

    std::string path = generate_note_filename(state->data_dir);
    if (!write_wav_file(path, state->audio_buffer)) {
        gtk_label_set_text(GTK_LABEL(state->label), "Failed to save");
        return;
    }

    // Write live transcription as .txt sidecar
    if (!state->live_transcription.empty()) {
        std::filesystem::path txt_path(path);
        txt_path.replace_extension(".txt");
        std::ofstream txt_file(txt_path);
        if (txt_file) {
            txt_file << state->live_transcription;
        }
    }

    state->audio_buffer.clear();
    state->live_transcription.clear();
    if (state->live_transcription_label != nullptr) {
        gtk_widget_hide(state->live_transcription_label);
        gtk_widget_set_no_show_all(state->live_transcription_label, TRUE);
    }
    gtk_widget_hide(state->save_discard_box);
    gtk_widget_set_no_show_all(state->save_discard_box, TRUE);

    load_notes(state);
    refresh_notes_list(state);

    gtk_label_set_text(GTK_LABEL(state->label), "Note saved");
}

static void on_discard_clicked(GtkWidget * /*button*/, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    state->audio_buffer.clear();
    state->live_transcription.clear();
    if (state->live_transcription_label != nullptr) {
        gtk_widget_hide(state->live_transcription_label);
        gtk_widget_set_no_show_all(state->live_transcription_label, TRUE);
    }
    gtk_widget_hide(state->save_discard_box);
    gtk_widget_set_no_show_all(state->save_discard_box, TRUE);
    gtk_label_set_text(GTK_LABEL(state->label), "Recording discarded");
}

static void on_play_clicked(GtkWidget *button, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    if (state->recording) return;

    int note_index = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "note_index"));

    // Toggle: if already playing this note, stop
    if (state->playing && state->playing_note_index == note_index) {
        stop_playback(state);
        return;
    }

    start_playback(state, note_index);
}

static void on_delete_clicked(GtkWidget *button, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    int note_index = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "note_index"));

    if (note_index < 0 ||
        note_index >= static_cast<int>(state->notes.size()))
        return;

    // Stop playback if we're playing this or any note
    if (state->playing) {
        stop_playback(state);
    }

    const std::string &filepath =
        state->notes[static_cast<size_t>(note_index)].filepath;
    std::filesystem::remove(filepath);

    // Also delete the .txt sidecar if it exists
    std::filesystem::path txt_path(filepath);
    txt_path.replace_extension(".txt");
    std::filesystem::remove(txt_path);

    load_notes(state);
    refresh_notes_list(state);

    gtk_label_set_text(GTK_LABEL(state->label), "Note deleted");
}

// --- Transcription ---

static void on_transcribe_response(GObject *source, GAsyncResult *result,
                                   gpointer userdata) {
    auto *data = static_cast<TranscribeCallbackData *>(userdata);
    AppState *state = data->state;
    int note_index = data->note_index;
    delete data;

    GError *error = nullptr;
    GBytes *response_bytes = soup_session_send_and_read_finish(
        SOUP_SESSION(source), result, &error);

    // Validate note_index is still in range (notes may have been deleted)
    if (note_index < 0 ||
        note_index >= static_cast<int>(state->notes.size())) {
        if (response_bytes) g_bytes_unref(response_bytes);
        if (error) g_error_free(error);
        return;
    }

    VoiceNote &note = state->notes[static_cast<size_t>(note_index)];
    note.transcribing = false;

    if (error != nullptr) {
        gtk_label_set_text(GTK_LABEL(state->label), "Transcription failed: network error");
        g_warning("Transcription error: %s", error->message);
        g_error_free(error);
        refresh_notes_list(state);
        return;
    }

    // Check HTTP status from the message — retrieve from session's last message
    // We need to get the status code from the response bytes content
    gsize response_len = 0;
    const char *response_data =
        static_cast<const char *>(g_bytes_get_data(response_bytes, &response_len));

    // Parse JSON response
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, response_data,
                                     static_cast<gssize>(response_len), &error)) {
        gtk_label_set_text(GTK_LABEL(state->label), "Transcription failed: invalid response");
        g_warning("JSON parse error: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_bytes_unref(response_bytes);
        refresh_notes_list(state);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);

    // Check for error responses
    if (json_object_has_member(obj, "message")) {
        const char *err_msg = json_object_get_string_member(obj, "message");
        char status_buf[256];
        g_snprintf(status_buf, sizeof(status_buf), "Transcription failed: %s", err_msg);
        gtk_label_set_text(GTK_LABEL(state->label), status_buf);
        g_object_unref(parser);
        g_bytes_unref(response_bytes);
        refresh_notes_list(state);
        return;
    }

    if (!json_object_has_member(obj, "text")) {
        gtk_label_set_text(GTK_LABEL(state->label), "Transcription failed: no text in response");
        g_object_unref(parser);
        g_bytes_unref(response_bytes);
        refresh_notes_list(state);
        return;
    }

    const char *text = json_object_get_string_member(obj, "text");
    note.transcription = text;

    // Save transcription to .txt sidecar
    std::filesystem::path txt_path(note.filepath);
    txt_path.replace_extension(".txt");
    std::ofstream txt_file(txt_path);
    if (txt_file) {
        txt_file << note.transcription;
    }

    g_object_unref(parser);
    g_bytes_unref(response_bytes);

    gtk_label_set_text(GTK_LABEL(state->label), "Transcription complete");
    refresh_notes_list(state);
}

static void transcribe_note(AppState *state, int note_index) {
    if (note_index < 0 ||
        note_index >= static_cast<int>(state->notes.size()))
        return;

    VoiceNote &note = state->notes[static_cast<size_t>(note_index)];

    // Read WAV file into GBytes
    GError *error = nullptr;
    GMappedFile *mapped = g_mapped_file_new(note.filepath.c_str(), FALSE, &error);
    if (mapped == nullptr) {
        gtk_label_set_text(GTK_LABEL(state->label), "Failed to read audio file");
        if (error) {
            g_warning("File read error: %s", error->message);
            g_error_free(error);
        }
        note.transcribing = false;
        refresh_notes_list(state);
        return;
    }
    GBytes *file_bytes = g_mapped_file_get_bytes(mapped);
    g_mapped_file_unref(mapped);

    // Build multipart form data
    SoupMultipart *multipart = soup_multipart_new(SOUP_FORM_MIME_TYPE_MULTIPART);
    soup_multipart_append_form_string(multipart, "model", "voxtral-mini-latest");

    // Extract filename from path
    std::filesystem::path fp(note.filepath);
    std::string filename = fp.filename().string();
    soup_multipart_append_form_file(multipart, "file", filename.c_str(),
                                     "audio/wav", file_bytes);
    g_bytes_unref(file_bytes);

    // Create message from multipart
    SoupMessage *msg = soup_message_new_from_multipart(
        "https://api.mistral.ai/v1/audio/transcriptions", multipart);
    soup_multipart_free(multipart);

    // Set authorization header
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    std::string auth = "Bearer " + state->api_key;
    soup_message_headers_replace(headers, "Authorization", auth.c_str());

    // Prepare callback data
    auto *cb_data = new TranscribeCallbackData{state, note_index};

    // Send async request
    soup_session_send_and_read_async(state->soup_session, msg,
                                      G_PRIORITY_DEFAULT, nullptr,
                                      on_transcribe_response, cb_data);
    g_object_unref(msg);
}

static void on_copy_clicked(GtkWidget *button, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    int note_index = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "note_index"));

    if (note_index < 0 ||
        note_index >= static_cast<int>(state->notes.size()))
        return;

    const VoiceNote &note = state->notes[static_cast<size_t>(note_index)];
    if (!note.transcription.empty()) {
        GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, note.transcription.c_str(), -1);
        gtk_label_set_text(GTK_LABEL(state->label), "Transcription copied");
    }
}

static void on_transcribe_clicked(GtkWidget *button, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);

    int note_index = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "note_index"));

    if (note_index < 0 ||
        note_index >= static_cast<int>(state->notes.size()))
        return;

    VoiceNote &note = state->notes[static_cast<size_t>(note_index)];
    note.transcribing = true;
    refresh_notes_list(state);

    gtk_label_set_text(GTK_LABEL(state->label), "Transcribing...");
    transcribe_note(state, note_index);
}

static void refresh_notes_list(AppState *state) {
    // Remove all existing rows
    GList *children = gtk_container_get_children(
        GTK_CONTAINER(state->notes_list_box));
    for (GList *iter = children; iter != nullptr; iter = iter->next) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    for (int i = 0; i < static_cast<int>(state->notes.size()); i++) {
        const VoiceNote &note = state->notes[static_cast<size_t>(i)];

        // Outer vertical box for the row
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        // Top row: label + buttons
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        // Label: "YYYY-MM-DD HH:MM:SS (X.Xs)"
        char label_text[128];
        g_snprintf(label_text, sizeof(label_text), "%s (%.1fs)",
                   note.display_name.c_str(), note.duration_seconds);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

        // Transcribe button or spinner
        if (note.transcribing) {
            GtkWidget *spinner = gtk_spinner_new();
            gtk_spinner_start(GTK_SPINNER(spinner));
            gtk_box_pack_start(GTK_BOX(row_box), spinner, FALSE, FALSE, 0);
        } else if (state->transcription_available && note.transcription.empty()) {
            GtkWidget *transcribe_btn = gtk_button_new_with_label("Transcribe");
            g_object_set_data(G_OBJECT(transcribe_btn), "note_index",
                              GINT_TO_POINTER(i));
            g_signal_connect(transcribe_btn, "clicked",
                             G_CALLBACK(on_transcribe_clicked), state);
            gtk_box_pack_start(GTK_BOX(row_box), transcribe_btn, FALSE, FALSE, 0);
        }

        // Copy button (only when transcription exists)
        if (!note.transcription.empty()) {
            GtkWidget *copy_btn =
                gtk_button_new_from_icon_name("edit-copy", GTK_ICON_SIZE_BUTTON);
            g_object_set_data(G_OBJECT(copy_btn), "note_index",
                              GINT_TO_POINTER(i));
            g_signal_connect(copy_btn, "clicked",
                             G_CALLBACK(on_copy_clicked), state);
            gtk_box_pack_start(GTK_BOX(row_box), copy_btn, FALSE, FALSE, 0);
        }

        // Play button
        const char *play_icon =
            (state->playing && state->playing_note_index == i)
                ? "media-playback-stop"
                : "media-playback-start";
        GtkWidget *play_btn =
            gtk_button_new_from_icon_name(play_icon, GTK_ICON_SIZE_BUTTON);
        g_object_set_data(G_OBJECT(play_btn), "note_index",
                          GINT_TO_POINTER(i));
        g_signal_connect(play_btn, "clicked",
                         G_CALLBACK(on_play_clicked), state);
        gtk_box_pack_start(GTK_BOX(row_box), play_btn, FALSE, FALSE, 0);

        // Delete button
        GtkWidget *del_btn =
            gtk_button_new_from_icon_name("edit-delete", GTK_ICON_SIZE_BUTTON);
        g_object_set_data(G_OBJECT(del_btn), "note_index",
                          GINT_TO_POINTER(i));
        g_signal_connect(del_btn, "clicked",
                         G_CALLBACK(on_delete_clicked), state);
        gtk_box_pack_start(GTK_BOX(row_box), del_btn, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), row_box, FALSE, FALSE, 0);

        // Transcription text below the button row
        if (!note.transcription.empty()) {
            GtkWidget *trans_label = gtk_label_new(note.transcription.c_str());
            gtk_label_set_xalign(GTK_LABEL(trans_label), 0.0);
            gtk_label_set_line_wrap(GTK_LABEL(trans_label), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(trans_label), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(trans_label), 40);
            gtk_label_set_selectable(GTK_LABEL(trans_label), TRUE);
            gtk_widget_set_margin_start(trans_label, 4);
            gtk_box_pack_start(GTK_BOX(vbox), trans_label, FALSE, FALSE, 0);
        }

        gtk_list_box_insert(GTK_LIST_BOX(state->notes_list_box),
                            vbox, -1);
    }

    gtk_widget_show_all(state->notes_list_box);
}

static void on_record_toggled(GtkWidget * /*button*/, gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);
    if (state->recording) {
        stop_recording(state);
    } else {
        // Stop playback if active
        if (state->playing) {
            stop_playback(state);
        }
        // Hide save/discard if visible (implicit discard)
        if (gtk_widget_get_visible(state->save_discard_box)) {
            state->audio_buffer.clear();
            state->live_transcription.clear();
            if (state->live_transcription_label != nullptr) {
                gtk_widget_hide(state->live_transcription_label);
                gtk_widget_set_no_show_all(state->live_transcription_label, TRUE);
            }
            gtk_widget_hide(state->save_discard_box);
            gtk_widget_set_no_show_all(state->save_discard_box, TRUE);
        }
        start_recording(state);
    }
}

// --- PulseAudio context ---

static void on_source_info(pa_context * /*c*/, const pa_source_info *info,
                           int eol, void *userdata) {
    if (eol > 0) return;
    if (info == nullptr) return;
    auto *state = static_cast<AppState *>(userdata);
    state->audio_sources.emplace_back(info->name, info->description);
}

static void on_pa_context_state(pa_context *c, void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        state->pa_ready = true;
        state->audio_sources.clear();
        pa_operation_unref(
            pa_context_get_source_info_list(c, on_source_info, state));
        if (state->record_button != nullptr) {
            gtk_widget_set_sensitive(state->record_button, TRUE);
        }
        if (state->label != nullptr) {
            if (!state->transcription_available) {
                gtk_label_set_text(GTK_LABEL(state->label),
                                   "Ready — set API key in Settings for transcription");
            } else {
                gtk_label_set_text(GTK_LABEL(state->label), "Ready");
            }
        }
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        state->pa_ready = false;
        if (state->record_button != nullptr) {
            gtk_widget_set_sensitive(state->record_button, FALSE);
        }
        if (state->label != nullptr) {
            gtk_label_set_text(GTK_LABEL(state->label), "Audio unavailable");
        }
        g_warning("PulseAudio context failed: %s",
                  pa_strerror(pa_context_errno(c)));
        break;
    default:
        break;
    }
}

static void init_pulseaudio(AppState *state) {
    state->pa_ml = pa_glib_mainloop_new(g_main_context_default());
    pa_mainloop_api *api = pa_glib_mainloop_get_api(state->pa_ml);

    state->pa_ctx = pa_context_new(api, "linscribe");
    pa_context_set_state_callback(state->pa_ctx, on_pa_context_state, state);
    pa_context_connect(state->pa_ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
}

static void cleanup_pulseaudio(AppState *state) {
    // Clean up playback stream
    if (state->playback_stream != nullptr) {
        pa_stream_disconnect(state->playback_stream);
        pa_stream_unref(state->playback_stream);
        state->playback_stream = nullptr;
    }
    // Clean up recording stream
    if (state->stream != nullptr) {
        pa_stream_disconnect(state->stream);
        pa_stream_unref(state->stream);
        state->stream = nullptr;
    }
    if (state->pa_ctx != nullptr) {
        pa_context_disconnect(state->pa_ctx);
        pa_context_unref(state->pa_ctx);
        state->pa_ctx = nullptr;
    }
    if (state->pa_ml != nullptr) {
        pa_glib_mainloop_free(state->pa_ml);
        state->pa_ml = nullptr;
    }
}

// --- Transcription service ---

static std::string get_api_key_path(AppState *state) {
    return state->data_dir + "/mistral_api_key";
}

static std::string load_saved_api_key(AppState *state) {
    std::ifstream in(get_api_key_path(state));
    if (!in) return "";
    std::string key;
    std::getline(in, key);
    // Trim whitespace
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r' ||
                            key.back() == ' '))
        key.pop_back();
    return key;
}

static void save_api_key(AppState *state, const std::string &key) {
    std::ofstream out(get_api_key_path(state));
    if (out) {
        out << key;
    }
}

static std::string get_hotkey_path(AppState *state) {
    return state->data_dir + "/dictation_hotkey";
}

static std::string load_saved_hotkey(AppState *state) {
    std::ifstream in(get_hotkey_path(state));
    if (!in) return "<Ctrl><Shift>space";
    std::string key;
    std::getline(in, key);
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r' ||
                            key.back() == ' '))
        key.pop_back();
    return key.empty() ? "<Ctrl><Shift>space" : key;
}

static void save_hotkey(AppState *state, const std::string &key) {
    std::ofstream out(get_hotkey_path(state));
    if (out) {
        out << key;
    }
}

static std::string get_audio_device_path(AppState *state) {
    return state->data_dir + "/audio_device";
}

static std::string load_saved_audio_device(AppState *state) {
    std::ifstream in(get_audio_device_path(state));
    if (!in) return "";
    std::string dev;
    std::getline(in, dev);
    while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r' ||
                            dev.back() == ' '))
        dev.pop_back();
    return dev;
}

static void save_audio_device(AppState *state, const std::string &dev) {
    std::ofstream out(get_audio_device_path(state));
    if (out) {
        out << dev;
    }
}

static void init_transcription_service(AppState *state) {
    // Check saved key first, then fall back to environment variable
    std::string key = load_saved_api_key(state);
    if (key.empty()) {
        const char *env_key = g_getenv("MISTRAL_API_KEY");
        if (env_key != nullptr && env_key[0] != '\0') {
            key = env_key;
        }
    }

    if (key.empty()) {
        state->transcription_available = false;
        return;
    }

    state->api_key = key;
    if (state->soup_session == nullptr) {
        state->soup_session = soup_session_new();
        soup_session_set_timeout(state->soup_session, 120);
    }
    state->transcription_available = true;
}

static void cleanup_transcription_service(AppState *state) {
    ws_disconnect(state);
    if (state->soup_session != nullptr) {
        g_object_unref(state->soup_session);
        state->soup_session = nullptr;
    }
}

// --- Dictation mode ---

static bool is_wayland_session() {
    const char *type = g_getenv("XDG_SESSION_TYPE");
    return type != nullptr && g_strcmp0(type, "wayland") == 0;
}

// Detect which typing tool works on this system (called once at dictation start)
static TypingTool detect_typing_tool() {
    if (!is_wayland_session()) return TypingTool::XDO;

    // On Wayland, test each tool with an empty string to check if it runs
    GError *error = nullptr;
    gint exit_status = 0;

    // Try wtype (wlroots compositors only — fails on GNOME)
    if (g_spawn_sync(nullptr,
                     (gchar *[]){(gchar *)"wtype", (gchar *)"", nullptr},
                     nullptr,
                     static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                         G_SPAWN_STDOUT_TO_DEV_NULL |
                         G_SPAWN_STDERR_TO_DEV_NULL),
                     nullptr, nullptr, nullptr, nullptr, &exit_status,
                     &error)) {
        if (g_spawn_check_wait_status(exit_status, nullptr)) {
            g_message("Dictation will use wtype");
            return TypingTool::WTYPE;
        }
    }
    g_clear_error(&error);

    // Try ydotool (works on all Wayland compositors via uinput)
    if (g_spawn_sync(nullptr,
                     (gchar *[]){(gchar *)"ydotool", (gchar *)"type",
                                 (gchar *)"", nullptr},
                     nullptr,
                     static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                         G_SPAWN_STDOUT_TO_DEV_NULL |
                         G_SPAWN_STDERR_TO_DEV_NULL),
                     nullptr, nullptr, nullptr, nullptr, &exit_status,
                     &error)) {
        if (g_spawn_check_wait_status(exit_status, nullptr)) {
            g_message("Dictation will use ydotool");
            return TypingTool::YDOTOOL;
        }
    }
    g_clear_error(&error);

    // Try xdotool (X11/XWayland windows only)
    if (g_spawn_sync(nullptr,
                     (gchar *[]){(gchar *)"xdotool", (gchar *)"type",
                                 (gchar *)"", nullptr},
                     nullptr,
                     static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                         G_SPAWN_STDOUT_TO_DEV_NULL |
                         G_SPAWN_STDERR_TO_DEV_NULL),
                     nullptr, nullptr, nullptr, nullptr, &exit_status,
                     &error)) {
        if (g_spawn_check_wait_status(exit_status, nullptr)) {
            g_message("Dictation will use xdotool");
            return TypingTool::XDOTOOL;
        }
    }
    g_clear_error(&error);

    g_warning("No working typing tool found (need wtype, ydotool, or xdotool)");
    return TypingTool::NONE;
}

// Flush accumulated dictation text in one batch (runs as GLib idle callback)
static gboolean flush_dictation_buffer(gpointer userdata) {
    auto *state = static_cast<AppState *>(userdata);
    state->dictation_flush_id = 0;

    if (state->dictation_buffer.empty() || !state->dictating)
        return G_SOURCE_REMOVE;

    std::string text = std::move(state->dictation_buffer);
    state->dictation_buffer.clear();

    static constexpr auto SPAWN_FLAGS = static_cast<GSpawnFlags>(
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
        G_SPAWN_STDERR_TO_DEV_NULL);

    switch (state->typing_tool) {
    case TypingTool::XDO:
        if (state->xdo != nullptr) {
            xdo_enter_text_window(state->xdo, CURRENTWINDOW, text.c_str(),
                                   12000);
        }
        break;
    case TypingTool::WTYPE: {
        gchar *argv[] = {
            (gchar *)"wtype", (gchar *)"--", (gchar *)text.c_str(), nullptr};
        g_spawn_sync(nullptr, argv, nullptr, SPAWN_FLAGS, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr);
        break;
    }
    case TypingTool::YDOTOOL: {
        gchar *argv[] = {
            (gchar *)"ydotool", (gchar *)"type", (gchar *)"--",
            (gchar *)text.c_str(), nullptr};
        g_spawn_sync(nullptr, argv, nullptr, SPAWN_FLAGS, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr);
        break;
    }
    case TypingTool::XDOTOOL: {
        gchar *argv[] = {
            (gchar *)"xdotool", (gchar *)"type", (gchar *)"--clearmodifiers",
            (gchar *)"--", (gchar *)text.c_str(), nullptr};
        g_spawn_sync(nullptr, argv, nullptr, SPAWN_FLAGS, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr);
        break;
    }
    case TypingTool::NONE:
        break;
    }

    return G_SOURCE_REMOVE;
}

// Queue text for typing — batches rapid deltas, flushes on next idle
static void type_text(AppState *state, const char *text) {
    if (text == nullptr || text[0] == '\0') return;

    state->dictation_buffer += text;

    if (state->dictation_flush_id == 0) {
        state->dictation_flush_id =
            g_idle_add(flush_dictation_buffer, state);
    }
}

static void on_dictation_stream_read(pa_stream *s, size_t /*nbytes*/,
                                      void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    const void *data;
    size_t length;

    while (pa_stream_peek(s, &data, &length) >= 0 && length > 0) {
        if (data != nullptr) {
            auto num_samples = length / sizeof(int16_t);
            auto *samples = static_cast<const int16_t *>(data);
            ws_send_audio(state, samples, num_samples);
        }
        pa_stream_drop(s);
    }
}

static void on_dictation_stream_state(pa_stream *s, void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    if (pa_stream_get_state(s) == PA_STREAM_FAILED) {
        g_warning("Dictation PulseAudio stream failed: %s",
                  pa_strerror(pa_context_errno(state->pa_ctx)));
        stop_dictation(state);
    }
}

static void start_dictation(AppState *state) {
    if (!state->transcription_available || state->dictating) return;
    if (!state->pa_ready) return;
    if (state->recording) return;  // voice note recording in progress

    // Detect which typing tool to use (once per dictation session)
    state->typing_tool = detect_typing_tool();
    if (state->typing_tool == TypingTool::NONE) {
        g_warning("No working typing tool found — cannot start dictation");
        return;
    }

    if (state->typing_tool == TypingTool::XDO) {
        state->xdo = xdo_new(nullptr);
        if (state->xdo == nullptr) {
            g_warning("Failed to create xdo handle");
            state->typing_tool = TypingTool::NONE;
            return;
        }
    }

    state->dictating = true;

    // Change tray icon to indicate dictation
    if (state->indicator != nullptr) {
        app_indicator_set_icon_full(state->indicator, "media-record",
                                     "Dictating");
    }

    // Create PA stream for dictation
    static const pa_sample_spec spec = {
        .format = PA_SAMPLE_S16LE,
        .rate = SAMPLE_RATE,
        .channels = NUM_CHANNELS,
    };

    state->stream = pa_stream_new(state->pa_ctx, "linscribe-dictation",
                                   &spec, nullptr);
    if (state->stream == nullptr) {
        g_warning("Failed to create dictation stream");
        stop_dictation(state);
        return;
    }

    pa_stream_set_read_callback(state->stream, on_dictation_stream_read, state);
    pa_stream_set_state_callback(state->stream, on_dictation_stream_state,
                                  state);

    pa_buffer_attr attr = {};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = 4410 * sizeof(int16_t);

    const char *dev = state->audio_device.empty()
                          ? nullptr
                          : state->audio_device.c_str();
    if (pa_stream_connect_record(state->stream, dev, &attr,
                                  PA_STREAM_ADJUST_LATENCY) < 0) {
        g_warning("Failed to connect dictation stream");
        pa_stream_unref(state->stream);
        state->stream = nullptr;
        stop_dictation(state);
        return;
    }

    // Start WebSocket transcription
    state->live_transcription.clear();
    state->resample_phase = 0.0;
    ws_connect(state);

    update_dictation_menu_label(state);
}

static void stop_dictation(AppState *state) {
    if (!state->dictating) return;

    state->dictating = false;

    // Cancel pending flush and clear buffer
    if (state->dictation_flush_id != 0) {
        g_source_remove(state->dictation_flush_id);
        state->dictation_flush_id = 0;
    }
    state->dictation_buffer.clear();

    // Stop PA stream
    if (state->stream != nullptr) {
        pa_stream_disconnect(state->stream);
        pa_stream_unref(state->stream);
        state->stream = nullptr;
    }

    // Disconnect WebSocket
    ws_disconnect(state);

    // Restore tray icon
    if (state->indicator != nullptr) {
        app_indicator_set_icon_full(state->indicator,
                                     "accessories-text-editor", "Linscribe");
    }

    // Free xdo
    if (state->xdo != nullptr) {
        xdo_free(state->xdo);
        state->xdo = nullptr;
    }

    update_dictation_menu_label(state);
}

static void on_hotkey_pressed(const char * /*keystring*/, void *user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (state->dictating) {
        stop_dictation(state);
    } else {
        start_dictation(state);
    }
}

static void update_dictation_menu_label(AppState *state) {
    if (state->dictation_menu_item == nullptr) return;
    gtk_menu_item_set_label(GTK_MENU_ITEM(state->dictation_menu_item),
                            state->dictating ? "Stop Speaking"
                                             : "Speak To Type");
}

// --- Tray menu ---

static void on_menu_transcribe(GtkMenuItem * /*item*/, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    gtk_widget_show_all(state->window);
    gtk_window_present(GTK_WINDOW(state->window));
}

static void on_menu_dictation(GtkMenuItem * /*item*/, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (state->dictating) {
        stop_dictation(state);
    } else {
        start_dictation(state);
    }
}

static void on_menu_settings(GtkMenuItem * /*item*/, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Settings", GTK_WINDOW(state->window),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                     GTK_DIALOG_DESTROY_WITH_PARENT),
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save", GTK_RESPONSE_ACCEPT,
        nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    // Audio device dropdown
    GtkWidget *device_label = gtk_label_new("Audio Device:");
    gtk_label_set_xalign(GTK_LABEL(device_label), 0.0);
    gtk_box_pack_start(GTK_BOX(content), device_label, FALSE, FALSE, 0);

    GtkWidget *device_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(device_combo), "", "Default");
    int active_index = 0;
    for (int i = 0; i < static_cast<int>(state->audio_sources.size()); i++) {
        const auto &src = state->audio_sources[static_cast<size_t>(i)];
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(device_combo),
                                   src.first.c_str(), src.second.c_str());
        if (src.first == state->audio_device) {
            active_index = i + 1;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(device_combo), active_index);
    gtk_box_pack_start(GTK_BOX(content), device_combo, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new("Mistral API Key:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                    "Leave blank to use MISTRAL_API_KEY env var");
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(entry), '*');

    // Pre-fill with saved key (not env var)
    std::string saved = load_saved_api_key(state);
    if (!saved.empty()) {
        gtk_entry_set_text(GTK_ENTRY(entry), saved.c_str());
    }

    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);

    // Hotkey field
    GtkWidget *hotkey_label = gtk_label_new("Dictation Hotkey:");
    gtk_label_set_xalign(GTK_LABEL(hotkey_label), 0.0);
    gtk_box_pack_start(GTK_BOX(content), hotkey_label, FALSE, FALSE, 0);

    GtkWidget *hotkey_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(hotkey_entry),
                                    "<Ctrl><Shift>space");
    gtk_entry_set_text(GTK_ENTRY(hotkey_entry), state->hotkey.c_str());
    gtk_box_pack_start(GTK_BOX(content), hotkey_entry, FALSE, FALSE, 0);

    gtk_widget_show_all(content);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        // Save audio device selection
        const gchar *device_id =
            gtk_combo_box_get_active_id(GTK_COMBO_BOX(device_combo));
        std::string new_device(device_id ? device_id : "");
        state->audio_device = new_device;
        save_audio_device(state, new_device);

        const char *new_key = gtk_entry_get_text(GTK_ENTRY(entry));
        std::string key_str(new_key ? new_key : "");

        save_api_key(state, key_str);

        // Reinitialize transcription service with new key
        init_transcription_service(state);

        if (!state->transcription_available) {
            gtk_label_set_text(GTK_LABEL(state->label),
                               "Ready — set API key in Settings for transcription");
        } else {
            gtk_label_set_text(GTK_LABEL(state->label), "Ready");
        }
        refresh_notes_list(state);

        // Update hotkey binding — unbind old, save new, rebind if available
        const char *new_hotkey = gtk_entry_get_text(GTK_ENTRY(hotkey_entry));
        std::string hotkey_str(new_hotkey ? new_hotkey : "");
        if (!is_wayland_session()) {
            if (!state->hotkey.empty()) {
                keybinder_unbind_all(state->hotkey.c_str());
            }
        }
        if (!hotkey_str.empty()) {
            state->hotkey = hotkey_str;
            save_hotkey(state, hotkey_str);
        }
        if (!is_wayland_session() && state->transcription_available &&
            !state->hotkey.empty()) {
            keybinder_bind(state->hotkey.c_str(), on_hotkey_pressed, state);
        }

        // Update dictation menu visibility based on transcription availability
        if (state->dictation_menu_item != nullptr) {
            if (state->transcription_available) {
                gtk_widget_set_no_show_all(state->dictation_menu_item, FALSE);
                gtk_widget_show(state->dictation_menu_item);
            } else {
                gtk_widget_hide(state->dictation_menu_item);
                gtk_widget_set_no_show_all(state->dictation_menu_item, TRUE);
            }
        }
    }

    gtk_widget_destroy(dialog);
}

static void on_menu_quit(GtkMenuItem * /*item*/, gpointer /*user_data*/) {
    g_application_quit(g_application_get_default());
}

// --- Application activation ---

static void activate(GApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);

    // If the window already exists, just present it
    if (state->window != nullptr) {
        gtk_widget_show_all(state->window);
        gtk_window_present(GTK_WINDOW(state->window));
        return;
    }

    // Initialize data directory and load existing notes
    ensure_data_dir(state);
    state->audio_device = load_saved_audio_device(state);
    load_notes(state);

    // Initialize transcription service
    init_transcription_service(state);

    // Create window
    state->window = gtk_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(state->window), "Linscribe");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 640, 550);

    // Hide on close instead of destroying
    g_signal_connect(state->window, "delete-event",
                     G_CALLBACK(gtk_widget_hide_on_delete), nullptr);

    // Layout
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_container_add(GTK_CONTAINER(state->window), box);

    // Record button (starts disabled until PulseAudio is ready)
    state->record_button = gtk_button_new_with_label("Record");
    gtk_widget_set_sensitive(state->record_button, FALSE);
    gtk_box_pack_start(GTK_BOX(box), state->record_button, FALSE, FALSE, 0);
    g_signal_connect(state->record_button, "clicked",
                     G_CALLBACK(on_record_toggled), state);

    // Level bar
    state->level_bar = gtk_level_bar_new_for_interval(0.0, 1.0);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(state->level_bar),
                           GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(state->level_bar),
                                      GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(state->level_bar),
                                      GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(state->level_bar),
                                      GTK_LEVEL_BAR_OFFSET_FULL);
    gtk_box_pack_start(GTK_BOX(box), state->level_bar, FALSE, FALSE, 0);

    // Live transcription label (hidden by default, shown during recording)
    state->live_transcription_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->live_transcription_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(state->live_transcription_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(state->live_transcription_label),
                                 PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(state->live_transcription_label), 40);
    gtk_label_set_selectable(GTK_LABEL(state->live_transcription_label), TRUE);
    gtk_widget_set_no_show_all(state->live_transcription_label, TRUE);
    gtk_box_pack_start(GTK_BOX(box), state->live_transcription_label,
                       FALSE, FALSE, 0);

    // Save/Discard button row (hidden by default)
    state->save_discard_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    state->save_button = gtk_button_new_with_label("Save");
    state->discard_button = gtk_button_new_with_label("Discard");
    gtk_box_pack_start(GTK_BOX(state->save_discard_box),
                       state->save_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(state->save_discard_box),
                       state->discard_button, TRUE, TRUE, 0);
    g_signal_connect(state->save_button, "clicked",
                     G_CALLBACK(on_save_clicked), state);
    g_signal_connect(state->discard_button, "clicked",
                     G_CALLBACK(on_discard_clicked), state);
    gtk_widget_set_no_show_all(state->save_discard_box, TRUE);
    gtk_box_pack_start(GTK_BOX(box), state->save_discard_box,
                       FALSE, FALSE, 0);

    // Status label
    state->label = gtk_label_new("Connecting to audio...");
    gtk_box_pack_start(GTK_BOX(box), state->label, FALSE, FALSE, 0);

    // Separator
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 0);

    // Scrolled window with notes list
    state->notes_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(state->notes_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(state->notes_scroll, -1, 200);
    gtk_box_pack_start(GTK_BOX(box), state->notes_scroll, TRUE, TRUE, 0);

    state->notes_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->notes_list_box),
                                    GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(state->notes_scroll),
                      state->notes_list_box);

    // Populate notes list
    refresh_notes_list(state);

    // Tray icon menu
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *transcribe_item = gtk_menu_item_new_with_label("Transcribe");
    g_signal_connect(transcribe_item, "activate",
                     G_CALLBACK(on_menu_transcribe), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), transcribe_item);

    // Dictation menu item (hidden if transcription not available)
    state->dictation_menu_item =
        gtk_menu_item_new_with_label("Speak To Type");
    g_signal_connect(state->dictation_menu_item, "activate",
                     G_CALLBACK(on_menu_dictation), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), state->dictation_menu_item);
    if (!state->transcription_available) {
        gtk_widget_set_no_show_all(state->dictation_menu_item, TRUE);
    }

    GtkWidget *settings_item = gtk_menu_item_new_with_label("Settings");
    g_signal_connect(settings_item, "activate",
                     G_CALLBACK(on_menu_settings), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings_item);

    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_menu_quit), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

    // Tray icon (stored in AppState for dictation icon changes)
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    state->indicator = app_indicator_new(
        "linscribe", "accessories-text-editor",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    G_GNUC_END_IGNORE_DEPRECATIONS
    app_indicator_set_status(state->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(state->indicator, GTK_MENU(menu));

    // Initialize keybinder for global hotkey (X11 only — Wayland blocks
    // X11 key grabs, so users must use the tray menu on Wayland)
    state->hotkey = load_saved_hotkey(state);
    if (!is_wayland_session()) {
        keybinder_init();
        if (state->transcription_available && !state->hotkey.empty()) {
            if (!keybinder_bind(state->hotkey.c_str(), on_hotkey_pressed,
                                state)) {
                g_warning("Failed to bind hotkey '%s'",
                          state->hotkey.c_str());
            }
        }
    } else {
        g_message("Wayland session — global hotkey unavailable, "
                  "use tray menu for dictation");
    }

    // Connect to PulseAudio (once)
    if (state->pa_ctx == nullptr) {
        init_pulseaudio(state);
    }
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new(
        "com.edmorley.linscribe", G_APPLICATION_DEFAULT_FLAGS);

    // Keep the app alive even when the window is hidden
    g_application_hold(G_APPLICATION(app));

    AppState state{};
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    // Cleanup dictation
    if (state.dictating) {
        stop_dictation(&state);
    }
    if (!is_wayland_session() && !state.hotkey.empty()) {
        keybinder_unbind_all(state.hotkey.c_str());
    }
    if (state.xdo != nullptr) {
        xdo_free(state.xdo);
        state.xdo = nullptr;
    }

    cleanup_transcription_service(&state);
    cleanup_pulseaudio(&state);
    g_object_unref(app);
    return status;
}
