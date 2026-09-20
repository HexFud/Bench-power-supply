# Bench-power-supply

I designed a simple and cost effective bench power supply for everyone.
To simplify the assmebly process i designed in kicad a small pcb where you connect external components

 ![PCB 3D model](/media/power_pcb.png)

## Specifications
* IN Voltage: 24V DC
* Power Module: compatible with buck modules CC/CV based on XL4015 (5A) or XL4016 (8A)

## Included Protections
* 6.3A fusible
* schottky diode to protect against invers polarity
* filtering capacitor

## First power-up

1. Do not plug the Nano, the DPS and the displays yet.
2. Power the PCB with 24V and check the 12V rail after the LM2596 and the 3V3 rail.
3. Power off, plug everything in, and power on again with no load on the output.
4. The output stays OFF at boot; enable it with a long press on the voltage encoder.

## Firmware

* Arduino IDE, board "Arduino Nano ESP32"
* Libraries: Adafruit SSD1306, Adafruit GFX Library
* Firmware: `Firmware/bench_psu.ino`
* Register addresses and scales are constants at the top of the sketch: check them against
  the protocol document of your DPS module.

## ⚠️ Crucial Assembly Notes

1. **Modifying the Buck Converter:** 
   To mount the voltage and current controls on the front panel, you must physically desolder the original tiny blue trimmers from the purchased Buck module. Solder wires into those remaining through-holes and route them to your front-panel potentiometers.
2. **Display Wiring (CRITICAL - DO NOT BURN YOUR DISPLAY):** 
   When using the standard 5-wire Chinese dual meters (3 thin wires, 2 thick wires), **THE THIN BLACK WIRE MUST BE LEFT UNCONNECTED**. The meter derives its ground reference entirely through the thick black wire. Connecting both black wires creates a ground loop that will melt the thin wire instantly under load. (Connector `J8` on this PCB is purposely designed with only 3 pins for the thin wires).
3. **High-Current Traces:** 
   The PCB tracks handling the 24V input, the Buck module, and the outputs carry up to 5–8A. When routing the PCB, ensure power traces are made very wide (min. 2mm–3mm) or left exposed so you can reinforce them with solder.
