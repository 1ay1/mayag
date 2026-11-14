#pragma once
// mayag/platform/macos_loop.hpp — Cocoa owns the main thread; jaal runs inside it.
//
// AppKit has to own the main thread's run loop: live resize, menu tracking
// and window dragging spin their own nested loops, and anything that waits
// elsewhere freezes during them. jaal's kernel never owns the thread (jaal
// D4), so on macOS the run loop drives the program (app.hpp embedded_app)
// from run loop sources, all in kCFRunLoopCommonModes so they keep firing
// inside those nested loops:
//
//   wake source    signalled from any thread by the kernel (a task or stream
//                  posted). CFRunLoopSourceSignal + CFRunLoopWakeUp are
//                  thread safe; an atomic flag coalesces bursts.
//   timer          one CFRunLoopTimer, re-armed to jaal's next deadline
//                  after every turn.
//   display link   CADisplayLink (macOS 14+) from the content view: vsync
//                  aligned, ProMotion aware, and paused unless something is
//                  animating. Older systems get a 60 Hz run loop timer.
//   signals        jaal's signal pipe, watched with a CFFileDescriptor.
//   notifications  window resize / backing-scale change, so a live resize
//                  redraws while AppKit is tracking it.
//
// Input arrives the plain way: the loop blocks in nextEventMatchingMask:
// (which runs the run loop and so services everything above), feeds each
// event to the window, lets AppKit have it with sendEvent:, then turns the
// program. Idle, the thread sleeps in mach_msg: no polling, 0% CPU.
//
// CoreFoundation is declared by hand for the same reason macos_window.hpp
// does it: Apple's headers need the blocks extension, which GCC lacks.

#include "macos_window.hpp"

#include <jaal/jaal.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

extern "C" {
typedef const void*                    CFTypeRef;
typedef const struct __CFString*       CFStringRef;
typedef const struct __CFAllocator*    CFAllocatorRef;
typedef struct __CFRunLoop*            CFRunLoopRef;
typedef struct __CFRunLoopSource*      CFRunLoopSourceRef;
typedef struct __CFRunLoopTimer*       CFRunLoopTimerRef;
typedef struct __CFFileDescriptor*     CFFileDescriptorRef;
typedef double                         CFAbsoluteTime;
typedef double                         CFTimeInterval;
typedef long                           CFIndex;
typedef unsigned long                  CFOptionFlags;
typedef unsigned long                  CFHashCode;
typedef unsigned char                  Boolean;

typedef struct {
    CFIndex     version;
    void*       info;
    const void* (*retain)(const void*);
    void        (*release)(const void*);
    CFStringRef (*copyDescription)(const void*);
    Boolean     (*equal)(const void*, const void*);
    CFHashCode  (*hash)(const void*);
    void        (*schedule)(void*, CFRunLoopRef, CFStringRef);
    void        (*cancel)(void*, CFRunLoopRef, CFStringRef);
    void        (*perform)(void*);
} CFRunLoopSourceContext;

typedef struct {
    CFIndex     version;
    void*       info;
    const void* (*retain)(const void*);
    void        (*release)(const void*);
    CFStringRef (*copyDescription)(const void*);
} CFRunLoopTimerContext;

typedef struct {
    CFIndex     version;
    void*       info;
    void*       (*retain)(void*);
    void        (*release)(void*);
    CFStringRef (*copyDescription)(void*);
} CFFileDescriptorContext;

typedef void (*CFRunLoopTimerCallBack)(CFRunLoopTimerRef, void*);
typedef void (*CFFileDescriptorCallBack)(CFFileDescriptorRef, CFOptionFlags, void*);

extern const CFStringRef kCFRunLoopCommonModes;

CFRunLoopRef       CFRunLoopGetMain(void);
void               CFRunLoopWakeUp(CFRunLoopRef);
CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef, CFIndex, CFRunLoopSourceContext*);
void               CFRunLoopSourceSignal(CFRunLoopSourceRef);
void               CFRunLoopSourceInvalidate(CFRunLoopSourceRef);
void               CFRunLoopAddSource(CFRunLoopRef, CFRunLoopSourceRef, CFStringRef);
CFRunLoopTimerRef  CFRunLoopTimerCreate(CFAllocatorRef, CFAbsoluteTime, CFTimeInterval,
                                        CFOptionFlags, CFIndex, CFRunLoopTimerCallBack,
                                        CFRunLoopTimerContext*);
