// calligrus-touch-test: console checks (no window) for the touch gestures: 1:1 drag, slop, tap against drag,
// Android's spline fling (distances checked against values worked out by hand from the AOSP formulas at
// density 2.625), velocity clamps, the LSQ2 and impulse velocities (a short flick against hand-computed values, and batching),
// the stale rule, interrupting a fling, pinch.
// Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <string>

#include "touch.h"

using namespace touch;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}
static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// A finger that goes down at (x, y0) and moves in `steps` events, `dt` apart, by `dy` each.
// Returns the time of the last event. Does not lift.
static double drag(Tracker& tr, int id, float x, float y0, int steps, float dy, double t, double dt) {
    tr.down(id, x, y0, t);
    for (int i = 1; i <= steps; i++) {
        t += dt;
        tr.move(id, x, y0 + dy * float(i), t);
    }
    return t;
}

static const float kDensity = 2.625f;  // a 420 dpi phone
static Params phone() { return Params::for_density(kDensity); }

static void test_params() {
    const Params p = phone();
    check(near(p.slop, 21.0f), "touch slop is 8 dp (21 px at density 2.625)");
    check(near(p.min_fling, 131.25f), "minimum fling velocity is 50 dp/s");
    check(near(p.max_fling, 21000.0f), "maximum fling velocity is 8000 dp/s");
    check(near(p.ppi, 420.0f), "ppi is 160 * density");
}

static void test_drag() {
    Tracker tr(phone());
    // 10 events of 12 px: the slop (21 px) is crossed in the second event and subtracted.
    double t = drag(tr, 0, 100, 500, 10, 12, 0.0, 0.016);
    Step s = tr.update(t);
    check(near(s.move_y, 120.0f - 21.0f), "a drag moves the content 1:1 minus the slop (120 px of finger = 99 px)");
    check(s.taps.empty(), "a drag is not a tap");
    tr.move(0, 100, 640, t + 0.016);
    s = tr.update(t + 0.016);
    check(near(s.move_y, 20.0f), "the next move adds only the new distance");
    tr.move(0, 100, 600, t + 0.032);
    check(near(tr.update(t + 0.032).move_y, -40.0f), "moving back moves back");
}

static void test_slop() {
    Tracker tr(phone());
    tr.down(0, 100, 500, 0.0);
    tr.move(0, 100, 515, 0.016);  // 15 px: inside the slop (21)
    check(near(tr.update(0.02).move_y, 0.0f), "inside the slop nothing moves");
    tr.move(0, 100, 526, 0.032);  // 26 px: 5 px beyond the slop
    check(near(tr.update(0.033).move_y, 5.0f), "the slop is subtracted when the drag starts: the first move is the excess only");
    Tracker up(phone());
    up.down(0, 100, 500, 0.0);
    up.move(0, 100, 470, 0.016);  // 30 px up
    check(near(up.update(0.02).move_y, -9.0f), "the same upwards (-30 + 21)");
}

static void test_slop_and_tap() {
    Tracker tr(phone());
    tr.down(0, 50, 60, 1.0);
    tr.move(0, 54, 66, 1.02);  // inside the slop
    check(near(tr.update(1.03).move_y, 0.0f), "inside the slop nothing moves");
    tr.up(0, 54, 66, 1.10);
    Step s = tr.update(1.11);
    check(s.taps.size() == 1 && near(s.taps[0].x, 50) && near(s.taps[0].y, 60), "a short press without moving is a tap (at the press point)");
    check(!tr.flinging() && !tr.busy(), "a tap leaves nothing running");

    tr.down(0, 50, 60, 2.0);
    tr.up(0, 50, 60, 2.6);
    check(tr.update(2.61).taps.empty(), "a long press is not a tap");

    tr.down(0, 50, 60, 3.0);
    tr.move(0, 50, 100, 3.03);
    tr.up(0, 50, 100, 3.5);  // (it rested first: no fling)
    s = tr.update(3.51);
    check(s.taps.empty() && near(s.move_y, 40.0f - 21.0f), "crossing the slop turns the press into a drag, no tap on lift");

    tr.down(0, 50, 60, 4.0);
    tr.move(0, 90, 62, 4.02);  // sideways beyond the slop: not a tap, not a drag
    tr.up(0, 90, 62, 4.05);
    s = tr.update(4.06);
    check(s.taps.empty() && near(s.move_y, 0.0f), "a sideways wander beyond the slop is neither tap nor drag");
}

