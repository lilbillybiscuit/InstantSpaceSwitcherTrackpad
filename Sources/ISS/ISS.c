#include "ISS.h"
#include "include/ISS.h"
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <float.h>
#include <stdio.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureScrollY = (CGEventField)119;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;
static const CGEventField kCGEventGestureSwipeMask = (CGEventField)134;
static const CGEventField kCGEventScrollGestureFlagBits = (CGEventField)135;
static const CGEventField kCGEventSwipeGestureFlagBits = (CGEventField)136;
static const CGEventField kCGEventGestureZoomDeltaX = (CGEventField)139;

// See IOHIDEventType enum in IOHIDFamily
static const uint32_t kIOHIDEventTypeDockSwipe = 23;

typedef uint32_t CGSEventType;
enum {
    kCGSEventScrollWheel = 22,
    kCGSEventZoom = 28,
    kCGSEventGesture = 29,
    kCGSEventDockControl = 30,
    kCGSEventFluidTouchGesture = 31,
};

typedef CF_ENUM(uint8_t, CGSGesturePhase) {
    kCGSGesturePhaseNone = 0,
    kCGSGesturePhaseBegan = 1,
    kCGSGesturePhaseChanged = 2,
    kCGSGesturePhaseEnded = 4,
    kCGSGesturePhaseCancelled = 8,
    kCGSGesturePhaseMayBegin = 128,
};

// Limited subset of motion constants observed in synthetic Dock swipe traces.
typedef CF_ENUM(uint16_t, CGGestureMotion) {
    kCGGestureMotionHorizontal = 1,
};

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

// Event tap callback (required but can be empty)
static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type, 
                                   CGEventRef event, void *refcon) {
    return event;
}

bool iss_init(void) {
    if (globalTap) return true;

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);
    globalTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        eventTapCallback,
        NULL
    );

    if (!globalTap) {
        return false;
    }

    globalSource = CFMachPortCreateRunLoopSource(NULL, globalTap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
    CGEventTapEnable(globalTap, true);

    return true;
}

void iss_destroy(void) {
    if (globalTap) {
        CGEventTapEnable(globalTap, false);
        if (globalSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
            CFRelease(globalSource);
            globalSource = NULL;
        }
        CFRelease(globalTap);
        globalTap = NULL;
    }
}

void iss_switch(ISSDirection direction) {
    // direction: 0 = left, 1 = right
    const bool isRight = (direction == ISSDirectionRight);

    // ScrollGestureFlagBits seem to mark direction (anything non-zero)
    int32_t scrollGestureFlagDirection = isRight ? 1 : 0;

    // Corresponds to distance, or something along those lines
    const double swipeProgress = isRight ? 2.0 : -2.0;

    // self-explanatory
    const double swipeVelocity = isRight ? 400.0 : -400.0;

    //
    // -- Begin gesture --
    //
    CGEventRef evA = CGEventCreate(NULL);
    CGEventSetIntegerValueField(evA, kCGSEventTypeField, kCGSEventGesture);

    CGEventRef evB = CGEventCreate(NULL);
    CGEventSetIntegerValueField(evB, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(evB, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(evB, kCGEventGesturePhase, kCGSGesturePhaseBegan);
    CGEventSetIntegerValueField(evB, kCGEventScrollGestureFlagBits, scrollGestureFlagDirection);
    CGEventSetIntegerValueField(evB, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(evB, kCGEventGestureScrollY, 0);
    // Cannot explain this
    CGEventSetDoubleValueField(evB, kCGEventGestureZoomDeltaX, FLT_TRUE_MIN);

    CGEventPost(kCGSessionEventTap, evB);
    CGEventPost(kCGSessionEventTap, evA);
    CFRelease(evA);
    CFRelease(evB);

    //
    // -- End gesture --
    //
    evA = CGEventCreate(NULL);
    CGEventSetIntegerValueField(evA, kCGSEventTypeField, kCGSEventGesture);

    evB = CGEventCreate(NULL);
    CGEventSetIntegerValueField(evB, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(evB, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(evB, kCGEventGesturePhase, kCGSGesturePhaseEnded);
    CGEventSetDoubleValueField(evB, kCGEventGestureSwipeProgress, swipeProgress);
    CGEventSetIntegerValueField(evB, kCGEventScrollGestureFlagBits, scrollGestureFlagDirection);
    CGEventSetIntegerValueField(evB, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(evB, kCGEventGestureScrollY, 0);
    CGEventSetDoubleValueField(evB, kCGEventGestureSwipeVelocityX, swipeVelocity);
    CGEventSetDoubleValueField(evB, kCGEventGestureSwipeVelocityY, 0);
    // Cannot explain this
    CGEventSetDoubleValueField(evB, kCGEventGestureZoomDeltaX, FLT_TRUE_MIN);

    CGEventPost(kCGSessionEventTap, evB);
    CGEventPost(kCGSessionEventTap, evA);
    CFRelease(evA);
    CFRelease(evB);
}
