// C++ smoke test for Retro68 on the Mac Plus: global constructors, virtual calls,
// templates, lambdas, new/delete, std::array/algorithm, exceptions. Results go into a
// 'CPPT' block (read by check_cpp.lua) and on the screen.
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <array>
#include <algorithm>
#include <stdexcept>

static constexpr unsigned long MAGIC = 0x43505054UL;   // 'CPPT'

struct Block {
    unsigned long magic, version, notMagic;   //  0
    unsigned long state;                      // 12: 0 running, 1 done
    unsigned long passBits;                   // 16: bit per check below
    unsigned long nChecks;                    // 20
    long values[8];                           // 24: raw results, for debugging
};
static Block blk __attribute__((aligned(4)));

// 1. Global constructor must run before main().
struct Counter {
    int n;
    Counter() : n(42) {}
};
static Counter gCounter;

// 2. Virtual dispatch.
struct Shape {
    virtual ~Shape() {}
    virtual long area() const = 0;
};
struct Box : Shape {
    long w, h;
    Box(long w_, long h_) : w(w_), h(h_) {}
    long area() const override { return w * h; }
};
struct Tri : Shape {
    long b, h;
    Tri(long b_, long h_) : b(b_), h(h_) {}
    long area() const override { return b * h / 2; }
};

// 3. Templates.
template <typename T, int N>
T sum(const std::array<T, N> &a)
{
    T s = 0;
    for (const T &x : a) s += x;
    return s;
}

#ifndef NO_EXCEPTIONS
static long mayThrow(int x)
{
    if (x > 3)
        throw std::runtime_error("too big");
    return x;
}
#endif

int main()
{
    blk.magic = MAGIC; blk.version = 1; blk.notMagic = ~MAGIC;
    int c = 0;
    auto check = [&](bool ok, long v) {
        blk.values[c] = v;
        if (ok) blk.passBits |= 1UL << c;
        c++;
    };

    check(gCounter.n == 42, gCounter.n);                          // bit 0: static ctor

    Shape *s1 = new Box(6, 7);                                    // bit 1: new + virtual
    Shape *s2 = new Tri(10, 5);
    check(s1->area() + s2->area() == 67, s1->area() + s2->area());
    delete s1;
    delete s2;

    std::array<short, 5> a{{5, 3, 9, 1, 7}};                     // bit 2: template + std::array
    check(sum<short, 5>(a) == 25, sum<short, 5>(a));

    std::sort(a.begin(), a.end());                                // bit 3: <algorithm> + lambda
    int mult = 10;
    long folded = 0;
    std::for_each(a.begin(), a.end(), [&](short x) { folded = folded * mult + x; });
    check(folded == 13579, folded);

#ifndef NO_EXCEPTIONS
    long caught = 0;                                              // bit 4: exceptions
    try {
        caught = mayThrow(2);
        caught = mayThrow(9);
    } catch (const std::runtime_error &e) {
        caught = -1;
    }
    check(caught == -1, caught);
#endif
    blk.nChecks = c;

    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nullptr);
    InitCursor();
    Rect r = qd.screenBits.bounds;
    WindowPtr win = NewWindow(nullptr, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    MoveTo(r.left + 40, 60);
    DrawString("\pRetro68 C++ test");
    MoveTo(r.left + 40, 80);
    DrawString(blk.passBits == (1UL << c) - 1 ? "\pPASS: ctor, virtual, template, lambda (+ exceptions)"
                                              : "\pFAIL");
    blk.state = 1;

    EventRecord ev;
    for (;;) {
        SystemTask();
        GetNextEvent(everyEvent, &ev);
    }
}