void               CFRunLoopTimerSetNextFireDate(CFRunLoopTimerRef, CFAbsoluteTime);
void               CFRunLoopTimerInvalidate(CFRunLoopTimerRef);
void               CFRunLoopAddTimer(CFRunLoopRef, CFRunLoopTimerRef, CFStringRef);
CFAbsoluteTime     CFAbsoluteTimeGetCurrent(void);
CFFileDescriptorRef CFFileDescriptorCreate(CFAllocatorRef, int, Boolean, CFFileDescriptorCallBack,
                                           const CFFileDescriptorContext*);
void               CFFileDescriptorEnableCallBacks(CFFileDescriptorRef, CFOptionFlags);
void               CFFileDescriptorInvalidate(CFFileDescriptorRef);
CFRunLoopSourceRef CFFileDescriptorCreateRunLoopSource(CFAllocatorRef, CFFileDescriptorRef, CFIndex);
void               CFRelease(CFTypeRef);

// Exported by libobjc; what @autoreleasepool compiles to.
void* objc_autoreleasePoolPush(void);
void  objc_autoreleasePoolPop(void*);
}

namespace mayag::platform::cocoa {

/// The main run loop, set up to drive one program. Main thread only, except
/// wake(), which any thread may call.
class RunLoop {
  public:
    std::function<void()>       on_turn;      // fold, draw, re-arm
    std::function<void(double)> on_vsync;     // one display refresh, dt seconds
    std::function<void(int)>    on_signal;    // a jaal::sig, as int

    explicit RunLoop(MacWindow& w) : win_(w), rl_(CFRunLoopGetMain()) {
        // Wake source.
        CFRunLoopSourceContext sc{};
        sc.info = this;
        sc.perform = [](void* self) { static_cast<RunLoop*>(self)->woken(); };
        wake_ = CFRunLoopSourceCreate(nullptr, 0, &sc);
        CFRunLoopAddSource(rl_, wake_, kCFRunLoopCommonModes);

        // jaal deadline timer. Repeating with a huge interval so it stays
        // valid after it fires; every turn moves its fire date.
        CFRunLoopTimerContext tc{};
        tc.info = this;
        timer_ = CFRunLoopTimerCreate(nullptr, far_future(), kForever, 0, 0,
            [](CFRunLoopTimerRef, void* self) { static_cast<RunLoop*>(self)->turn(); }, &tc);
        CFRunLoopAddTimer(rl_, timer_, kCFRunLoopCommonModes);

        make_target();
        observe_window();
        make_display_link();
    }

    RunLoop(const RunLoop&)            = delete;
    RunLoop& operator=(const RunLoop&) = delete;

    ~RunLoop() {
        using namespace objc;
        if (link_ != nullptr) {
            msg<void>(link_, sel("invalidate"));
            link_ = nullptr;
        }
        if (target_ != nullptr) {
            id center = msg_cls<id>(objc_getClass("NSNotificationCenter"), sel("defaultCenter"));
            msg<void>(center, sel("removeObserver:"), target_);
            set_owner(target_, nullptr);
            msg<void>(target_, sel("release"));
        }
        if (fallback_ != nullptr) { CFRunLoopTimerInvalidate(fallback_); CFRelease(fallback_); }
        if (sig_src_ != nullptr) { CFRunLoopSourceInvalidate(sig_src_); CFRelease(sig_src_); }
        if (sig_fd_ != nullptr) { CFFileDescriptorInvalidate(sig_fd_); CFRelease(sig_fd_); }
        CFRunLoopTimerInvalidate(timer_);
        CFRelease(timer_);
        CFRunLoopSourceInvalidate(wake_);
        CFRelease(wake_);
    }

    // ── what embedded_app calls ─────────────────────────────────────────

    /// Any thread. Coalesced: one signal per burst until the loop runs it.
    void wake() noexcept {
        if (pending_.exchange(true, std::memory_order_acq_rel)) return;
        CFRunLoopSourceSignal(wake_);
        CFRunLoopWakeUp(rl_);
    }

