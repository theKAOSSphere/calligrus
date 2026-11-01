// Touch gestures: see touch.h. The scroll physics are re-implemented from the public behaviour of the
// Android Open Source Project (Apache License 2.0): android.view.ViewConfiguration,
// android.widget.OverScroller / SplineOverScroller and android.view.VelocityTracker (LSQ2 strategy, and the impulse one for comparison).
#include "touch.h"

#include <algorithm>
#include <cmath>

namespace touch {

namespace {

// SplineOverScroller constants.
const float kInflexion = 0.35f;        // SplineOverScroller.INFLEXION: where the flick turns into the long glide
const float kStartTension = 0.5f;      // START_TENSION
const float kEndTension = 1.0f;        // END_TENSION
const float kGravity = 9.80665f;       // SensorManager.GRAVITY_EARTH
const int kSamples = 100;              // NB_SAMPLES
// DECELERATION_RATE = ln(0.78) / ln(0.9)
const float kDecelerationRate = float(std::log(0.78) / std::log(0.9));

// SPLINE_POSITION: the position of a fling (0..1) at kSamples + 1 evenly spaced times, found the way
// SplineOverScroller's static initializer does: a cubic Bezier through (0,0) .. (1,1) with control values
// P1 = START_TENSION * INFLEXION and P2 = 1 - END_TENSION * (1 - INFLEXION), solved by bisection.
const float* spline_position() {
    static float table[kSamples + 1];
    static bool ready = false;
    if (!ready) {
        const float p1 = kStartTension * kInflexion;
        const float p2 = 1.0f - kEndTension * (1.0f - kInflexion);
        float x_min = 0.0f;
        for (int i = 0; i < kSamples; i++) {
            const float alpha = float(i) / kSamples;
            float x_max = 1.0f, x, coef;
            for (int guard = 0; guard < 100; guard++) {
                x = x_min + (x_max - x_min) / 2.0f;
                coef = 3.0f * x * (1.0f - x);
                const float tx = coef * ((1.0f - x) * p1 + x * p2) + x * x * x;
                if (std::fabs(tx - alpha) < 1e-5f) break;
                if (tx > alpha) x_max = x;
                else x_min = x;
            }
            table[i] = coef * ((1.0f - x) * kStartTension + x) + x * x * x;
        }
        table[kSamples] = 1.0f;
        ready = true;
    }
    return table;
}

// SplineOverScroller.getSplineDeceleration
double spline_deceleration(float v, const Params& p) {
    const double coeff = double(kGravity) * 39.37 * double(p.ppi) * 0.84;  // mPhysicalCoeff
    return std::log(double(kInflexion) * std::fabs(double(v)) / (double(p.friction) * coeff));
}

float kinetic_energy_to_velocity(float work) { return (work < 0 ? -1.0f : 1.0f) * std::sqrt(std::fabs(work)) * 1.41421356f; }

}  // namespace

Params Params::for_density(float density) {
    Params p;
    p.slop *= density, p.min_fling *= density, p.max_fling *= density;
    p.ppi = 160.0f * density;
    return p;
}

float spline_fling_distance(float v, const Params& p) {
    if (v == 0) return 0;
    const double l = spline_deceleration(v, p);
    const double decel_minus_one = double(kDecelerationRate) - 1.0;
    const double coeff = double(p.friction) * 9.80665 * 39.37 * double(p.ppi) * 0.84;
    const double d = coeff * std::exp(double(kDecelerationRate) / decel_minus_one * l);
    return float(v < 0 ? -d : d);
}

double spline_fling_duration(float v, const Params& p) {
    if (v == 0) return 0;
    const double l = spline_deceleration(v, p);
    return std::floor(1000.0 * std::exp(l / (double(kDecelerationRate) - 1.0))) / 1000.0;  // (whole milliseconds)
}

float spline_fling_position(float v, double t, const Params& p) {
    const double dur = spline_fling_duration(v, p);
    const float total = spline_fling_distance(v, p);
    if (dur <= 0 || t >= dur) return total;
    if (t <= 0) return 0;
    const float* pos = spline_position();
    const float frac = float(t / dur);
    const int index = std::min(int(kSamples * frac), kSamples - 1);
    const float t_inf = float(index) / kSamples, t_sup = float(index + 1) / kSamples;
    const float slope = (pos[index + 1] - pos[index]) / (t_sup - t_inf);
    return (pos[index] + (frac - t_inf) * slope) * total;
}

float impulse_velocity(const std::vector<VSample>& s) {
    if (s.size() < 2) return 0;
    // VelocityTracker.calculateImpulseVelocity: the finger's kinetic energy "work" built up sample by sample.
    float work = 0;
    for (size_t i = 1; i < s.size(); i++) {
        if (s[i].t == s[i - 1].t) continue;
        const float vprev = kinetic_energy_to_velocity(work);
        const float vcurr = (s[i].y - s[i - 1].y) / float(s[i].t - s[i - 1].t);
        work += (vcurr - vprev) * std::fabs(vcurr);
        if (i == 1) work *= 0.5f;  // initial condition: the first interval starts from rest
    }
    return kinetic_energy_to_velocity(work);
}

float lsq2_velocity(const std::vector<VSample>& s, double horizon_s, size_t max_samples) {
    // VelocityTracker's LeastSquaresVelocityTrackerStrategy (degree 2): walk back from the newest sample while the
    // sample is no older than the horizon (at most max_samples of them), with tau = -age in seconds.
    if (s.empty()) return 0;
    std::vector<double> tau, y;
    for (size_t i = s.size(); i-- > 0 && tau.size() < max_samples;) {
        const double age = s.back().t - s[i].t;
        if (age > horizon_s) break;
        tau.push_back(-age);
        y.push_back(double(s[i].y));
    }
    const size_t m = tau.size();
    const size_t degree = std::min<size_t>(2, m - 1);
    if (degree < 1) return 0;
    // Normal equations of the polynomial fit, c0 + c1 tau (+ c2 tau^2): A c = b with A[j][k] = sum tau^(j+k).
    const size_t n = degree + 1;
    double a[3][4] = {};
    for (size_t i = 0; i < m; i++) {
        const double pw[5] = {1, tau[i], tau[i] * tau[i], tau[i] * tau[i] * tau[i], tau[i] * tau[i] * tau[i] * tau[i]};
        for (size_t j = 0; j < n; j++) {
            for (size_t k = 0; k < n; k++) a[j][k] += pw[j + k];
            a[j][n] += pw[j] * y[i];
        }
    }
    // Gaussian elimination with partial pivoting; a (nearly) singular system has no velocity (AOSP: no estimate).
    for (size_t c = 0; c < n; c++) {
        size_t piv = c;
        for (size_t r = c + 1; r < n; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return 0;
        for (size_t k = 0; k <= n; k++) std::swap(a[c][k], a[piv][k]);
        for (size_t r = c + 1; r < n; r++) {
            const double f = a[r][c] / a[c][c];
            for (size_t k = c; k <= n; k++) a[r][k] -= f * a[c][k];
        }
    }
    double coef[3] = {};
    for (size_t c = n; c-- > 0;) {
        double v = a[c][n];
        for (size_t k = c + 1; k < n; k++) v -= a[c][k] * coef[k];
        coef[c] = v / a[c][c];
    }
    return float(coef[1]);  // y'(0)
}

int Tracker::find(int id) const {
    for (int i = 0; i < n_; i++)
        if (f_[i].id == id) return i;
    return -1;
}

// VelocityTracker.addMovement: a pause of stale_s or more means the pointer had stopped: forget the past.
void Tracker::sample(double t, float y) {
    if (!samples_.empty() && t < samples_.back().t) return;  // (out of order: not a movement of the finger)
    if (!samples_.empty() && t - samples_.back().t >= p_.stale_s) samples_.clear();
    // Two samples with the same time are one (LSQ's addMovement: ACTION_MOVE and ACTION_POINTER_DOWN can share it).
    if (!samples_.empty() && t == samples_.back().t) samples_.back().y = y;
    else samples_.push_back({t, y});
    while (!samples_.empty() && (t - samples_.front().t > p_.velocity_window_s || samples_.size() > p_.velocity_max_samples))
        samples_.erase(samples_.begin());
}

void Tracker::down(int id, float x, float y, double t) {
    if (n_ >= 2 || find(id) >= 0) return;  // a third finger is ignored
    if (n_ == 0) {
        suppress_tap_ = flinging_;  // a touch that catches a fling only stops it
        flinging_ = false, v_ = 0, fling_total_ = fling_given_ = 0;
        dragging_ = pinched_ = moved_ = false;
        samples_.clear();
        start_x_ = x, start_y_ = y, t_down_ = t;
        last_y_ = y;
        sample(t, y);
    }
    f_[n_++] = {id, x, y};
    if (n_ == 2) {  // pinch: the baseline is the pair as it stands
        pinched_ = dragging_ = true;
        const float dx = f_[1].x - f_[0].x, dy = f_[1].y - f_[0].y;
        pinch_dist_ = std::sqrt(dx * dx + dy * dy);
        last_y_ = (f_[0].y + f_[1].y) / 2;
        focus_x_ = (f_[0].x + f_[1].x) / 2, focus_y_ = last_y_;
        samples_.clear();
    }
}

void Tracker::move(int id, float x, float y, double t) {
    const int i = find(id);
    if (i < 0) return;
    f_[i].x = x, f_[i].y = y;
    if (n_ == 1) {
        sample(t, y);  // (the tracker sees every movement from the press on)
        if (!dragging_) {
            const float dx = x - start_x_, dy = y - start_y_;
            if (std::sqrt(dx * dx + dy * dy) > p_.slop) moved_ = true;
            if (std::fabs(dy) <= p_.slop) return;
            // ScrollView: the slop is subtracted when the drag starts, so the content starts from rest.
            dragging_ = true;
            last_y_ = start_y_ + (dy > 0 ? p_.slop : -p_.slop);
        }
        pending_move_ += y - last_y_;
        last_y_ = y;
        focus_x_ = x, focus_y_ = y;
    } else if (n_ == 2) {
        const float dx = f_[1].x - f_[0].x, dy = f_[1].y - f_[0].y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (pinch_dist_ > 1.0f && dist > 1.0f) pending_zoom_ *= dist / pinch_dist_;
        pinch_dist_ = dist;
        const float cy = (f_[0].y + f_[1].y) / 2;
        focus_x_ = (f_[0].x + f_[1].x) / 2, focus_y_ = cy;
        pending_move_ += cy - last_y_;
        last_y_ = cy;
    }
}

void Tracker::start_fling(double t) {
    lift_ = Lift();
    lift_.valid = true;
    lift_.samples = int(samples_.size());
    lift_.stale = !samples_.empty() && t - samples_.back().t >= p_.stale_s;
    // The finger rested before the lift: no momentum.
    if (samples_.size() < 2 || lift_.stale) return;
    lift_.impulse = impulse_velocity(samples_);
    lift_.lsq2 = lsq2_velocity(samples_, p_.velocity_window_s, p_.velocity_max_samples);
    float v = std::clamp(lift_.lsq2, -p_.max_fling, p_.max_fling);
    lift_.clamped = v;
    if (std::fabs(v) < p_.min_fling) return;  // ViewConfiguration.getScaledMinimumFlingVelocity
    lift_.flung = true;
    v_ = v, flinging_ = true;
    fling_total_ = spline_fling_distance(v, p_);
    fling_given_ = 0;
    fling_start_ = t;
}

void Tracker::up(int id, float x, float y, double t) {
    const int i = find(id);
    if (i < 0) return;
    f_[i].x = x, f_[i].y = y;
    if (n_ == 2) {  // one finger stays: it carries on as a drag, from where it is (no jump, no fling)
        f_[0] = f_[1 - i];
        n_ = 1;
        last_y_ = f_[0].y;
        samples_.clear();
        return;
    }
    n_ = 0;
    if (pinched_) return;
    if (dragging_) start_fling(t);
    else if (!suppress_tap_ && !moved_ && t - t_down_ <= p_.tap_max_s) taps_.push_back({start_x_, start_y_});
}

void Tracker::cancel() {
    n_ = 0;
    dragging_ = pinched_ = false;
    flinging_ = false, v_ = 0, fling_total_ = fling_given_ = 0;
    samples_.clear();
}

Step Tracker::update(double t) {
    Step s;
    s.move_y = pending_move_;
    s.zoom = pending_zoom_;
    s.focus_x = focus_x_, s.focus_y = focus_y_;
    s.pinching = n_ == 2;
    if (n_ == 1 && !dragging_ && !moved_ && !pinched_ && !suppress_tap_ && t - t_down_ >= p_.press_delay_s) {
        s.pressing = true;
        s.press_x = start_x_, s.press_y = start_y_;
    }
    s.taps = std::move(taps_);
    taps_.clear();
    pending_move_ = 0, pending_zoom_ = 1.0f;
    if (flinging_ && n_ == 0) {
        // The fling's position along SplineOverScroller's curve, `t` seconds after the lift; hand out the new part.
        const double el = std::max(0.0, t - fling_start_);
        float at = spline_fling_position(v_, el, p_);
        if (el >= spline_fling_duration(v_, p_)) at = fling_total_, flinging_ = false;
        s.move_y += at - fling_given_;
        s.fling_y = at - fling_given_;
        fling_given_ = at;
    }
    return s;
}

}  // namespace touch