static void test_press_delay() {
    Tracker tr(phone());
    tr.down(0, 50, 60, 1.0);
    check(!tr.update(1.05).pressing, "no press feedback before 90 ms (a scroll that starts must not flash a pressed button)");
    Step s = tr.update(1.095);
    check(s.pressing && near(s.press_x, 50) && near(s.press_y, 60), "the press shows after 90 ms of rest");
    tr.move(0, 50, 100, 1.1);
    check(!tr.update(1.11).pressing, "a drag cancels the press");
    Tracker t2(phone());
    t2.down(0, 50, 60, 1.0);
    t2.move(0, 50, 100, 1.02);
    check(!t2.update(1.2).pressing, "a finger that dragged never shows a press");
}

static void test_spline() {
    const Params p = phone();
    // Worked out by hand from SplineOverScroller: l = ln(0.35 v / (0.015 coeff)), coeff = 9.80665 * 39.37 * ppi * 0.84,
    // distance = 0.015 coeff exp(DECELERATION_RATE / (DECELERATION_RATE - 1) l),
    // duration = floor(1000 exp(l / (DECELERATION_RATE - 1))) ms.
    struct Row {
        float v, dist;
        int ms;
    } rows[] = {{500, 28.6578f, 163}, {1000, 95.4799f, 272}, {2000, 318.1124f, 454}, {4000, 1059.8616f, 757}, {21000, 18864.2022f, 2566}};
    for (const Row& r : rows) {
        const float d = spline_fling_distance(r.v, p);
        check(near(d, r.dist, r.dist * 1e-4f + 0.01f), "spline distance at " + std::to_string(int(r.v)) + " px/s = " + std::to_string(d) + " (AOSP " + std::to_string(r.dist) + ")");
        check(std::lround(spline_fling_duration(r.v, p) * 1000) == r.ms, "spline duration at " + std::to_string(int(r.v)) + " px/s is " + std::to_string(r.ms) + " ms");
    }
    check(near(spline_fling_distance(-1000, p), -95.4799f, 0.02f), "a negative velocity flings the other way");
    // The curve: SPLINE_POSITION table lookups at a quarter and half of the 272 ms.
    check(near(spline_fling_position(1000, 0.068, p), 55.742f, 0.05f), "the curve at 68 ms is 55.74 px");
    check(near(spline_fling_position(1000, 0.136, p), 81.961f, 0.05f), "the curve at 136 ms is 81.96 px");
    check(near(spline_fling_position(1000, 5.0, p), 95.4799f, 0.02f), "after the duration the whole distance is covered");
    check(near(spline_fling_position(1000, 0.0, p), 0.0f), "at the start nothing is covered");
    check(spline_fling_distance(1000, Params::for_density(1.0f)) > spline_fling_distance(1000, p), "the same px/s is a slower real speed on a denser screen: it flings less far");
}

