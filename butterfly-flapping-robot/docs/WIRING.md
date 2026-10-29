# Wiring Quick Reference

## Transmitter (ESP32-C3 Mini)
- Left joystick VRx -> GPIO4
- Left joystick VRy -> GPIO1
- Right joystick VRx -> GPIO0
- Right joystick VRy -> GPIO2
- Status LED: GPIO3 -> 220-330 ohm resistor -> LED anode; LED cathode -> GND
- Joystick VCC -> 3V3 only if the joystick module supports 3.3 V
- Joystick GND -> ESP32 GND

## Receiver (ESP32-C3 Mini)
- Left MG90S signal -> GPIO5
- Right MG90S signal -> GPIO6
- Status LED: GPIO3 -> 220-330 ohm resistor -> LED anode; LED cathode -> GND
- Both servo red wires -> external regulated 5 V
- Both servo brown/black wires -> external supply GND
- External supply GND -> receiver ESP32 GND

Never connect 5 V directly to ESP32 GPIO or ADC pins. Test servos with linkages disconnected.
