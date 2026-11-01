// Touch gestures, kept free of ImGui, the window and the clock so they can be tested from the console
// (calligrus-touch-test). The Android shell feeds finger events with their timestamps (seconds on any
// steady clock) and asks once per frame what to do: scroll, zoom, or tap. (Spec: section 35.)
//
// The scrolling follows Android's own physics, from the Android Open Source Project (Apache License 2.0):
//   * android.view.ViewConfiguration: touch slop 8 dp, minimum fling velocity 50 dp/s, maximum 8000 dp/s,
//     scroll friction 0.015, tap timeout 100 ms (here 90 ms before a press shows);
//   * android.widget.OverScroller / SplineOverScroller: the fling's distance, duration and position curve;
//   * android.view.VelocityTracker, the default "LSQ2" strategy (what the touch screen's X and Y use): the speed of
//     the finger at the lift is the slope at the newest sample of a least-squares parabola through the samples of
//     the last 100 ms (at most 20); a pause of 40 ms or more (ASSUME_POINTER_STOPPED_TIME) forgets older samples,
//     and two samples with the same time are one (the later wins). The "impulse" strategy is still here
//     (impulse_velocity) for comparison; the debug log prints both.
//
//   * a drag moves the content 1:1 with the finger; the slop distance is subtracted when the drag starts,
//     so the content starts from rest (as ScrollView does);
//   * lifting while moving starts a fling (SplineOverScroller); a finger that rested >= 40 ms does not;
//   * a short press that did not move is a tap; a touch that stops a running fling is not;
//   * two fingers pinch: the distance ratio is the zoom factor, the centre still scrolls.
#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace touch {

struct Params {
    float slop = 8.0f;                 // px a finger may wander before the press becomes a drag (8 dp)
    double tap_max_s = 0.35;           // the longest press that still counts as a tap
    double press_delay_s = 0.09;       // a press shows (pressed button) only after this long without moving
    float min_fling = 50.0f;           // px/s: a lift slower than this just stops (50 dp/s)
    float max_fling = 8000.0f;         // px/s: faster lifts are capped (8000 dp/s)
    double velocity_window_s = 0.10;   // VelocityTracker HORIZON: the lift speed uses this much of the end of the drag
    size_t velocity_max_samples = 20;  // VelocityTracker's LSQ HISTORY_SIZE
    double stale_s = 0.04;             // VelocityTracker ASSUME_POINTER_STOPPED_TIME: no momentum after a rest this long
    float ppi = 160.0f;                // 160 * density: scales the fling's physical coefficient
    float friction = 0.015f;           // ViewConfiguration.getScrollFriction()

    // ViewConfiguration's limits for a screen of this density (dp -> px).
    static Params for_density(float density);
};

// SplineOverScroller's fling for a lift velocity `v` (px/s, sign = direction): total distance (px) and
// duration (s). `fling_position` is the distance covered `t` seconds after the lift (0 <= t, clamped at the end).
float spline_fling_distance(float v, const Params& p);
double spline_fling_duration(float v, const Params& p);
float spline_fling_position(float v, double t, const Params& p);

// VelocityTracker's impulse strategy over samples (oldest first), px/s. 0 for fewer than two samples.
struct VSample {
    double t;
    float y;
};
float impulse_velocity(const std::vector<VSample>& samples);

// VelocityTracker's LSQ2 strategy (Android's default for a finger's X and Y), px/s: the samples within `horizon_s` of
// the newest (at most `max_samples`, newest first) are fitted with a parabola y(tau) in the time tau <= 0 before the
// newest sample, by least squares; the velocity is y'(0). With fewer than three samples the fit is a line (the degree
// is the sample count minus one); with one sample, or when the fit is degenerate, the velocity is 0.
float lsq2_velocity(const std::vector<VSample>& samples, double horizon_s = 0.10, size_t max_samples = 20);