static void test_impulse() {
    std::vector<VSample> steady = {{0, 0}, {0.016, 16}, {0.032, 32}, {0.048, 48}};
    check(near(impulse_velocity(steady), 1000.0f, 0.5f), "impulse: steady motion at 1000 px/s is 1000 px/s");
    std::vector<VSample> accel = {{0, 0}, {0.016, 8}, {0.032, 32}, {0.048, 80}};
    check(near(impulse_velocity(accel), 3230.07f, 0.5f), "impulse: an accelerating finger (hand-computed 3230.07 px/s)");
    std::vector<VSample> up = {{0, 0}, {0.016, -16}, {0.032, -32}};
    check(near(impulse_velocity(up), -1000.0f, 0.5f), "impulse: upward motion is negative");
    check(impulse_velocity({{0, 0}}) == 0.0f, "impulse: one sample is no velocity");
    // A fling needs only the last 100 ms: an old slow part does not count.
    Tracker tr(phone());
    tr.down(0, 100, 800, 0.0);
    double t = 0;
    for (int i = 1; i <= 20; i++) tr.move(0, 100, 800 - 1.0f * float(i), t += 0.01);   // slow, 100 px/s, 0.2 s
    for (int i = 1; i <= 10; i++) tr.move(0, 100, 780 - 20.0f * float(i), t += 0.01);  // then 2000 px/s for 0.1 s
    tr.up(0, 100, 580, t + 0.005);
    check(tr.flinging() && near(tr.fling_speed(), -2000.0f, 60.0f), "the lift speed comes from the last 100 ms (" + std::to_string(tr.fling_speed()) + ")");
}

// A short flick: 80 px of travel in 60 ms, six samples 12 ms apart (the finger speeds up: 6, 12, 20, 24, 18 px per step).
// Hand-computed (a degree-2 least-squares fit over tau = t - t_newest, velocity = the slope at tau = 0; numpy.polyfit
// agrees): LSQ2 = -2203.571 px/s. The impulse strategy says -1585.944 px/s for the same samples.
static const double kFlickT[6] = {0.000, 0.012, 0.024, 0.036, 0.048, 0.060};
static const float kFlickDy[6] = {0, 6, 18, 38, 62, 80};

static void test_lsq2() {
    check(near(lsq2_velocity({{0, 0}, {0.016, 16}, {0.032, 32}, {0.048, 48}}), 1000.0f, 0.01f), "lsq2: steady motion at 1000 px/s is 1000 px/s");
    check(near(lsq2_velocity({{0, 0}, {0.016, -16}, {0.032, -32}}), -1000.0f, 0.01f), "lsq2: upward motion is negative");
    check(lsq2_velocity({{0, 0}}) == 0.0f && lsq2_velocity({}) == 0.0f, "lsq2: one sample is no velocity");
    check(near(lsq2_velocity({{0, 0}, {0.060, 80}}), 1333.33f, 0.05f), "lsq2: two samples are a line (80 px in 60 ms = 1333.33 px/s)");
    check(near(lsq2_velocity({{0, 0}, {0.030, 20}, {0.060, 80}}), 2666.667f, 0.05f), "lsq2: three samples, a parabola through them (hand-computed 2666.67 px/s)");
    check(near(lsq2_velocity({{0, 0}, {0.020, 10}, {0.040, 40}, {0.060, 80}}), 2475.0f, 0.05f), "lsq2: four samples (hand-computed 2475 px/s)");
    std::vector<VSample> flick;
    for (int i = 0; i < 6; i++) flick.push_back({kFlickT[i], kFlickDy[i]});
    check(near(lsq2_velocity(flick), 2203.571f, 0.05f), "lsq2: the six-sample short flick (hand-computed 2203.57 px/s, " + std::to_string(lsq2_velocity(flick)) + ")");
    check(near(impulse_velocity(flick), 1585.944f, 0.05f), "impulse: the same flick (hand-computed 1585.94 px/s)");
    // Samples older than the 100 ms horizon do not count; neither do samples beyond the 20 newest.
    std::vector<VSample> old = {{-0.5, 300}, {-0.2, 200}};
    old.insert(old.end(), flick.begin(), flick.end());
    check(near(lsq2_velocity(old), lsq2_velocity(flick), 0.01f), "lsq2: samples older than 100 ms are ignored");
    std::vector<VSample> same_time = flick;
    same_time.insert(same_time.begin() + 3, same_time[2]);  // a sample repeated (same time and place)
    check(lsq2_velocity(same_time) != 0.0f, "lsq2: a repeated sample does not break the fit");
}

