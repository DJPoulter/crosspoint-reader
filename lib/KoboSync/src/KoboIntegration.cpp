#include "KoboIntegration.h"

#include "KoboSyncActivity.h"
#include "KoboSyncSettingsActivity.h"
#include "KoboSyncTokenStore.h"

namespace KoboIntegration {

Activity* createSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onDone) {
  return new KoboSyncActivity(renderer, mappedInput, onDone);
}

Activity* createSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onBack) {
  return new KoboSyncSettingsActivity(renderer, mappedInput, onBack);
}

bool isEnabled() {
  return !KOBO_TOKEN_STORE.getToken().empty();
}

}  // namespace KoboIntegration
