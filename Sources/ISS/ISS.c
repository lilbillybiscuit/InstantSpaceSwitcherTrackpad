#include "include/ISS.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <float.h>
#include <math.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureScrollY = (CGEventField)119;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;
static const CGEventField kCGEventScrollGestureFlagBits = (CGEventField)135;
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

typedef CF_ENUM(uint8_t, ISSGestureTrackingState) {
    ISSGestureStateIdle = 0,
    ISSGestureStateTrackingCandidate = 1,
    ISSGestureStateTrackingSpaceSwipe = 2,
    ISSGestureStateFinishing = 3,
    ISSGestureStateCooldown = 4,
};

typedef int32_t CGSConnectionID;
typedef uint64_t CGSSpaceID;

extern CFArrayRef CGSCopyManagedDisplaySpaces(CGSConnectionID connection, CFStringRef display) __attribute__((weak_import));
extern CFStringRef CGSCopyActiveMenuBarDisplayIdentifier(CGSConnectionID connection) __attribute__((weak_import));
extern CGSConnectionID CGSMainConnectionID(void) __attribute__((weak_import));
extern CGSSpaceID CGSGetActiveSpace(CGSConnectionID connection) __attribute__((weak_import));

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

static bool testingEnabled = false;
static ISSSpaceInfo testingSpaceInfo = {0, 0};

static bool gestureLoggingEnabled = false;
static bool gestureCompletionEnabled = false;
static os_log_t gestureLog = NULL;

static struct {
    ISSGestureTrackingState state;
    ISSDirection direction;
    double lastProgress;
    double lastVelocityX;
    double lastTimestamp;
    double cooldownUntil;
    double selfInjectionUntil;
    unsigned int completionCount;
} gestureTracker = {0};

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo);
static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay);
static void iss_load_gesture_options(void);
static void iss_reset_gesture_tracker(void);
static bool iss_handle_gesture_snapshot(CGSEventType cgsType,
                                        int64_t hidType,
                                        int64_t phase,
                                        double progress,
                                        double velocityX,
                                        double velocityY,
                                        int64_t flags,
                                        int64_t motion,
                                        double timestamp);
static bool iss_post_switch_gesture(ISSDirection direction);
static bool iss_post_finish_gesture(ISSDirection direction);
static void iss_post_gesture_completion_notification(unsigned int targetIndex);
static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction);
static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);

static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type, 
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (globalTap) {
            CGEventTapEnable(globalTap, true);
        }
        if (gestureLoggingEnabled && gestureLog) {
            os_log_info(gestureLog, "event tap re-enabled after disabled type=%d", type);
        }
        return event;
    }

    CGSEventType cgsType = (CGSEventType)CGEventGetIntegerValueField(event, kCGSEventTypeField);
    if (type == kCGEventScrollWheel ||
        cgsType == kCGSEventScrollWheel ||
        cgsType == kCGSEventZoom ||
        cgsType == kCGSEventGesture ||
        cgsType == kCGSEventDockControl ||
        cgsType == kCGSEventFluidTouchGesture) {
        bool handled = iss_handle_gesture_snapshot(
            cgsType,
            CGEventGetIntegerValueField(event, kCGEventGestureHIDType),
            CGEventGetIntegerValueField(event, kCGEventGesturePhase),
            CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress),
            CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX),
            CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityY),
            CGEventGetIntegerValueField(event, kCGEventScrollGestureFlagBits),
            CGEventGetIntegerValueField(event, kCGEventGestureSwipeMotion),
            CFAbsoluteTimeGetCurrent()
        );
        if (handled) {
            return NULL;
        }
    }

    return event;
}

static bool cgs_symbols_available(void) {
    return (&CGSMainConnectionID != NULL) &&
           (&CGSGetActiveSpace != NULL) &&
           (&CGSCopyManagedDisplaySpaces != NULL);
}

static bool env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value) {
        return false;
    }
    return !strcmp(value, "1") ||
           !strcasecmp(value, "true") ||
           !strcasecmp(value, "yes") ||
           !strcasecmp(value, "on");
}