// The tracker gives the lift the LSQ2 speed, and it does not depend on how the events are batched, interleaved with
// frames, or repeated (spec 35: the samples are fed with their own times, however they arrive).
static float flick_speed(int mode, Lift* lift = nullptr) {
    Tracker tr(phone());
    const double t0 = 100.0;
    tr.down(0, 300, 1500, t0 + kFlickT[0]);
    for (int i = 1; i < 6; i++) {
        tr.move(0, 300, 1500 - kFlickDy[i], t0 + kFlickT[i]);
        if (mode == 1) tr.update(t0 + kFlickT[i]);  // a frame after every sample
        if (mode == 2 && i == 3) tr.move(0, 300, 1500 - kFlickDy[i], t0 + kFlickT[i]);  // the same sample again
        if (mode == 3) tr.move(0, 300, 1500 - kFlickDy[i] + 7, t0 + kFlickT[i]), tr.move(0, 300, 1500 - kFlickDy[i], t0 + kFlickT[i]);  // a wrong one, then the right one, same time: the later wins
    }
    tr.up(0, 300, 1420, t0 + 0.064);
    if (lift) *lift = tr.last_lift();
    return tr.flinging() ? tr.fling_speed() : 0.0f;
}

static void test_flick() {
    Lift l;
    const float v = flick_speed(0, &l);
    check(near(v, -2203.571f, 0.1f), "a short flick (80 px in 60 ms, six samples) lifts at the hand-computed LSQ2 speed (" + std::to_string(v) + ")");
    check(l.valid && l.samples == 6 && l.flung && !l.stale && near(l.lsq2, -2203.571f, 0.1f) && near(l.impulse, -1585.944f, 0.1f), "the lift record has both strategies and the sample count");
    check(near(flick_speed(1), v, 0.001f), "frames between the samples do not change the speed");
    check(near(flick_speed(2), v, 0.001f), "a repeated sample does not change the speed");
    check(near(flick_speed(3), v, 0.001f), "two samples at the same time are one (the later wins)");
    // Several samples arriving in one batch, as a MotionEvent's history: the same samples, the same speed.
    Tracker batch(phone());
    batch.down(0, 300, 1500, 100.0);
    for (int i = 1; i < 6; i++) batch.move(0, 300, 1500 - kFlickDy[i], 100.0 + kFlickT[i]);  // (no update() between: one frame)
    batch.up(0, 300, 1420, 100.064);
    check(near(batch.fling_speed(), v, 0.001f), "a batch of samples gives the same speed as the samples one by one");
    // Receive times would be wrong: the same samples all stamped with one time are not a movement at all (no fling).
    Tracker flat(phone());
    flat.down(0, 300, 1500, 100.0);
    for (int i = 1; i < 6; i++) flat.move(0, 300, 1500 - kFlickDy[i], 100.0);
    flat.up(0, 300, 1420, 100.004);
    check(!flat.flinging() || near(flat.fling_speed(), 0.0f, 1.0f), "samples with one common time carry no speed (why the real event times matter)");
}

static void test_clamps() {
    const Params p = phone();
    {
        Tracker tr(p);  // 100 px/s: below the minimum (131 px/s)
        double t = drag(tr, 0, 100, 800, 10, -1.6f, 0.0, 0.016);
        tr.up(0, 100, 784, t + 0.005);
        check(!tr.flinging(), "a lift slower than 50 dp/s does not fling");
    }
    {
        Tracker tr(p);  // 150 px/s: above it
        double t = drag(tr, 0, 100, 800, 10, -2.4f, 0.0, 0.016);
        tr.up(0, 100, 776, t + 0.005);
        check(tr.flinging() && near(tr.fling_speed(), -150.0f, 5.0f), "a lift a little above 50 dp/s flings");
    }
    {
        Tracker tr(p);  // 80000 px/s: capped to 21000
        double t = drag(tr, 0, 100, 5000, 6, -1280.0f, 0.0, 0.016);
        tr.up(0, 100, -2680, t + 0.002);
        check(tr.flinging() && near(tr.fling_speed(), -21000.0f, 1.0f), "a faster lift is capped at 8000 dp/s");
        check(near(tr.fling_total(), -18864.2f, 1.0f), "the capped fling covers the AOSP distance for 21000 px/s");
    }
}

