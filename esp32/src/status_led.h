#pragma once
#include <stdint.h>

// Onboard WS2812 on GPIO8.
//
//   boot / waiting for the pedal   white, slowly blinking
//   connected                      solid -- white until a switch is pressed,
//                                  then the colour of that switch
//   footswitch pressed             brief dip to dark, then the new colour
//   error                          red, solid
//   stopped                        off
//
// Non-blocking: everything is driven from tick().

namespace led {

enum class State : uint8_t { Off, Booting, Waiting, Connected, Error };

void begin();
void setState(State s);
/// Colour shown while connected, from the switch that was pressed last.
void setSwitchColor(uint32_t rgb);
/// Brief dip to dark, acknowledging a press.
void blip();
void tick();

}  // namespace led
