#pragma once

#include "field_prep.h"
#include "groovy_sender.h"
#include "jpeg_decode.h"
#include "latency_probe.h"
#include "rate_lock.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace groovy {

// The streaming pipeline, independent of capture and of network back-pressure:
//
//   capture callback -> submit_*() -> [one pending sample]
//   prep thread      -> decode, scale, LZ4 -> [triple buffer of prepared fields]
//   sender thread    -> send on the raster, or repeat at its deadline
//   audio thread     -> paced 10 ms PCM chunks
//
// A newer sample replaces an unprocessed one instead of forming a queue. While
// running, the threads are the sole users of the Sender.
class AsyncSender {
public:
    AsyncSender() = default;
    AsyncSender(const AsyncSender&) = delete;
    AsyncSender& operator=(const AsyncSender&) = delete;
    ~AsyncSender();

    // Connects on its own thread: waiting for Groovy's INIT ACK must not stall
    // the window.
    void start_open(const std::string& ip, VideoMode mode, uint32_t audio_rate_hz);

    // Stops the threads (so no blit is half-sent), then closes the session.
    void close();

    bool set_mode(VideoMode mode);
    // Part of Groovy's INIT, so the picture restarts, but neither clock changes.
    bool set_audio(uint32_t audio_rate_hz);
    void set_view(const ViewSettings& view);

    // Capture callbacks. `pitch` is the row stride (luma stride for 4:2:0).
    // media_100ns is the sample's own timestamp, in 100 ns units, or -1 if it has none.
    void submit_packed422(const uint8_t* source, long pitch, unsigned width, unsigned height,
                          bool uyvy, std::int64_t media_100ns);
    // B8G8R8A8, tightly stored after the copy. Alpha is not used.
    void submit_bgra(const uint8_t* source, long pitch, unsigned width, unsigned height,
                     std::int64_t media_100ns);
    // Scale a desktop picture as it arrives and hold it until release_held_frame().
    // Those releases are the stream. scale_budget() is how long a picture needs,
    // so a newer one starts only when it can finish in time.
    bool submit_bgra_held(const uint8_t* source, long pitch, unsigned width, unsigned height);
    // One beat of the desktop clock. Counted from the start, so the rate is known
    // before the stream begins.
    void note_desktop_tick();
    bool release_held_frame();
    std::chrono::nanoseconds scale_budget() const;
    bool desktop_scale_busy() const { return scale_busy_.load(); }
    HANDLE held_scale_event();
    void submit_planar420(const uint8_t* source, long pitch, unsigned width, unsigned height,
                          bool i420, std::int64_t media_100ns);
    void submit_mjpeg(const uint8_t* jpeg, size_t size, unsigned width, unsigned height,
                      std::int64_t media_100ns);
    void submit_pcm(const uint8_t* pcm, size_t bytes);

    // Latest decoded MJPEG frame as RGB24; false if nothing newer than `seen`.
    bool copy_preview(std::vector<uint8_t>& dest, unsigned& width, unsigned& height,
                      uint64_t& seen);

    struct Statistics {
        double send_ms = 0.0; // total
        uint64_t sent = 0, replaced = 0, errors = 0;
        uint64_t duplicates = 0; // repeated pictures sent since this stream started
    };
    Statistics take_statistics();
    // Average MJPEG decode time since the last call (0 if none).
    double take_decode_ms();
    // Messages from the sender thread, for the main thread to print.
    std::string take_log();
    // Latest one-second latency line; empty if none since the last call.
    std::string take_probe_line();
    // The capture rate as timed so far, from any device being captured. False until
    // there are a couple of seconds of it.
    bool measured_capture_rate(double& fps);
    // Connected, but capture runs at a rate no plan fits, as a card can with its source off.
    bool capture_unusual() const { return capture_unusual_.load(); }

    bool connected() const { return connected_.load(); }
    // The mode in use: the requested one, or its NTSC or PAL counterpart if only that
    // fits the measured capture rate. Valid while connected.
    VideoMode mode() const { return sender_.mode(); }
    bool opening() const { return opening_.load(); }
    // Showing the Calibrating image while timing capture and the MiSTer.
    bool calibrating() const { return calibrating_.load(); }
    // Why the stream failed; empty if it did not.
    std::string error() const;

    void set_timing_log(bool enabled) { timing_log_.store(enabled); }
    // Takes effect on the next thread start.
    void set_rate_lock(bool enabled) { rate_lock_.store(enabled); }
    // The capture was reopened and its rate is unknown: measure again.
    void restart_rate_lock();
    // The capture graph was replaced. The next source learns its own stamp-to-arrival
    // gap; a frame already in flight from the old graph does not set it.
    void reset_handoff();
    // The source changed during a stream: call start_recalibrate() to show the
    // Calibrating image and time it again.
    bool recalibrate_needed() const { return rate_lock_.load() && recalibrate_.load(); }
    void start_recalibrate();

private:
    enum class PendingKind { Yuy2, Uyvy, Mjpeg, Nv12, I420, Bgra };

    // Capture timing wanted before planning a rate.
    static constexpr double kCaptureMeasureSeconds = 5.0;
    static constexpr double kTrimCaptureSeconds = 2.0;

    struct FreeWatch {
        std::chrono::steady_clock::time_point next{};
        double fps = 0.0;
    };