static void test_stale() {
    Tracker tr(phone());
    double t = drag(tr, 0, 100, 800, 8, -20, 0.0, 0.016);
    tr.up(0, 100, 640, t + 0.039);
    check(tr.flinging(), "a rest of 39 ms still flings");
    Tracker r(phone());
    t = drag(r, 0, 100, 800, 8, -20, 0.0, 0.016);
    r.up(0, 100, 640, t + 0.04);
    check(!r.flinging(), "a finger that rested 40 ms before the lift has no momentum");
    Tracker r2(phone());
    t = drag(r2, 0, 100, 800, 8, -20, 0.0, 0.016);
    r2.up(0, 100, 640, t + 0.3);
    check(!r2.flinging(), "nor after a long rest");
}

static void test_fling() {
    const Params p = phone();
    Tracker tr(p);
    // 20 px per 16 ms = 1250 px/s, lifted at once.
    double t = drag(tr, 0, 100, 800, 12, -20, 0.0, 0.016);
    tr.up(0, 100, 560, t + 0.004);
    check(tr.flinging(), "lifting while moving starts a fling");
    check(near(tr.fling_speed(), -1250.0f, 10.0f), "fling speed is the finger's speed at the lift (-1250 px/s)");
    tr.update(t + 0.004);  // (the drag's own movement is delivered first)
    const float expect = spline_fling_distance(tr.fling_speed(), p);
    const double dur = spline_fling_duration(tr.fling_speed(), p);
    double now = t + 0.004;
    float total = 0, prev = 0;
    bool monotone = true;
    int frames = 0;
    while (tr.flinging() && frames < 2000) {
        now += 1.0 / 60;
        const float m = tr.update(now).move_y;
        monotone = monotone && m <= 0.0f && (prev == 0 || std::fabs(m) <= std::fabs(prev) + 1.0f);
        prev = m;
        total += m;
        frames++;
    }
    check(near(total, expect, 0.5f), "the fling covers the spline distance (" + std::to_string(total) + " vs " + std::to_string(expect) + ")");
    check(monotone, "the fling slows down every frame");
    check(!tr.flinging() && !tr.busy(), "the fling ends by itself");
    check(std::fabs(double(frames) / 60 - dur) < 0.05, "and takes the spline duration (" + std::to_string(frames) + " frames, " + std::to_string(dur) + " s)");

    // The frame rate does not change the distance (time based).
    Tracker slow(p);
    t = drag(slow, 0, 100, 800, 12, -20, 0.0, 0.016);
    slow.up(0, 100, 560, t + 0.004);
    slow.update(t + 0.004);
    float slow_total = 0;
    now = t + 0.004;
    for (int i = 0; i < 400 && slow.flinging(); i++) now += 1.0 / 30, slow_total += slow.update(now).move_y;
    check(near(slow_total, total, 0.5f), "30 fps and 60 fps flings agree");

    // A faster flick goes farther.
    Tracker fast(p);
    t = drag(fast, 0, 100, 900, 6, -60, 0.0, 0.016);  // 3750 px/s
    fast.up(0, 100, 540, t + 0.002);
    fast.update(t + 0.002);
    float far_total = 0;
    now = t;
    for (int i = 0; i < 600 && fast.flinging(); i++) now += 1.0 / 60, far_total += fast.update(now).move_y;
    check(far_total < total * 3 && far_total < -400.0f, "a faster flick goes farther");
}

