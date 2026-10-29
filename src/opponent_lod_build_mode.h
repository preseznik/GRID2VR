#pragma once

#if defined(GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC) && defined(GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION)
#error Opponent LOD diagnostic and controlled activation are mutually exclusive.
#endif

namespace grid2vr::opponentlod::mode {
#ifdef GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC
inline constexpr bool Diagnostic=true;
#else
inline constexpr bool Diagnostic=false;
#endif
#ifdef GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION
inline constexpr bool ControlledActivation=true;
#else
inline constexpr bool ControlledActivation=false;
#endif
inline constexpr bool SingleAttachment=Diagnostic || ControlledActivation;
inline constexpr bool ScopedOwnership=Diagnostic || ControlledActivation;
}
