#ifndef ISS_h
#define ISS_h

#include <stdbool.h>

/** @brief Initialize the event tap
 * @return true on success, false on failure
 */
bool iss_init(void);

/** @brief Tear down the event tap */
void iss_destroy(void);

/** @brief The direction to switch spaces towards */
typedef enum {
    ISSDirectionLeft = 0,
    ISSDirectionRight = 1
} ISSDirection;

/** @brief Performs the space switch
 * @param direction The direction to switch spaces towards
 */
void iss_switch(ISSDirection direction);

#endif /* ISS_h */