static void test_swipe_events() {
    Tracker tr(phone());  // `input swipe`: events every few ms
    double t = drag(tr, 0, 100, 1800, 30, -40, 0.0, 0.005);
    tr.up(0, 100, 600, t + 0.003);
    check(tr.flinging() && tr.fling_speed() < -5000.0f, "a swipe with dense events flings at the swipe's speed");
}

static void test_fling_edges() {
    Tracker tr(phone());
    double t = drag(tr, 0, 100, 800, 8, -40, 0.0, 0.016);
    tr.up(0, 100, 480, t + 0.003);
    tr.update(t + 0.003);
    check(tr.flinging(), "fling running");
    tr.stop_fling();
    check(!tr.flinging() && near(tr.update(t + 0.02).move_y, 0.0f), "stop_fling (the scroll hit an end) stops it");
}

static void test_catch() {
    Tracker c(phone());
    double t = drag(c, 0, 100, 800, 12, -20, 0.0, 0.016);
    c.up(0, 100, 560, t + 0.004);
    c.update(t + 0.1);
    check(c.flinging(), "fling running");
    c.down(0, 100, 600, t + 0.2);
    check(!c.flinging(), "touching down stops a running fling at once");
    check(near(c.update(t + 0.21).move_y, 0.0f), "and nothing more is delivered");
    c.up(0, 100, 600, t + 0.25);
    check(c.update(t + 0.26).taps.empty(), "the touch that caught a fling is not a tap");
    check(!c.update(t + 0.3).pressing, "and shows no press");
    c.down(0, 100, 600, t + 1.0);
    c.up(0, 100, 600, t + 1.05);
    check(c.update(t + 1.06).taps.size() == 1, "the next touch is a tap again");
}

static void test_pinch() {
    Tracker tr(phone());
    tr.down(0, 300, 500, 0.0);
    tr.down(1, 500, 500, 0.01);  // 200 apart
    tr.move(1, 600, 500, 0.03);  // 300 apart
    Step s = tr.update(0.04);
    check(near(s.zoom, 1.5f, 1e-3f), "spreading the fingers from 200 to 300 px zooms by 1.5");
    check(near(s.focus_x, 450, 0.5f) && near(s.focus_y, 500, 0.5f), "the pinch centre is reported");
    check(s.pinching && tr.pinching(), "two fingers down is a pinch");
    tr.move(1, 450, 500, 0.05);  // 150 apart
    s = tr.update(0.06);
    check(near(s.zoom, 0.5f, 1e-3f), "pinching in halves it (300 -> 150)");
    tr.move(0, 300, 540, 0.07);
    tr.move(1, 450, 540, 0.07);
    s = tr.update(0.08);
    check(near(s.move_y, 40.0f, 0.5f) && near(s.zoom, 1.0f, 1e-3f), "moving both fingers scrolls by the centre's move");
    tr.up(1, 450, 540, 0.1);
    s = tr.update(0.11);
    check(s.taps.empty() && !tr.flinging() && !s.pinching, "lifting one finger of a pinch is no tap and no fling, the pinch is over");
    tr.move(0, 300, 600, 0.12);
    check(near(tr.update(0.13).move_y, 60.0f), "the finger left carries on as a drag, from where it is");
    tr.up(0, 300, 600, 0.14);
    check(!tr.flinging() && tr.update(0.15).taps.empty(), "a gesture that pinched never flings or taps");
    tr.down(0, 10, 10, 1.0);
    tr.down(1, 20, 20, 1.0);
    tr.down(2, 30, 30, 1.0);
    tr.cancel();
    check(!tr.touching() && !tr.busy(), "cancel clears everything");
}