static bool preferences_flag_enabled(CFStringRef key) {
    CFPropertyListRef value = CFPreferencesCopyAppValue(key, kCFPreferencesCurrentApplication);
    if (!value) {
        value = CFPreferencesCopyAppValue(key, CFSTR("com.interversehq.InstantSpaceSwitcher"));
    }

    bool enabled = false;
    if (value) {
        if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
            enabled = CFBooleanGetValue((CFBooleanRef)value);
        } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
            enabled = CFStringCompare((CFStringRef)value, CFSTR("true"), kCFCompareCaseInsensitive) == kCFCompareEqualTo ||
                      CFStringCompare((CFStringRef)value, CFSTR("1"), 0) == kCFCompareEqualTo ||
                      CFStringCompare((CFStringRef)value, CFSTR("yes"), kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        }
        CFRelease(value);
    }

    return enabled;
}

static void iss_load_gesture_options(void) {
    gestureLoggingEnabled = env_flag_enabled("ISS_GESTURE_LOGGING") ||
                            preferences_flag_enabled(CFSTR("ISSGestureLoggingEnabled"));
    gestureCompletionEnabled = env_flag_enabled("ISS_GESTURE_COMPLETION") ||
                               preferences_flag_enabled(CFSTR("ISSGestureCompletionEnabled"));
    if (!gestureLog) {
        gestureLog = os_log_create("com.interversehq.InstantSpaceSwitcher", "gesture");
    }
}

static void iss_reset_gesture_tracker(void) {
    memset(&gestureTracker, 0, sizeof(gestureTracker));
    gestureTracker.state = ISSGestureStateIdle;
    gestureTracker.direction = ISSDirectionLeft;
}

static const char *gesture_state_name(ISSGestureTrackingState state) {
    switch (state) {
    case ISSGestureStateIdle:
        return "idle";
    case ISSGestureStateTrackingCandidate:
        return "trackingCandidate";
    case ISSGestureStateTrackingSpaceSwipe:
        return "trackingSpaceSwipe";
    case ISSGestureStateFinishing:
        return "finishing";
    case ISSGestureStateCooldown:
        return "cooldown";
    }
    return "unknown";
}

static void gesture_transition(ISSGestureTrackingState state, double timestamp, const char *reason) {
    if (gestureTracker.state != state && gestureLoggingEnabled && gestureLog) {
        os_log_info(gestureLog, "state %{public}s -> %{public}s reason=%{public}s",
                    gesture_state_name(gestureTracker.state),
                    gesture_state_name(state),
                    reason);
    }
    gestureTracker.state = state;
    gestureTracker.lastTimestamp = timestamp;
}

static bool gesture_direction_from_snapshot(double progress,
                                            double velocityX,
                                            int64_t flags,
                                            ISSDirection *direction) {
    if (fabs(progress) >= 0.04) {
        *direction = progress > 0 ? ISSDirectionRight : ISSDirectionLeft;
        return true;
    }
    if (fabs(velocityX) >= 40.0) {
        *direction = velocityX > 0 ? ISSDirectionRight : ISSDirectionLeft;
        return true;
    }
    if (flags == 0 || flags == 1) {
        *direction = flags ? ISSDirectionRight : ISSDirectionLeft;
        return true;
    }
    return false;
}

static bool gesture_is_space_swipe_candidate(CGSEventType cgsType,
                                             int64_t hidType,
                                             int64_t motion,
                                             double progress,
                                             double velocityX) {
    if (cgsType == kCGSEventDockControl && hidType == kIOHIDEventTypeDockSwipe) {
        return motion == 0 || motion == kCGGestureMotionHorizontal || fabs(progress) > 0.0 || fabs(velocityX) > 0.0;
    }

    return false;
}

static bool gesture_is_neutral_interstitial(CGSEventType cgsType,
                                            int64_t hidType,
                                            int64_t phase,
                                            double progress,
                                            double velocityX,
                                            double velocityY,
                                            int64_t flags,
                                            int64_t motion) {
    return cgsType == kCGSEventGesture &&
           hidType == 0 &&
           phase == kCGSGesturePhaseNone &&
           fabs(progress) == 0.0 &&
           fabs(velocityX) == 0.0 &&
           fabs(velocityY) == 0.0 &&
           flags == 0 &&
           motion == 0;
}

