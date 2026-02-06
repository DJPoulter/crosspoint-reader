#pragma once

#include <functional>

// Forward declarations so the main app does not need to include activity headers
class Activity;
class GfxRenderer;
class MappedInputManager;

namespace KoboIntegration {

/** Create the Kobo sync activity (handshake, fetch list, download EPUBs). Caller owns the returned pointer. */
Activity* createSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onDone);

/** Create the Kobo sync settings activity (edit sync token). Caller owns the returned pointer. */
Activity* createSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onBack);

/** True when Kobo sync is enabled (sync token is configured). */
bool isEnabled();

}  // namespace KoboIntegration