// What the last lift worked out (for the touch debug log, see window_android.cpp).
struct Lift {
    bool valid = false;
    int samples = 0;    // the samples the estimate used
    float lsq2 = 0;     // px/s, the strategy in use
    float impulse = 0;  // px/s, what the impulse strategy would have said
    float clamped = 0;  // px/s, lsq2 limited to the maximum
    bool stale = false; // the finger had rested 40 ms or more: no momentum
    bool flung = false;
};

struct Tap {
    float x = 0, y = 0;
};

// What happened since the last update().
struct Step {
    float move_y = 0;                // px the content moves (+ = down, the way the fingers go)
    float fling_y = 0;               // ...of which the fling's share
    float zoom = 1.0f;               // factor to multiply the zoom with (pinch)
    float focus_x = 0, focus_y = 0;  // the pinch centre (or the finger)
    bool pinching = false;           // two fingers are down
    bool pressing = false;           // a finger has rested (without moving) for press_delay_s: show the press
    float press_x = 0, press_y = 0;
    std::vector<Tap> taps;
};

// A scrolled list (popup, panel): the offset after the content moved `move_y` px with the finger (+ = down, so the offset
// shrinks), kept within 0..max. `hit_end` says a limit stopped it (a fling ends there, as in a native list).
inline float scroll_list(float offset, float move_y, float max, bool* hit_end = nullptr) {
    const float want = offset - move_y;
    const float got = want < 0 ? 0 : want > max ? max : want;
    if (hit_end) *hit_end = move_y != 0 && got != want;
    return got;
}

class Tracker {
public:
    explicit Tracker(const Params& p = Params()) : p_(p) {}

    void down(int id, float x, float y, double t);
    void move(int id, float x, float y, double t);
    void up(int id, float x, float y, double t);
    void cancel();  // the system took the touch away: stop, no fling, no tap

    // Collects what the events since the last call (and a running fling up to `t`) amount to.
    Step update(double t);

    // The scroll hit an end of the document: the fling is over.
    void stop_fling() { flinging_ = false, v_ = 0; }

    bool touching() const { return n_ > 0; }
    bool flinging() const { return flinging_; }
    bool pinching() const { return n_ == 2; }
    bool dragging() const { return dragging_; }  // the slop was crossed along this tracker's axis
    // One finger has wandered past the slop without a vertical drag starting: a sideways gesture (a slider).
    bool horizontal() const { return n_ == 1 && moved_ && !dragging_; }
    float start_x() const { return start_x_; }
    float start_y() const { return start_y_; }
    // Something is going on that needs frames (a finger down, a fling, results not yet collected).
    bool busy() const { return n_ > 0 || flinging_ || pending_move_ != 0 || pending_zoom_ != 1.0f || !taps_.empty(); }
    // What the most recent lift computed (valid after up()).
    const Lift& last_lift() const { return lift_; }
    // The lift velocity (px/s) the running fling started with.
    float fling_speed() const { return v_; }
    // The distance (px) the running fling will cover in all, and how much of it has been handed out.
    float fling_total() const { return fling_total_; }

private:
    struct Finger {
        int id = -1;
        float x = 0, y = 0;
    };

    int find(int id) const;
    void sample(double t, float y);
    void start_fling(double t);

    Params p_;
    Finger f_[2];
    int n_ = 0;
    bool dragging_ = false;  // the slop was crossed (or a finger remains after a pinch)
    bool moved_ = false;     // the finger wandered beyond the slop (no tap any more)
    bool pinched_ = false;   // a second finger touched during this gesture
    bool suppress_tap_ = false;
    float start_x_ = 0, start_y_ = 0;
    double t_down_ = 0;
    float last_y_ = 0;  // the y (or centre y) the content was last moved to
    float pinch_dist_ = 0;
    std::vector<VSample> samples_;
    Lift lift_;
    float pending_move_ = 0;
    float pending_zoom_ = 1.0f;
    float focus_x_ = 0, focus_y_ = 0;
    std::vector<Tap> taps_;
    bool flinging_ = false;
    float v_ = 0;            // lift velocity, px/s
    float fling_total_ = 0;  // signed total distance of this fling
    float fling_given_ = 0;  // of which handed out already
    double fling_start_ = 0;
};

}  // namespace touch