    void arm(std::optional<std::chrono::nanoseconds> d) {
        CFRunLoopTimerSetNextFireDate(timer_, d ? CFAbsoluteTimeGetCurrent() +
                                                      std::chrono::duration<double>(*d).count()
                                                : far_future());
    }

    void frames(bool on) {
        using namespace objc;
        last_vsync_.reset();
        if (link_ != nullptr) {
            msg<void>(link_, sel("setPaused:"), static_cast<BOOL>(on ? NO : YES));
        } else if (fallback_ != nullptr) {
            CFRunLoopTimerSetNextFireDate(fallback_, on ? CFAbsoluteTimeGetCurrent() : far_future());
        }
    }

    /// The program is done: leave run() after the current callback.
    void stop() {
        using namespace objc;
        stopped_ = true;
        // nextEventMatchingMask: only returns for an event; post an empty one.
        id ev = msg_cls<id>(objc_getClass("NSEvent"),
            sel("otherEventWithType:location:modifierFlags:timestamp:windowNumber:"
                "context:subtype:data1:data2:"),
            static_cast<unsigned long>(15) /* ApplicationDefined */, CGPoint{0, 0},
            static_cast<unsigned long>(0), 0.0, static_cast<long>(0), static_cast<id>(nullptr),
            static_cast<short>(0), static_cast<long>(0), static_cast<long>(0));
        msg<void>(win_.ns_app(), sel("postEvent:atStart:"), ev, static_cast<BOOL>(YES));
    }

    /// Watch jaal's signal pipe (SIGINT, SIGTERM, ...).
    void watch_signals(int fd) {
        CFFileDescriptorContext fc{};
        fc.info = this;
        sig_fd_ = CFFileDescriptorCreate(nullptr, fd, false,
            [](CFFileDescriptorRef f, CFOptionFlags, void* self) {
                auto* rl = static_cast<RunLoop*>(self);
                if (rl->on_signal) rl->on_signal(-1);              // "drain the pipe"
                CFFileDescriptorEnableCallBacks(f, kReadCallBack);  // one-shot by design
            }, &fc);
        CFFileDescriptorEnableCallBacks(sig_fd_, kReadCallBack);
        sig_src_ = CFFileDescriptorCreateRunLoopSource(nullptr, sig_fd_, 0);
        CFRunLoopAddSource(rl_, sig_src_, kCFRunLoopCommonModes);
    }

