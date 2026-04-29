#ifndef ISS_h
#define ISS_h

#include <stdbool.h>

/** @brief Initialize resources
 * @return true on success, false on failure
 */
bool iss_init(void);

/** @brief Clean up resources */
void iss_destroy(void);

/** @brief The direction to switch spaces towards */
typedef enum {
    ISSDirectionLeft = 0,
    ISSDirectionRight = 1
} ISSDirection;

/**
 * @brief Describes the current space state for the active display.
 */
typedef struct {
    unsigned int currentIndex; /**< Zero-based index of the active space */
    unsigned int spaceCount;   /**< Total number of user-visible spaces */
} ISSSpaceInfo;

/**
 * @brief Performs the space switch if the requested move is within bounds.
 * @param direction The direction to switch spaces towards
 * @return true if the switch was posted, false if blocked by bounds or errors
 */
bool iss_switch(ISSDirection direction);

/**
 * @brief Retrieves the current space info for the display where the cursor is located.
 * @param info Output pointer that receives the info struct.
 * @return true on success, false if unavailable (e.g. API failure)
 */
bool iss_get_space_info(ISSSpaceInfo *info);

/**
 * @brief Retrieves the current space info for the active menu-bar display.
 * @param info Output pointer that receives the info struct.
 * @return true on success, false if unavailable (e.g. API failure)
 */
bool iss_get_menubar_space_info(ISSSpaceInfo *info);

/**
 * @brief Determines if a move in the given direction is allowed for the info.
 * @param info Space info snapshot.
 * @param direction Desired direction to move.
 * @return true if the move is permissible.
 */
bool iss_can_move(ISSSpaceInfo info, ISSDirection direction);

/**
 * @brief Attempts to switch directly to the provided space index.
 * @param targetIndex Zero-based index for the desired space.
 * @return true if the request succeeded (already on target or switches posted)
 */
bool iss_switch_to_index(unsigned int targetIndex);

/**
 * @brief Test-only mode that avoids private system calls and mutates an in-memory space state.
 */
void iss_testing_enable(void);
void iss_testing_disable(void);
bool iss_testing_set_space_state(unsigned int currentIndex, unsigned int spaceCount);
void iss_testing_set_gesture_options(bool loggingEnabled, bool completionEnabled);
void iss_testing_reset_gesture_state(void);
bool iss_testing_handle_gesture_event(int cgsType,
                                      int hidType,
                                      int phase,
                                      double progress,
                                      double velocityX,
                                      int flags,
                                      int motion,
                                      double timestamp);
unsigned int iss_testing_completion_count(void);
int iss_testing_gesture_state(void);

#endif /* ISS_h */
