# arduino-midi-gadgets

Collection of MIDI-capable gadgets built for UNO/ATTiny85/RP2350 microcontrollers.

```bash
alias compile="arduino-cli compile -b ATTinyCore:avr:attinyx5micr --output-dir build/attiny85"
alias compile="arduino-cli compile -b arduino:avr:uno --output-dir build/uno"
alias arduino-cli compile -b rp2040:rp2040:waveshare_rp2350b_plus_w \
  --output-dir build/rp2350 \
  --build-property "compiler.cpp.extra_flags=-Iq/q_lib/include -Iq/infra/include" .
```

```bash
alias test="wokwi-cli --scenario test-scenario.yml"
test
```
