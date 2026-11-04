#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include "field_prep.h"
#include "video_mode.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace groovy {

// Groovy UDP client: session handling, modelines, blits, audio and ACK tracking.
// Fields go out one at a time, numbered from the raster at send time.
class Sender {
public:
    using clock = std::chrono::steady_clock;

    Sender() = default;
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    ~Sender() { close(); }

    // `clock_scale` multiplies the mode's nominal pixel clock.
    bool open(const std::string& ip, VideoMode mode, uint32_t audio_rate_hz, double clock_scale);

    // Callers must have stopped every sending thread first, and CLOSE ends the
    // session, so it goes out exactly once and only after an INIT ACK.
    void close();

    // Unblocks a pending INIT recv(), so closing doesn't wait for its timeout.
    void interrupt();

    bool set_mode(VideoMode mode, double clock_scale);
    // Takes effect at the next INIT, such as set_mode().
    void set_audio_rate(uint32_t hz) { audio_rate_hz_ = hz == 44100 || hz == 48000 ? hz : 0; }

    // Blit rate of a mode at its nominal pixel clock.
    static double nominal_mode_rate(VideoMode mode);

    // Sets the current mode's pixel clock to nominal * `scale`, without INIT. Groovy
    // applies it at a vblank once the last field sent has started scanning, and
    // holds the raster there until the next field starts inflating. A field sent
    // before that vblank therefore leaves the raster running.
    bool retune(double scale);

    // The GroovyGrab core can trim the pixel rate with no reset or visible step, which
    // replaces retune() once a stream is running. Known after connecting.
    bool has_trim() const { return has_trim_; }
    // Runs the raster this many ppm fast (negative: slow). INIT clears it.
    bool set_trim(double ppm);

    bool send_prepared(const PreparedField& field, double& elapsed_ms);
    bool send_audio(const uint8_t* pcm, uint16_t bytes);
    uint16_t pcm_bytes_per_chunk() const;
    // The source parity the last blit carried.
    uint8_t last_parity() const { return last_parity_; }

    VideoMode mode() const { return mode_; }
    // Why the last call failed; empty if it did not.
    const std::string& error() const { return error_; }
    const std::string& error_detail() const { return error_detail_; } // for the console
    // Messages since the last call. Printing would stall the thread that is sending.
    std::string take_log();
    uint16_t v_total() const { return v_total_; }
    double pixel_clock_mhz() const { return pixel_clock_mhz_; }

    // One blit period: a frame, or a field in interlaced modes (Groovy's frame
    // counter and vcount/v_total position are per field there).
    std::chrono::nanoseconds frame_duration() const;
    double field_ms() const;

    // Blit rate the current modeline should produce. The real rate differs only
    // by MiSTer's crystal error.
    double nominal_rate() const;

    // Raster position (fraction of a field) where Groovy parks after SWITCHRES
    // and restarts on the next blit: line V+1, V+2 if interlaced.
    double release_fraction() const;

    // When to send so the frame after the last one sent is not left empty. `early`
    // leaves more margin, for a frame with no sample to wait for.
    clock::time_point next_frame_deadline(bool early) const;

    // Next time the beam reaches the usual send point: release_fraction minus
    // `margin_seconds`. A frame that missed this field's land goes out here.
    clock::time_point next_land(double margin_seconds) const;

    // Raster position (blit periods, fractional) at the most recent ACK for one
    // of our fields, and when that ACK arrived.
    bool raster_sample(double& position, clock::time_point& at) const;

    // Timings of the last send_prepared(), for the latency probe.
    struct FieldTiming {
        double convert_ms = 0.0, lz4_ms = 0.0, stream_ms = 0.0, sleep_ms = 0.0;
        unsigned bytes = 0;
        clock::time_point sent_at{};
        bool acked = false; // ACK for this field arrived within the send call
        clock::time_point ack_at{};
        int lead = 0;       // frame number - gpu_frame when that ACK arrived
        uint16_t vcount = 0;
    };
    const FieldTiming& last_timing() const { return timing_; }

private:
    struct AckStatus {
        uint32_t echo = 0;
        uint32_t gpu_frame = 0;
        uint16_t vcount = 0;
        bool vga_f1 = false;
        bool vram_synced = true;
        bool vram_queued = true;
        bool frame_skip = false;
    };

    bool handshake(VideoMode mode);
    bool await_reply(std::array<uint8_t, 64>& reply, int length, std::chrono::milliseconds limit);
    bool reinit(VideoMode mode);
    bool restart_session();
    bool apply_mode(VideoMode mode);
    bool transmit_field(const char* data, int compressed_size, uint8_t udp_field,
                        clock::time_point started, double& elapsed_ms);
    bool send_packet(const uint8_t* data, size_t size);
    bool send_chunks(const uint8_t* data, size_t size);
    bool out_of_buffers();
    void send_close();
    void set_recv_timeout(DWORD timeout_ms);
    void parse_ack(const uint8_t* reply, int length);
    void poll_ack();
    void note_ack();
    void wait_ack(std::chrono::milliseconds limit);
    void advance_frame();
    bool mailbox_lost();
    void reset_mailbox_watch();
    double raster_at_time(clock::time_point t) const;
    double frame_seconds() const;
    bool fail(const char* detail, const char* message = "Can't reach MiSTer. Check the address and network");
    bool give_up(const char* detail, const char* message);

    WSADATA wsa_{};
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in endpoint_{};
    bool wsa_started_ = false;
    bool timer_period_set_ = false;
    std::atomic<bool> interrupted_{false};
    std::mutex socket_mutex_;   // interrupt() comes from another thread
    bool session_open_ = false; // Groovy ACKed our INIT: CLOSE is owed
    bool has_trim_ = false;
    double trim_ppm_ = 0.0;     // what the core is trimmed by
    std::mutex wire_mutex_;     // video and audio threads share the socket
    std::string error_;
    std::string error_detail_;
    std::string log_;
    std::atomic<uint32_t> full_buffer_sends_{0}; // video and audio threads both count these

    VideoMode mode_ = VideoMode::i480;
    uint32_t audio_rate_hz_ = 0;
    double clock_scale_ = 1.0;
    double pixel_clock_mhz_ = 0.0;
    uint16_t h_total_ = 0;
    uint16_t v_total_ = 525;
    uint32_t line_time_ns_ = 63'556;

    uint32_t frame_ = 0;
    AckStatus ack_{};
    uint32_t field_reset_frame_ = 0; // first frame after a retune restarts the field
    uint8_t last_parity_ = 0;
    FieldTiming timing_;
    clock::time_point last_blit_at_{};

    bool raster_valid_ = false;
    double raster_position_ = 0.0;
    clock::time_point raster_at_{};
    clock::time_point raster_epoch_{};
    std::array<double, 60> raster_offsets_{}; // ~1 s of ACK phase offsets
    size_t raster_count_ = 0;
    size_t raster_next_ = 0;

    uint32_t frames_since_reset_ = 0;
    uint32_t unsynced_streak_ = 0;
    uint32_t no_ack_streak_ = 0;
    uint32_t empty_queue_streak_ = 0;
    uint32_t frame_skip_streak_ = 0;
    clock::time_point last_decoder_restart_{};
};

} // namespace groovy