    /// Block in AppKit until stop(). Everything else happens in callbacks.
    void run() {
        using namespace objc;
        id app = win_.ns_app();
        id distant = msg_cls<id>(objc_getClass("NSDate"), sel("distantFuture"));
        id mode = nsstring("kCFRunLoopDefaultMode");
        msg<void>(distant, sel("retain"));
        msg<void>(mode, sel("retain"));
        while (!stopped_) {
            void* pool = objc_autoreleasePoolPush();
            id ev = msg<id>(app, sel("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                            any_event_mask, distant, mode, static_cast<BOOL>(YES));
            if (ev != nullptr && !stopped_) {
                win_.feed(ev);
                msg<void>(app, sel("sendEvent:"), ev);
                turn();
            }
            objc_autoreleasePoolPop(pool);
        }
        msg<void>(mode, sel("release"));
        msg<void>(distant, sel("release"));
    }

  private:
    static constexpr CFTimeInterval kForever = 1.0e10;
    static constexpr CFOptionFlags  kReadCallBack = 1;   // kCFFileDescriptorReadCallBack

    static CFAbsoluteTime far_future() { return CFAbsoluteTimeGetCurrent() + kForever; }

    void woken() {
        pending_.store(false, std::memory_order_release);   // before the turn: a
        turn();                                             // later wake re-signals
    }
    void turn() { if (!stopped_ && on_turn) on_turn(); }

    void vsync(double timestamp) {
        if (stopped_ || !on_vsync) return;
        const double dt = last_vsync_ ? timestamp - *last_vsync_ : 1.0 / 60.0;
        last_vsync_ = timestamp;
        on_vsync(dt);
    }

    // ── the Objective-C target for the display link and notifications ──
    //
    // Built once at runtime: NSObject subclass with an `owner` ivar pointing
    // back here, and two methods. No .mm file, mayag stays header-only.

    static void tick_imp(id self, SEL, id link) {
        if (auto* rl = owner(self)) {
            rl->vsync(objc::msg<double>(link, objc::sel("timestamp")));
        }
    }
    static void changed_imp(id self, SEL, id) {
        if (auto* rl = owner(self)) rl->turn();
    }
    static void fallback_imp(CFRunLoopTimerRef, void* self) {
        auto* rl = static_cast<RunLoop*>(self);
        rl->vsync(CFAbsoluteTimeGetCurrent());
    }

    static Class target_class() {
        static Class cls = [] {
            if (Class existing = objc_getClass("MayagRunLoopTarget")) return existing;
            Class c = objc_allocateClassPair(objc_getClass("NSObject"), "MayagRunLoopTarget", 0);
            class_addIvar(c, "mayag_owner", sizeof(void*), 3 /* log2 align */, "^v");
            class_addMethod(c, objc::sel("mayagTick:"), reinterpret_cast<IMP>(&tick_imp), "v@:@");
            class_addMethod(c, objc::sel("mayagChanged:"), reinterpret_cast<IMP>(&changed_imp), "v@:@");
            objc_registerClassPair(c);
            return c;
        }();
        return cls;
    }
    static std::ptrdiff_t owner_offset() {
        static const std::ptrdiff_t off =
            ivar_getOffset(class_getInstanceVariable(target_class(), "mayag_owner"));
        return off;
    }
    static RunLoop* owner(id obj) {
        return *reinterpret_cast<RunLoop**>(reinterpret_cast<char*>(obj) + owner_offset());
    }
    static void set_owner(id obj, RunLoop* rl) {
        *reinterpret_cast<RunLoop**>(reinterpret_cast<char*>(obj) + owner_offset()) = rl;
    }

    void make_target() {
        using namespace objc;
        target_ = msg<id>(msg_cls<id>(target_class(), sel("alloc")), sel("init"));
        set_owner(target_, this);
    }

    void observe_window() {
        using namespace objc;
        id center = msg_cls<id>(objc_getClass("NSNotificationCenter"), sel("defaultCenter"));
        for (const char* name : {"NSWindowDidResizeNotification",
                                 "NSWindowDidChangeBackingPropertiesNotification",
                                 "NSWindowDidChangeScreenNotification"}) {
            msg<void>(center, sel("addObserver:selector:name:object:"), target_,
                      sel("mayagChanged:"), nsstring(name), win_.ns_window());
        }
    }

    void make_display_link() {
        using namespace objc;
        id view = win_.ns_view();
        const SEL make = sel("displayLinkWithTarget:selector:");
        if (msg<BOOL>(view, sel("respondsToSelector:"), make)) {
            // macOS 14+: vsync of whatever display the view is on, follows it
            // across screens, and runs at ProMotion rates when asked.
            link_ = msg<id>(view, make, target_, sel("mayagTick:"));
            msg<void>(link_, sel("setPaused:"), static_cast<BOOL>(YES));
            id main = msg_cls<id>(objc_getClass("NSRunLoop"), sel("mainRunLoop"));
            msg<void>(link_, sel("addToRunLoop:forMode:"), main,
                      reinterpret_cast<id>(const_cast<void*>(static_cast<const void*>(kCFRunLoopCommonModes))));
        } else {
            CFRunLoopTimerContext tc{};
            tc.info = this;
            fallback_ = CFRunLoopTimerCreate(nullptr, far_future(), 1.0 / 60.0, 0, 0,
                                             &fallback_imp, &tc);
            CFRunLoopAddTimer(rl_, fallback_, kCFRunLoopCommonModes);
        }
    }

    MacWindow&               win_;
    CFRunLoopRef             rl_ = nullptr;
    CFRunLoopSourceRef       wake_ = nullptr;
    CFRunLoopTimerRef        timer_ = nullptr;
    CFRunLoopTimerRef        fallback_ = nullptr;
    CFFileDescriptorRef      sig_fd_ = nullptr;
    CFRunLoopSourceRef       sig_src_ = nullptr;
    id                       target_ = nullptr;
    id                       link_ = nullptr;
    std::atomic<bool>        pending_{false};
    bool                     stopped_ = false;
    std::optional<double>    last_vsync_;
};

}  // namespace mayag::platform::cocoa
