// Unit tests of the GSAP 2.1.3 port (src/engine/gsap.*). Scenario-level fidelity is checked against the
// original game frame by frame (build/run_traces.sh); these pin down the core semantics.
#include <cmath>
#include <cstdio>

#include "engine/gsap.h"

using namespace gsap;

static int g_fail = 0, g_checks = 0;

static void check(bool ok, const char *what, double got, double want)
{
    g_checks++;
    if (!ok) {
        std::printf("FAIL %s: got %.6f want %.6f\n", what, got, want);
        g_fail++;
    }
}
#define NEAR(got, want, tol, what) check(std::fabs(double(got) - double(want)) <= (tol), what, got, want)

static void frames(Engine &e, int n)
{
    for (int i = 0; i < n; i++) e.tick();
}

int main()
{
    // eases (GSAP 2 Ease.getRatio)
    NEAR(Power1Out().getRatio(0.5), 0.75, 1e-12, "Power1.easeOut(0.5)");
    NEAR(Power1In().getRatio(0.5), 0.25, 1e-12, "Power1.easeIn(0.5)");
    NEAR(Power2InOut().getRatio(0.75), 0.9375, 1e-12, "Power2.easeInOut(0.75)");
    NEAR(BounceOut().getRatio(1.0), 1.0, 1e-12, "Bounce.easeOut(1)");

    {
        // a tween created between frames starts at the root's last time; its first (lazy) render lands in the
        // next frame, and it completes exactly after duration
        Engine e;
        cr::Vec3 v;
        int done = 0;
        Vars o;
        o.withEase(Ease::make(2, 0)); // Linear
        o.onComplete = [&] { done++; };
        e.to(&v, {{'x', 10}}, 0.5, o);
        frames(e, 15);
        NEAR(v.x, 5, 1e-4, "linear tween halfway after 15 frames");
        frames(e, 15);
        NEAR(v.x, 10, 1e-6, "linear tween at the end");
        check(done == 1, "onComplete once", done, 1);
    }
    {
        // timeline children run in sequence; the timeline's onComplete fires once at the end
        Engine e;
        cr::Vec3 s{1, 1, 1};
        int done = 0;
        Vars tv;
        tv.onComplete = [&] { done++; };
        auto tl = e.timeline(tv);
        tl->to(&s, {{'y', 1.2}}, 0.1).to(&s, {{'y', 0.8}}, 0.1).to(&s, {{'y', 1}}, 0.1, Vars().withEase(BounceOut()));
        frames(e, 6);
        NEAR(s.y, 1.2, 1e-4, "segment 1 end");
        frames(e, 6);
        NEAR(s.y, 0.8, 1e-4, "segment 2 end");
        frames(e, 8);
        NEAR(s.y, 1.0, 1e-4, "timeline end");
        check(done == 1, "timeline onComplete once", done, 1);
    }
    {
        // TimelineMax repeat -1: cycles restart from the recorded start values
        Engine e;
        cr::Vec3 s{1, 1, 1};
        Vars rv;
        rv.repeat = -1;
        auto tl = e.timeline(rv);
        tl->to(&s, {{'y', 0.8}}, 0.3, Vars().withEase(Power1In())).to(&s, {{'y', 1}}, 0.3, Vars().withEase(Power1Out()));
        frames(e, 18);
        NEAR(s.y, 0.8, 2e-3, "idle down");
        frames(e, 18);
        NEAR(s.y, 1.0, 2e-3, "idle up");
        frames(e, 9);
        NEAR(s.y, 1.0 - 0.2 * 0.25, 5e-3, "second cycle restarts from 1.0");
    }
    {
        // overwrite auto: a newer tween takes over the overlapping property; the older tween keeps the others
        Engine e;
        cr::Vec3 v;
        Vars lin;
        lin.withEase(Ease::make(2, 0));
        e.to(&v, {{'x', 10}, {'y', 10}}, 1.0, lin);
        frames(e, 30);
        e.to(&v, {{'x', -10}}, 0.5, lin);
        frames(e, 31);
        NEAR(v.x, -10, 1e-3, "overwritten property follows the newer tween");
        NEAR(v.y, 10, 0.5, "other property keeps the older tween (y ends near 10)");
    }
    {
        // a tween whose every property gets overwritten is disabled: its onComplete never fires
        Engine e;
        cr::Vec3 v;
        int done = 0;
        Vars a;
        a.onComplete = [&] { done++; };
        e.to(&v, {{'x', 10}}, 0.5, a);
        frames(e, 5);
        e.to(&v, {{'x', 3}}, 0.2);
        frames(e, 60);
        check(done == 0, "fully overwritten tween never completes", done, 0);
        NEAR(v.x, 3, 1e-6, "value from the overwriting tween");
    }
    {
        // a timeline whose only running child is killed by overwrite shrinks and completes early
        Engine e;
        cr::Vec3 v;
        int done = 0;
        Vars tv;
        tv.onComplete = [&] { done++; };
        auto tl = e.timeline(tv);
        tl->to(&v, {{'z', 5}}, 1.0);
        frames(e, 6);
        e.to(&v, {{'z', -5}}, 0.2);
        frames(e, 3);
        check(done == 1, "emptied timeline fired onComplete early", done, 1);
    }
    {
        // bezier cubic ends on the last anchor
        Engine e;
        cr::Vec3 p;
        const cr::Vec3 values[4] = {{0, 0, 0}, {1, 2, 0}, {2, 2, 0}, {3, 0, 1}};
        e.bezierTo(&p, values, 0.5);
        frames(e, 31);
        NEAR(p.x, 3, 1e-5, "bezier end x");
        NEAR(p.y, 0, 1e-5, "bezier end y");
        NEAR(p.z, 1, 1e-5, "bezier end z");
    }
    {
        // paused tweens stop and never complete
        Engine e;
        cr::Vec3 v;
        int done = 0;
        Vars a;
        a.onComplete = [&] { done++; };
        auto t = e.to(&v, {{'x', 10}}, 0.5, a);
        frames(e, 10);
        double at = v.x;
        t->pause();
        frames(e, 60);
        NEAR(v.x, at, 1e-9, "paused tween holds its value");
        check(done == 0, "paused tween never completes", done, 0);
    }
    std::printf("test_gsap: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