static bool iss_handle_gesture_snapshot(CGSEventType cgsType,
                                        int64_t hidType,
                                        int64_t phase,
                                        double progress,
                                        double velocityX,
                                        double velocityY,
                                        int64_t flags,
                                        int64_t motion,
                                        double timestamp) {
    if (!gestureLoggingEnabled && !gestureCompletionEnabled) {
        return false;
    }

    bool candidate = gesture_is_space_swipe_candidate(cgsType, hidType, motion, progress, velocityX);
    ISSDirection direction = gestureTracker.direction;
    bool hasDirection = gesture_direction_from_snapshot(progress, velocityX, flags, &direction);
    bool neutralInterstitial = gesture_is_neutral_interstitial(cgsType,
                                                              hidType,
                                                              phase,
                                                              progress,
                                                              velocityX,
                                                              velocityY,
                                                              flags,
                                                              motion);

    if (neutralInterstitial && gestureTracker.state != ISSGestureStateIdle) {
        return false;
    }

    if (gestureLoggingEnabled && gestureLog && (candidate || gestureTracker.state != ISSGestureStateIdle)) {
        os_log_debug(gestureLog,
                     "event cgs=%u hid=%lld phase=%lld progress=%{public}.3f vx=%{public}.1f vy=%{public}.1f flags=%lld motion=%lld candidate=%{public}d dir=%{public}s state=%{public}s",
                     cgsType,
                     hidType,
                     phase,
                     progress,
                     velocityX,
                     velocityY,
                     flags,
                     motion,
                     candidate,
                     hasDirection ? (direction == ISSDirectionRight ? "right" : "left") : "unknown",
                     gesture_state_name(gestureTracker.state));
    }

    if (timestamp < gestureTracker.selfInjectionUntil) {
        return false;
    }

    if (gestureTracker.state == ISSGestureStateCooldown) {
        if (timestamp < gestureTracker.cooldownUntil) {
            return false;
        }
        gesture_transition(ISSGestureStateIdle, timestamp, "cooldownExpired");
    }

    if (!candidate) {
        if (gestureTracker.state == ISSGestureStateTrackingCandidate ||
            gestureTracker.state == ISSGestureStateTrackingSpaceSwipe) {
            gesture_transition(ISSGestureStateIdle, timestamp, "nonCandidate");
        }
        return false;
    }

    if (phase == kCGSGesturePhaseBegan || phase == kCGSGesturePhaseMayBegin) {
        gestureTracker.direction = hasDirection ? direction : gestureTracker.direction;
        gestureTracker.lastProgress = progress;
        gestureTracker.lastVelocityX = velocityX;
        gesture_transition(ISSGestureStateTrackingCandidate, timestamp, "began");
        return false;
    }

    if (phase == kCGSGesturePhaseChanged || phase == kCGSGesturePhaseNone) {
        if (hasDirection) {
            gestureTracker.direction = direction;
        }
        gestureTracker.lastProgress = progress;
        gestureTracker.lastVelocityX = velocityX;
        if (gestureTracker.state == ISSGestureStateIdle) {
            gesture_transition(ISSGestureStateTrackingCandidate, timestamp, "changedFromIdle");
        }
        if (hasDirection && (fabs(progress) >= 0.08 || fabs(velocityX) >= 80.0)) {
            gesture_transition(ISSGestureStateTrackingSpaceSwipe, timestamp, "stableDirection");
        }
        return false;
    }

    if (phase != kCGSGesturePhaseEnded && phase != kCGSGesturePhaseCancelled) {
        return false;
    }

    if (hasDirection) {
        gestureTracker.direction = direction;
    }

    double effectiveProgress = fabs(progress) >= fabs(gestureTracker.lastProgress) ? progress : gestureTracker.lastProgress;
    double absProgress = fabs(effectiveProgress);
    bool wasTracking = gestureTracker.state == ISSGestureStateTrackingCandidate ||
                       gestureTracker.state == ISSGestureStateTrackingSpaceSwipe;
    bool progressQualifies = absProgress >= 0.15 && absProgress < 1.20;

    if (!gestureCompletionEnabled || !wasTracking || !hasDirection || !progressQualifies) {
        gesture_transition(ISSGestureStateCooldown, timestamp, "endedNoCompletion");
        gestureTracker.cooldownUntil = timestamp + 0.15;
        return false;
    }

    ISSSpaceInfo info;
    bool canMove = iss_get_space_info(&info) && iss_can_move(info, gestureTracker.direction);
    if (!canMove) {
        gesture_transition(ISSGestureStateCooldown, timestamp, "blockedByBounds");
        gestureTracker.cooldownUntil = timestamp + 0.20;
        return false;
    }

    unsigned int targetIndex = info.currentIndex;
    if (gestureTracker.direction == ISSDirectionRight) {
        targetIndex++;
    } else {
        targetIndex--;
    }

    gestureTracker.selfInjectionUntil = timestamp + 0.08;
    gesture_transition(ISSGestureStateFinishing, timestamp, "injectFinish");
    bool posted = iss_post_finish_gesture(gestureTracker.direction);
    gestureTracker.completionCount += posted ? 1 : 0;
    if (posted) {
        iss_post_gesture_completion_notification(targetIndex);
    }
    gestureTracker.cooldownUntil = timestamp + (posted ? 0.08 : 0.20);
    gesture_transition(ISSGestureStateCooldown, timestamp, posted ? "postedFinish" : "postFailed");
    return posted;
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo) {
    if (!displayDict || !outInfo) {
        return false;
    }

    const void *spacesValue = CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
    if (!spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        return false;
    }

    // Try to get current space from display dict (more accurate per-display)
    CGSSpaceID displayActiveSpace = 0;
    const void *currentSpaceValue = CFDictionaryGetValue(displayDict, CFSTR("Current Space"));
    if (currentSpaceValue && CFGetTypeID(currentSpaceValue) == CFDictionaryGetTypeID()) {
        CFDictionaryRef currentSpaceDict = (CFDictionaryRef)currentSpaceValue;
        CFNumberRef currentSpaceID = (CFNumberRef)CFDictionaryGetValue(currentSpaceDict, CFSTR("id64"));
        if (currentSpaceID && CFGetTypeID(currentSpaceID) == CFNumberGetTypeID()) {
            CFNumberGetValue(currentSpaceID, kCFNumberSInt64Type, &displayActiveSpace);
        }
    }
    
    // Use display-specific active space if available, otherwise use global
    CGSSpaceID targetActiveSpace = displayActiveSpace != 0 ? displayActiveSpace : activeSpace;
    bool hasTargetActiveSpace = displayActiveSpace != 0 || hasActiveSpace;

    CFArrayRef spaces = (CFArrayRef)spacesValue;
    const CFIndex spaceCount = CFArrayGetCount(spaces);

    unsigned int totalSpaces = 0;
    unsigned int activeIndex = 0;
    bool foundActive = false;

    for (CFIndex i = 0; i < spaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }

        CGSSpaceID candidate = 0;
        if (CFNumberGetValue(idNumber, kCFNumberSInt64Type, &candidate)) {
            if (!foundActive && hasTargetActiveSpace && candidate == targetActiveSpace) {
                activeIndex = totalSpaces;
                foundActive = true;
            }
            totalSpaces++;
        }
    }

    if (totalSpaces == 0 || (hasTargetActiveSpace && !foundActive)) {
        return false;
    }

    outInfo->spaceCount = totalSpaces;
    outInfo->currentIndex = foundActive ? activeIndex : 0;
    return true;
}