    bool open(const std::string& ip, VideoMode mode, uint32_t audio_rate_hz);
    VideoMode plan_start(VideoMode mode);
    double start_clock_error() const;
    bool relock_allowed();
    bool free_run_can_lock(std::chrono::steady_clock::time_point now, FreeWatch& watch);
    bool prepare_calibration_fields(VideoMode mode, PreparedField& image, PreparedField& black);
    bool prepare_black(VideoMode mode, PreparedField& black);
    bool send_at_deadline(const PreparedField& field);
    bool wait_for_capture(VideoMode mode);
    bool calibrate(VideoMode mode);
    bool calibrate_and_start(VideoMode mode);
    void stop_session();
    void start_workers();
    void stop_workers();
    void stop_on_send_error();
    void set_error(std::string value);
    void report_sender_error();
    void add_log(const std::string& text);

    void publish_pending(PendingKind kind, unsigned width, unsigned height,
                         std::chrono::steady_clock::time_point source, bool hold = false,
                         std::chrono::steady_clock::time_point started = {});
    void note_desktop_scale(std::chrono::nanoseconds cost);
    void finish_held_scale(bool keep, const PreparedField& field, uint64_t epoch);
    std::chrono::steady_clock::time_point media_time(std::int64_t sample_100ns);
    bool late_handoff(const PreparedField& field) const;
    void note_handoff(const PreparedField& field);
    void publish_preview(const uint8_t* rgb, unsigned width, unsigned height);
    bool decode_mjpeg(const std::vector<uint8_t>& jpeg, unsigned width, unsigned height,
                      std::vector<uint8_t>& rgb, double& decode_ms);
    bool take_pcm(std::vector<uint8_t>& out, size_t want);
    void record_probe(LatencyProbe& probe, bool fresh, std::chrono::steady_clock::time_point captured,
                      double pickup_ms, double decode_ms, unsigned& repeats, double trim_ppm);

    void prep_loop(VideoMode mode);
    void sender_loop();
    void audio_loop();

    Sender sender_;
    mutable std::mutex error_mutex_;
    std::string error_;
    std::thread open_thread_;
    std::atomic<bool> opening_{false};
    std::atomic<bool> calibrating_{false};
    std::atomic<bool> cancel_open_{false};
    std::atomic<bool> connected_{false};

    std::thread prep_thread_;
    std::thread sender_thread_;
    std::thread audio_thread_;

    // Guards pending_*, the prepared_ indices and running_.
    std::mutex mutex_;
    std::condition_variable pending_cv_;  // capture sample -> prep
    std::condition_variable prepared_cv_; // prepared field -> sender
    bool running_ = false;
    bool pending_ready_ = false;
    std::vector<uint8_t> pending_;
    std::vector<uint8_t> incoming_; // capture thread only: filled outside the lock
    std::chrono::steady_clock::time_point pending_at_{};
    std::chrono::steady_clock::time_point pending_source_{};
    std::chrono::steady_clock::time_point pending_started_{};
    bool pending_hold_ = false;
    PreparedField held_;
    bool held_ready_ = false;
    std::atomic<bool> scale_busy_{false};
    HANDLE scale_done_ = nullptr;
    std::array<std::chrono::nanoseconds, 8> scale_ring_{};
    int scale_ring_pos_ = 0;
    int scale_ring_count_ = 0;
    std::atomic<std::int64_t> scale_budget_ns_{0};
    unsigned pending_w_ = 0;
    unsigned pending_h_ = 0;
    PendingKind pending_kind_ = PendingKind::Yuy2;
    std::array<PreparedField, 3> prepared_;
    PreparedField black_; // sent until the first sample arrives
    bool have_black_ = false;
    int prep_write_ = 0, prep_ready_ = 1, prep_front_ = 2;
    std::atomic<uint8_t> next_parity_{0}; // what the next blit should need: parities alternate
    bool prep_new_ = false;

    FieldPrep field_prep_;
    jpeg_decode::Decoder jpeg_decoder_;
    std::atomic<bool> letterbox_{false};
    std::atomic<int> crop_h_{0};
    std::atomic<int> crop_v_{0};

    std::mutex audio_mutex_;
    std::condition_variable audio_cv_;
    std::vector<uint8_t> audio_;
    std::atomic<bool> audio_run_{false};

    std::mutex preview_mutex_;
    std::vector<uint8_t> preview_;
    unsigned preview_w_ = 0;
    unsigned preview_h_ = 0;
    uint64_t preview_seq_ = 0;

    std::atomic<bool> timing_log_{false};
    std::atomic<bool> rate_lock_{true};
    std::atomic<bool> recalibrate_{false};
    std::atomic<bool> capture_unusual_{false};
    std::vector<std::chrono::steady_clock::time_point> relocks_;
    std::chrono::steady_clock::time_point free_until_{};
    CaptureRateMeter capture_rate_;
    // How long after its timestamp a picture usually arrives, from the last one
    // that was on time. A later picture repeats this refresh and goes out on the
    // next one. handoff_epoch_ advances when the capture graph is replaced, so
    // the delay is learned again.
    uint64_t handoff_epoch_ = 0;
    uint64_t pending_epoch_ = 0;
    bool delivery_valid_ = false;
    bool held_late_ = false;
    std::chrono::steady_clock::duration delivery_offset_{};
    bool media_origin_valid_ = false;
    std::int64_t media_origin_ = 0;
    std::chrono::steady_clock::time_point media_steady_{};
    RatePlan start_plan_;
    double start_fps_ = 0.0;
    double mister_clock_ = 1.0; // this MiSTer's actual rate relative to the one asked for
    double trim_ppm_ = 0.0;     // kept across mode changes, which clear it in the core
    std::mutex probe_mutex_;
    std::string probe_line_;
    std::string log_;

    std::atomic<uint64_t> decode_microseconds_{0};
    std::atomic<uint64_t> decoded_frames_{0};
    std::atomic<uint64_t> send_microseconds_{0};
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> duplicates_{0};
    std::atomic<uint64_t> replaced_{0};
    std::atomic<uint64_t> errors_{0};
};

} // namespace groovy