// A finger on a popup list: the gesture decides scroll or click; nothing else moves (spec 35).
static void test_list_gestures() {
    // drag: scroll delta, no click, no press shown while moving
    Tracker tr(phone());
    tr.down(0, 300, 1000, 0.0);
    double t = 0;
    float scrolled = 0, off = 100;
    bool pressed = false;
    for (int i = 1; i <= 10; i++) {
        t += 0.016;
        tr.move(0, 300, 1000 - 12.0f * float(i), t);
        Step s = tr.update(t);
        pressed = pressed || s.pressing;
        off = scroll_list(off, s.move_y, 1e9f);
        scrolled += s.move_y;
    }
    tr.up(0, 300, 1000 - 120, t);
    check(near(off, 100 + 99.0f), "list drag: the offset follows the finger 1:1 after the slop (finger up = offset grows)");
    check(!pressed, "list drag: no pressed feedback while moving");
    check(tr.update(t).taps.empty(), "list drag: no click on lift");
    // clamp at the limits
    bool end = false;
    check(near(scroll_list(10, 50, 500, &end), 0) && end, "list scroll clamps at the top and reports the end");
    check(near(scroll_list(490, -50, 500, &end), 500) && end, "list scroll clamps at the bottom and reports the end");
    check(near(scroll_list(100, -50, 500, &end), 150) && !end, "list scroll inside the limits is no end");
    // tap: a click, no scroll
    Tracker tp(phone());
    tp.down(0, 300, 1000, 0.0);
    tp.move(0, 302, 1003, 0.02);
    tp.up(0, 302, 1003, 0.12);
    Step s = tp.update(0.12);
    check(s.taps.size() == 1 && s.move_y == 0, "list tap: one click, no scroll");
    // rest first (press shown), then drag: the press is cancelled, no click
    Tracker tr2(phone());
    tr2.down(0, 300, 1000, 0.0);
    s = tr2.update(0.1);
    check(s.pressing, "list: a resting finger shows the press after 90 ms");
    tr2.move(0, 300, 940, 0.12);
    s = tr2.update(0.12);
    check(!s.pressing && s.move_y != 0, "list: moving past the slop cancels the press and scrolls");
    tr2.up(0, 300, 940, 0.3);
    check(tr2.update(0.3).taps.empty(), "list: drag then rest then lift: no click");
    // sideways (slider) is told apart
    Tracker th(phone());
    th.down(0, 300, 1000, 0.0);
    th.move(0, 340, 1002, 0.02);
    check(th.horizontal() && th.update(0.02).move_y == 0, "a sideways wander is horizontal and does not scroll");
    Tracker tv(phone());
    tv.down(0, 300, 1000, 0.0);
    tv.move(0, 302, 960, 0.02);
    check(!tv.horizontal(), "a vertical drag is not horizontal");
    // Direction lock on a wide block: the page tracker and a tracker fed x as y (window_android.cpp); the first to pass the slop wins.
    {
        Tracker page(phone()), side(phone());
        page.down(0, 800, 1200, 0.0), side.down(0, 1200, 800, 0.0);
        double tt = 0;
        for (int i = 1; i <= 6; i++) {  // 10 px sideways, 1 px down per event
            tt += 0.016;
            page.move(0, 800 - 10.0f * float(i), 1200 + float(i), tt);
            side.move(0, 1200 + float(i), 800 - 10.0f * float(i), tt);
        }
        check(side.dragging() && !page.dragging(), "sideways finger: the block tracker crosses the slop, the page tracker does not");
        check(near(side.update(tt).move_y, -60.0f + 21.0f), "the block follows the finger 1:1 after the slop (content moves left)");
        Tracker page2(phone()), side2(phone());
        page2.down(0, 800, 1200, 0.0), side2.down(0, 1200, 800, 0.0);
        page2.move(0, 790, 1250, 0.016), side2.move(0, 1250, 790, 0.016);
        check(page2.dragging() && !side2.dragging(), "mostly vertical finger: the page tracker wins");
    }
}

int main() {
    test_params();
    test_drag();
    test_slop();
    test_slop_and_tap();
    test_press_delay();
    test_spline();
    test_impulse();
    test_lsq2();
    test_flick();
    test_clamps();
    test_stale();
    test_fling();
    test_swipe_events();
    test_fling_edges();
    test_catch();
    test_pinch();
    test_list_gestures();
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
