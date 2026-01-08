#pragma once

#include <HardwareSerial.h>

#include <string>
#include <utility>

class MappedInputManager;
class GfxRenderer;

class Activity {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  std::string name;
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), name(std::move(name)) {}
  virtual ~Activity() = default;
  virtual void onEnter() { Serial.printf("[%lu] [ACT] Entering activity: %s\n", millis(), name.c_str()); }
  virtual void onExit() { Serial.printf("[%lu] [ACT] Exiting activity: %s\n", millis(), name.c_str()); }
  virtual void loop() {}
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  const std::string& getName() const { return name; }
  // Get the effective activity name (subactivity name if available, otherwise activity name)
  virtual std::string getEffectiveActivityName() const { return name; }
};
