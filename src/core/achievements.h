#pragma once
#include "common/types.h"

class StateWrapper;
class CDImage;

namespace Achievements {

#ifdef WITH_CHEEVOS

// Implemented in Host.
extern bool Reset();
extern bool DoState(StateWrapper& sw);
extern void GameChanged(const std::string& path, CDImage* image);

/// Returns true if features such as save states should be disabled.
extern bool IsChallengeModeActive();

extern void DisplayBlockedByChallengeModeMessage();

#else

// Make noops when compiling without cheevos.
static inline bool Reset()
{
  return true;
}
static inline bool DoState(StateWrapper& sw)
{
  return true;
}
static constexpr inline bool IsChallengeModeActive()
{
  return false;
}
static inline void DisplayBlockedByChallengeModeMessage() {}

#endif

} // namespace Achievements