static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay) {
    if (testingEnabled) {
        if (!info || testingSpaceInfo.spaceCount == 0) {
            return false;
        }
        *info = testingSpaceInfo;
        return true;
    }

    if (!cgs_symbols_available()) {
        fprintf(stderr, "ISS: required CGS symbols missing\n");
        return false;
    }

    CGSConnectionID connection = CGSMainConnectionID();
    if (connection == 0) {
        fprintf(stderr, "ISS: CGSMainConnectionID returned 0\n");
        return false;
    }

    CGSSpaceID activeSpace = 0;
    bool hasActiveSpace = false;
    if (&CGSGetActiveSpace != NULL) {
        activeSpace = CGSGetActiveSpace(connection);
        if (activeSpace != 0) {
            hasActiveSpace = true;
        } else {
            fprintf(stderr, "ISS: CGSGetActiveSpace returned 0\n");
            return false;
        }
    }

    // Get display identifier based on mode
    CFStringRef activeDisplayIdentifier = NULL;
    
    if (useCursorDisplay) {
        // Get display where cursor is located
        CGEventRef tempEvent = CGEventCreate(NULL);
        CGPoint cursorLocation = CGEventGetLocation(tempEvent);
        CFRelease(tempEvent);
        
        CGDirectDisplayID cursorDisplay = 0;
        uint32_t cursorDisplayCount = 0;
        
        if (CGGetDisplaysWithPoint(cursorLocation, 1, &cursorDisplay, &cursorDisplayCount) == kCGErrorSuccess && cursorDisplayCount > 0) {
            CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID(cursorDisplay);
            if (displayUUID) {
                activeDisplayIdentifier = CFUUIDCreateString(NULL, displayUUID);
                CFRelease(displayUUID);
            }
        }
    } else {
        // Get menubar display
        if (&CGSCopyActiveMenuBarDisplayIdentifier != NULL) {
            activeDisplayIdentifier = CGSCopyActiveMenuBarDisplayIdentifier(connection);
        }
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, activeDisplayIdentifier);
    if (!displays && activeDisplayIdentifier) {
        displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    }
    if (!displays) {
        if (activeDisplayIdentifier) {
            CFRelease(activeDisplayIdentifier);
        }
        return false;
    }

    const CFIndex displayCount = CFArrayGetCount(displays);
    CFDictionaryRef targetDisplay = NULL;
    CFDictionaryRef fallbackDisplay = NULL;

    for (CFIndex i = 0; i < displayCount; i++) {
        const void *displayValue = CFArrayGetValueAtIndex(displays, i);
        if (!displayValue || CFGetTypeID(displayValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef displayDict = (CFDictionaryRef)displayValue;

        if (!fallbackDisplay) {
            fallbackDisplay = displayDict;
        }

        if (!activeDisplayIdentifier || targetDisplay) {
            continue;
        }

        CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
        if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID() && CFEqual(identifier, activeDisplayIdentifier)) {
            targetDisplay = displayDict;
        }
    }

    if (!targetDisplay) {
        targetDisplay = fallbackDisplay;
    }

    bool success = false;
    if (targetDisplay) {
        success = extract_space_info_from_display(targetDisplay, activeSpace, hasActiveSpace, info);
    }

    if (activeDisplayIdentifier) {
        CFRelease(activeDisplayIdentifier);
    }
    CFRelease(displays);

    return success;
}

static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction) {
    if (!info) {
        return false;
    }
    if (info->spaceCount == 0) {
        return true;
    }

    if (direction == ISSDirectionLeft) {
        return info->currentIndex == 0;
    }

    return info->currentIndex + 1 >= info->spaceCount;
}

