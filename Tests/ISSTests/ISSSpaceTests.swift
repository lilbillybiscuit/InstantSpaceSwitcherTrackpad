import XCTest
import ISS

final class ISSSpaceTests: XCTestCase {
    private let dockControl = 30
    private let gesture = 29
    private let dockSwipeHID = 23
    private let phaseNone = 0
    private let phaseBegan = 1
    private let phaseChanged = 2
    private let phaseEnded = 4
    private let phaseCancelled = 8
    private let horizontalMotion = 1
    private let stateCooldown: Int32 = 4

    override func setUp() {
        super.setUp()
        iss_testing_enable()
        XCTAssertTrue(iss_testing_set_space_state(1, 3))
        iss_testing_set_gesture_options(false, false)
        iss_testing_reset_gesture_state()
    }

    override func tearDown() {
        iss_testing_disable()
        super.tearDown()
    }

    func testCanMoveReportsAvailableDirections() throws {
        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertTrue(iss_can_move(info, ISSDirectionLeft))
        XCTAssertTrue(iss_can_move(info, ISSDirectionRight))

        XCTAssertTrue(iss_switch(ISSDirectionLeft))
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertFalse(iss_can_move(info, ISSDirectionLeft))
        XCTAssertTrue(iss_can_move(info, ISSDirectionRight))
    }

    func testSwitchRespectsBounds() {
        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))

        XCTAssertTrue(iss_switch(ISSDirectionLeft))
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 0)

        XCTAssertFalse(iss_switch(ISSDirectionLeft))
        XCTAssertTrue(iss_switch(ISSDirectionRight))
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 1)
    }

    func testSwitchToIndexTargetsExpectedSpace() {
        XCTAssertTrue(iss_switch_to_index(2))
        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 2)

        XCTAssertFalse(iss_switch_to_index(5))
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 2)
    }

    func testGestureCompletionDisabledOnlyTracksAndCoolsDown() {
        iss_testing_set_gesture_options(true, false)

        XCTAssertFalse(gestureEvent(phase: phaseBegan, progress: 0.0, velocityX: 0.0, timestamp: 1.0))
        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: 0.45, velocityX: 120.0, timestamp: 1.1))
        XCTAssertFalse(gestureEvent(phase: phaseEnded, progress: 0.55, velocityX: 120.0, timestamp: 1.2))

        XCTAssertEqual(iss_testing_completion_count(), 0)
        XCTAssertEqual(iss_testing_gesture_state(), stateCooldown)
    }

    func testGestureEndCompletesOnceWhenProgressQualifies() {
        iss_testing_set_gesture_options(false, true)

        XCTAssertFalse(gestureEvent(phase: phaseBegan, progress: 0.0, velocityX: 0.0, timestamp: 2.0))
        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: 0.40, velocityX: 140.0, timestamp: 2.1))
        XCTAssertTrue(gestureEvent(phase: phaseEnded, progress: 0.55, velocityX: 180.0, timestamp: 2.2))

        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 2)
        XCTAssertEqual(iss_testing_completion_count(), 1)

        XCTAssertFalse(gestureEvent(phase: phaseEnded, progress: 0.55, velocityX: 180.0, timestamp: 2.25))
        XCTAssertEqual(iss_testing_completion_count(), 1)

        XCTAssertFalse(gestureEvent(phase: phaseBegan, progress: 0.0, velocityX: 0.0, flags: 0, timestamp: 2.31))
        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: -0.25, velocityX: -100.0, flags: 0, timestamp: 2.32))
        XCTAssertTrue(gestureEvent(phase: phaseEnded, progress: -0.25, velocityX: -100.0, flags: 0, timestamp: 2.33))
        XCTAssertEqual(iss_testing_completion_count(), 2)
    }

    func testGestureDoesNotCompleteForTinyOrAmbiguousProgress() {
        iss_testing_set_gesture_options(false, true)

        XCTAssertFalse(gestureEvent(phase: phaseBegan, progress: 0.0, velocityX: 0.0, flags: 4, timestamp: 3.0))
        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: 0.03, velocityX: 10.0, flags: 4, timestamp: 3.1))
        XCTAssertFalse(gestureEvent(phase: phaseCancelled, progress: 0.04, velocityX: 10.0, flags: 4, timestamp: 3.2))

        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 1)
        XCTAssertEqual(iss_testing_completion_count(), 0)
    }

    func testGestureCompletionRespectsSpaceBounds() {
        XCTAssertTrue(iss_testing_set_space_state(2, 3))
        iss_testing_set_gesture_options(false, true)

        XCTAssertFalse(gestureEvent(phase: phaseBegan, progress: 0.0, velocityX: 0.0, timestamp: 4.0))
        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: 0.50, velocityX: 140.0, timestamp: 4.1))
        XCTAssertFalse(gestureEvent(phase: phaseEnded, progress: 0.60, velocityX: 180.0, timestamp: 4.2))

        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 2)
        XCTAssertEqual(iss_testing_completion_count(), 0)
    }

    func testNeutralGestureInterstitialsDoNotResetTracking() {
        iss_testing_set_gesture_options(false, true)

        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: 0.10, velocityX: 0.0, timestamp: 5.0))
        XCTAssertFalse(neutralInterstitial(timestamp: 5.01))
        XCTAssertFalse(gestureEvent(phase: phaseChanged, progress: 0.28, velocityX: 0.0, timestamp: 5.02))
        XCTAssertFalse(neutralInterstitial(timestamp: 5.03))
        XCTAssertTrue(gestureEvent(phase: phaseEnded, progress: 0.28, velocityX: -3.1, timestamp: 5.04))

        var info = ISSSpaceInfo()
        XCTAssertTrue(iss_get_space_info(&info))
        XCTAssertEqual(info.currentIndex, 2)
        XCTAssertEqual(iss_testing_completion_count(), 1)
    }

    private func gestureEvent(
        phase: Int,
        progress: Double,
        velocityX: Double,
        flags: Int = 1,
        timestamp: Double
    ) -> Bool {
        iss_testing_handle_gesture_event(
            Int32(dockControl),
            Int32(dockSwipeHID),
            Int32(phase),
            progress,
            velocityX,
            Int32(flags),
            Int32(horizontalMotion),
            timestamp
        )
    }

    private func neutralInterstitial(timestamp: Double) -> Bool {
        iss_testing_handle_gesture_event(
            Int32(gesture),
            0,
            Int32(phaseNone),
            0.0,
            0.0,
            0,
            0,
            timestamp
        )
    }
}
