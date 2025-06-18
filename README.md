# tpmix

tpmix is a GUI Control Program for Topping Audio Interfaces' Mixer, for Linux, maybe MacOS X. Please do not file tickets for windoze.


## Supported HW

- Topping E4X4 (152a:8754), tested
- E2x2, HID tags are quite different, wip
- Larger devices support is possible, please capture the HID interaction, see [topping.e4x4.hid.md]


## Features

### Works

- Input, Mix, Loopback, Output, Phone Control
- Electronic switchs (Phone gain, 48V, mon, inst) status bi-directional sync
- Status auto save and load, in ~/.config/toppingmixer/toppingmixer.settings
- All level meters. Inputs, Playbacks, Loopbacks input and output, Outputs input and output
- Gain step can be finer than 1dB, it's digital, (linearly) can be almost any value between (-64.0,-64.0)
- manual load/save, and save to device

### Works not

- Restoring 48V phantom power. For device safety reason, code commented out
- Save/load Solo switches. See toppingmixer.settings and below

### TODOs

- Peak indicator for all level meters
- Reset timer for level meter, the device stops updating if signal lower than -96dBfs
- push all saved status to the device


## Design

### HW Signal path

```
48V, inst >-+                          +--> [Input buses]
            |                          |
    +-------+                          |       +--< [Playbacks]
    |                                  |       |
[Inputs] --> <HW Gain> -> Input Gain --+--> Mix Gain --> Mix Pan --> [Mix A/B/C/D]
                                       |
                                       +--> MON Switch --> [MON Bus]



                                                +--> LINE sw --> [Line Out]
                                                |
[Mix/Input Buses] --> Selection --> [Outputs] --+-- MON SW -> mixer --> <HW Vol> --> <Gain SW> --> Phones
                                                                ^
                                                                |                                                        
[MON Bus] ------------------------------------------------------+



[Mix/Input Buses] -> Selection --> Volume [Loopbacks]


```

Notes:

- **Inputs** include IN1..IN4, PLAYBACK1..PLAYBACK8
- **Input buses** include IN{1..4} mono; IN1+2, IN3+4 stereo; PLAYBACK{1+2..7+8} stereo
- **MON Bus** seems to be mono
- **Outputs** is 2ch x 2
- **Mix Buses** is 2ch x 4
- **Mute** is signal multiplied by 0 scaler
- **Solo** is just mute other signals
- **Phase** is signal multiplied by a negative scaler
- **Inst** control is Hi-Z and unbalanced input

###  SW deps

- wxWidgets 3.2 or later, tested with wx3.2  (older might also work)
- HIDAPI 0.14 or later (older might also work)
- g++ 14.2, with C++20 features, (older might also work)


### `toppingmixer.settings`

format: lines of printf format ("%04x %08x\n", id16, val32)

Example:

```
2101 00000000
2103 00000000
2105 02000000
2201 00000000
2203 00000000
2205 02000000
.
.
.
```

- **id16** is borrowed from Topping HID protocol, `21__` means Input CH 1. `2_01` means **MON**, `2_02` means **48V**, `2_03` means **INST**, `2_05` means **Gain**, `2_04` means level meter, etc
- **val32** is also  borrowed from same protocol. for switchs 1 means on, 0 means off, otherwise `int32_t` value. Gain 0x02000000 is 0dB. when input is pan center, gain left/right is 0x01000000/0x01000000
- **Solo** is actually controlled by muting other channels
- **Mute** is actually 0 digital gain
- This file can be edited manualy, for debug / experiment
- Borrowing HID protocol drastically simplified SW design

## Known issues:

1. **segmentation fault** when exit, due to unknown string free violation of `PanelOutputs` class. wxWidgets related
2. **Mute** is saved, but Gain/Volume is not saved if muted.
3. **Solo** status is not saved. because Solo is actually muting other channels.
4. udev rules for hidraw does not support VID:PID, so, manually chmod/chown the hidraw device, or assign your group for all hidraw dev (kind of unsafe)
5. Not really an issue, if HID device open failed, there will be a demo mode, just some flashing level meters, and pan/gain/volume will be remembered.
6. occasionally crash when open, debugging

## License

GNU General Public License v3.0 or later