bool iss_can_move(ISSSpaceInfo info, ISSDirection direction) {
    return !iss_should_block_switch(&info, direction);
}

static bool iss_post_switch_gesture(ISSDirection direction) {
    if (testingEnabled) {
        if (iss_should_block_switch(&testingSpaceInfo, direction)) {
            return false;
        }
        if (direction == ISSDirectionRight) {
            testingSpaceInfo.currentIndex++;
        } else {
            testingSpaceInfo.currentIndex--;
        }
        return true;
    }

    const bool isRight = (direction == ISSDirectionRight);
    gestureTracker.selfInjectionUntil = CFAbsoluteTimeGetCurrent() + 0.08;

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
    if (!evA) {
        return false;
    }
    CGEventSetIntegerValueField(evA, kCGSEventTypeField, kCGSEventGesture);

    CGEventRef evB = CGEventCreate(NULL);
    if (!evB) {
        CFRelease(evA);
        return false;
    }
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
    if (!evA) {
        return false;
    }
    CGEventSetIntegerValueField(evA, kCGSEventTypeField, kCGSEventGesture);

    evB = CGEventCreate(NULL);
    if (!evB) {
        CFRelease(evA);
        return false;
    }
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

    return true;
}

static bool iss_post_finish_gesture(ISSDirection direction) {
    return iss_post_switch_gesture(direction);
}

static void iss_post_gesture_completion_notification(unsigned int targetIndex) {
    CFNumberRef targetIndexValue = CFNumberCreate(NULL, kCFNumberIntType, &targetIndex);
    if (!targetIndexValue) {
        return;
    }

    const void *keys[] = { CFSTR("targetIndex") };
    const void *values[] = { targetIndexValue };
    CFDictionaryRef userInfo = CFDictionaryCreate(NULL,
                                                  keys,
                                                  values,
                                                  1,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    CFRelease(targetIndexValue);
    if (!userInfo) {
        return;
    }

    CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(),
                                         CFSTR("com.interversehq.InstantSpaceSwitcher.gestureDidComplete"),
                                         NULL,
                                         userInfo,
                                         true);
    CFRelease(userInfo);
}

bool iss_init(void) {
    if (globalTap) {
        return true;
    }

    iss_load_gesture_options();
    iss_reset_gesture_tracker();

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) |
                       CGEventMaskBit(kCGEventKeyUp) |
                       CGEventMaskBit(kCGEventScrollWheel) |
                       CGEventMaskBit(kCGSEventZoom) |
                       CGEventMaskBit(kCGSEventGesture) |
                       CGEventMaskBit(kCGSEventDockControl) |
                       CGEventMaskBit(kCGSEventFluidTouchGesture);
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
    iss_reset_gesture_tracker();
}

bool iss_get_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, true);
}

bool iss_get_menubar_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, false);
}

static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction) {
    if (iss_should_block_switch(info, direction)) {
        return false;
    }
    if (!iss_post_switch_gesture(direction)) {
        return false;
    }

    return true;
}

bool iss_switch(ISSDirection direction) {
    ISSSpaceInfo info;
    if (iss_get_space_info(&info)) {
        return iss_switch_with_info(&info, direction);
    }

    return iss_post_switch_gesture(direction);
}

bool iss_switch_to_index(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }

    if (info.spaceCount == 0) {
        return false;
    }

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    if (info.currentIndex == targetIndex) {
        return !outOfBounds;
    }

    ISSDirection direction = info.currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight ? (targetIndex - info.currentIndex) : (info.currentIndex - targetIndex);

    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_post_switch_gesture(direction)) {
            return false;
        }
    }

    return !outOfBounds;
}

void iss_testing_enable(void) {
    testingEnabled = true;
    testingSpaceInfo.currentIndex = 0;
    testingSpaceInfo.spaceCount = 1;
    gestureLoggingEnabled = false;
    gestureCompletionEnabled = false;
    iss_reset_gesture_tracker();
}

void iss_testing_disable(void) {
    testingEnabled = false;
    testingSpaceInfo.currentIndex = 0;
    testingSpaceInfo.spaceCount = 0;
    gestureLoggingEnabled = false;
    gestureCompletionEnabled = false;
    iss_reset_gesture_tracker();
}

bool iss_testing_set_space_state(unsigned int currentIndex, unsigned int spaceCount) {
    if (!testingEnabled || spaceCount == 0 || currentIndex >= spaceCount) {
        return false;
    }
    testingSpaceInfo.currentIndex = currentIndex;
    testingSpaceInfo.spaceCount = spaceCount;
    return true;
}

void iss_testing_set_gesture_options(bool loggingEnabled, bool completionEnabled) {
    gestureLoggingEnabled = loggingEnabled;
    gestureCompletionEnabled = completionEnabled;
    if (!gestureLog) {
        gestureLog = os_log_create("com.interversehq.InstantSpaceSwitcher", "gesture");
    }
}

void iss_testing_reset_gesture_state(void) {
    iss_reset_gesture_tracker();
}

bool iss_testing_handle_gesture_event(int cgsType,
                                      int hidType,
                                      int phase,
                                      double progress,
                                      double velocityX,
                                      int flags,
                                      int motion,
                                      double timestamp) {
    return iss_handle_gesture_snapshot((CGSEventType)cgsType,
                                       hidType,
                                       phase,
                                       progress,
                                       velocityX,
                                       0,
                                       flags,
                                       motion,
                                       timestamp);
}

unsigned int iss_testing_completion_count(void) {
    return gestureTracker.completionCount;
}

int iss_testing_gesture_state(void) {
    return gestureTracker.state;
}
