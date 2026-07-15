/*
    tpmix, a Topping Audio Interface GUI Controler
    Copyright (C) 2025  terrance hendrik

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <cstdint>
#include <cstdlib>
#include <format>

#include <filesystem>
#include <map>
#include <set>
#include <thread>
#include <tuple>

#include <wx/checkbox.h>
#include <wx/gbsizer.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/tglbtn.h>
#include <wx/wx.h>

#include <inttypes.h>
#include <math.h>

#include <hidapi/hidapi.h>

void write32BE(uint8_t *buf, int32_t v32) {
  buf[0] = (v32 >> 24) & 0xff;
  buf[1] = (v32 >> 16) & 0xff;
  buf[2] = (v32 >> 8) & 0xff;
  buf[3] = (v32) & 0xff;
}

uint16_t read16BE(uint8_t *buf) {
  uint16_t v = 0;
  v = buf[0];
  v <<= 8;
  v |= buf[1];
  return v;
}

uint32_t read32BE(uint8_t *buf) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < 4; i++) {
    v <<= 8;
    v |= buf[i];
  }
  return v;
}

void printBuf8(const uint8_t *buf, size_t n,
               const char *prefix = (const char *)"") {
  printf(prefix);
  for (size_t i = 0; i < n; ++i) {
    if ((i != 0) && (0 == i % 4)) {
      printf(" ");
    }
    printf("%02x ", buf[i]);
  }
  printf("\n");
}

class Gain {
protected:
  const static int32_t POS_RANGE = 36;
  const static int32_t NEG_RANGE = 90;
  int32_t dBpos[POS_RANGE + 1];
  int32_t dBneg[NEG_RANGE + 1];

public:
  Gain() { generateDBs(); };

  void generateDBs() {
    for (int32_t d = 0; d <= POS_RANGE; d++) {
      dBpos[d] = 0x01000000 * pow(10.0, (double)d / 20.0);
    }
    for (int32_t d = 0; d <= NEG_RANGE; d++) {
      dBneg[d] = 0x01000000 * pow(10.0, -(double)d / 20.0);
    }
    dBneg[NEG_RANGE] = 0;
  };

  int32_t getDBscale(int32_t dB) {
    int32_t r = 0;
    if (dB > POS_RANGE) {
      r = dBpos[POS_RANGE];
    } else if (dB >= 0) {
      r = dBpos[dB];
    } else if (dB > -NEG_RANGE) {
      r = dBneg[-dB];
    } else {
      r = 0;
    }
    return r;
  };

  int32_t getMonoGain(int32_t dBGain, bool mute, bool solo, bool anySolo,
                      bool phase) {
    int32_t gain = 0;
    if ((mute) || ((!solo) && anySolo)) {
      gain = 0;
    } else {
      gain = getDBscale(dBGain) * 2;
      if (phase) {
        gain = -gain;
      }
    }
    return gain;
  };

  /**
   * @param percentPan: -100..100, -100 for full left, 100 for full right
   */
  std::tuple<int32_t, int32_t> getStereoGain(int32_t dBGain, bool mute,
                                             bool solo, bool anySolo,
                                             bool phase, int32_t percentPan) {
    int64_t gainL = 0, gainR = 0;
    int32_t gain = getMonoGain(dBGain, mute, solo, anySolo, phase);
    int32_t percentRight = (percentPan + 100) / 2;
    gainL = (int64_t)gain * (100 - percentRight) / 100;
    gainR = (int64_t)gain * percentRight / 100;
    return {(int32_t)gainL, (int32_t)gainR};
  };
};

class ToppingHID {
public:
  uint16_t vid = 0x152a;
  uint16_t pid = 0x8754;
  int32_t numInputs = 4;
  std::map<uint16_t, int32_t> settings;
  ToppingHID() {
    uint16_t pids[] = {0x8755, 0x8756, 0x8752, 0x8754};
    for (uint16_t p : pids) {
      handle = hid_open(vid, p, NULL);
      if (NULL != handle) {
        pid = p;
        break;
      }
    }
    if (NULL == handle) {
      printf("HID open failed (tried 0x8755, 0x8756, 0x8752, 0x8754)!\n");
    } else {
      if (pid == 0x8755) {
        numInputs = 1;
      } else if (pid == 0x8752 || pid == 0x8756) {
        numInputs = 2;
      } else {
        numInputs = 4;
      }
    }
    prepareBuf();
  };

  void initializeSettingsWithDefaults() {
    // 1. Inputs (ch 0..3)
    for (int i = 0; i < 4; ++i) {
      uint16_t base = 0x2100 + (i << 8);
      if (!settings.contains(base + 1))
        settings[base + 1] = 0; // MON
      if (!settings.contains(base + 2))
        settings[base + 2] = 0; // 48V
      if (!settings.contains(base + 3))
        settings[base + 3] = 0; // INST
      if (!settings.contains(base + 5))
        settings[base + 5] = 0x02000000; // Gain (0 dB)
    }

    // 2. Mixers (bus 0..3, src 0..11)
    for (int bus = 0; bus < 4; ++bus) {
      for (int src = 0; src < 12; ++src) {
        uint16_t keyL = ((0x61 + bus * 2) << 8) | (src + 1);
        uint16_t keyR = ((0x62 + bus * 2) << 8) | (src + 1);
        int32_t defaultL = (src % 2 == 0) ? 0x02000000 : 0;
        int32_t defaultR = (src % 2 == 1) ? 0x02000000 : 0;
        if (!settings.contains(keyL))
          settings[keyL] = defaultL;
        if (!settings.contains(keyR))
          settings[keyR] = defaultR;
      }
    }

    // 3. Loopbacks (ch 0..5)
    for (int i = 0; i < 6; ++i) {
      uint16_t key = ((0x51 + i) << 8) | 3;
      if (!settings.contains(key))
        settings[key] = 0x02000000; // Loopback Vol (0 dB)
    }
    // Loopback source select
    for (int i = 0; i < 3; ++i) {
      uint16_t key = ((0x57 + i) << 8) | 1;
      if (!settings.contains(key))
        settings[key] = 1;
    }

    // 4. Outputs (ch 0..3)
    for (int i = 0; i < 4; ++i) {
      uint16_t key = ((0x31 + i) << 8) | 3;
      if (!settings.contains(key))
        settings[key] = 0x02000000; // Output Vol (0 dB)
    }
    // Output select for phones (ch 4..5)
    for (int i = 0; i < 2; ++i) {
      uint16_t key = ((0x35 + i) << 8) | 1;
      if (!settings.contains(key))
        settings[key] = 7; // Default source 7 (Playback 1+2)

      uint16_t keyGain = ((0x35 + i) << 8) | 2;
      if (!settings.contains(keyGain))
        settings[keyGain] = 0; // Phone gain boost off
    }

    // 5. Device Settings
    if (!settings.contains(0x3901))
      settings[0x3901] = 1; // Auto Standby default: ON
    if (!settings.contains(0x3a01))
      settings[0x3a01] = 1; // OTG Mode default: ON
  }

  void readInfo() {
    int res = 0;
    const static size_t MAX_STR = 256;
    wchar_t wstr[MAX_STR];
    res = hid_get_manufacturer_string(handle, wstr, MAX_STR - 1);
    printf("Manufacturer String: %ls\n", wstr);

    res = hid_get_product_string(handle, wstr, MAX_STR);
    printf("Product String: %ls\n", wstr);

    res = hid_get_serial_number_string(handle, wstr, MAX_STR);
    printf("Serial Number String: %ls\n", wstr);
    (void)res;
  }

  // @param: ch 0..3, logic channel 1..4
  // @param: gain 0x80000000..0x7fffffff, in fixedpoint
  void setInputGainiI32(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x05;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };
  // @param: ch 0..3, logic channel 1..4
  // @param: on: 0/1, 1 turn on phantom power
  void setInput48V(int16_t ch, bool pOn, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x02;
    int32_t on32 = pOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };

  // @param: ch 0..3, logic channel 1..4
  // @param: on: 0/1, 1 turn on phantom power
  void setInputMon(int16_t ch, bool MonOn, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x01;
    int32_t on32 = MonOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };

  // @param: ch 0..3, logic channel 1..4
  // @param: on: 0/1, 1 turn on Instrument/Hi-Z
  void setInputInst(int16_t ch, bool InstOn, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x03;
    int32_t on32 = InstOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };

  // @param: bus 0..3, for Bus A..D
  // @param: src 0..3, for inour 1..4, 4..b: for playback 1..8
  void setMixVol(int32_t bus, int32_t src, int32_t gainL, int32_t gainR,
                 bool exec = true) {
    int32_t l = bus * 2;
    int32_t r = l + 1;

    buf[5] = 0x61 + l;
    buf[6] = 1 + src;
    write32BE(&buf[7], gainL);
    enqueue(exec);

    buf[5] = 0x61 + r;
    write32BE(&buf[7], gainR);
    enqueue(exec);
  };

  // @param: ch 0..1, logic phone 1, 2
  // @param: sel 1..14: 1..4: input 1..4; 5,6: input 1/2, 3/4; 7..10, play 1/2
  // ..7/8,  11..14: Mix A..D
  void setLoopSel(int16_t ch, int32_t sel, bool exec = true) {
    buf[5] = 0x57 + ch;
    buf[6] = 0x01;
    write32BE(&buf[7], sel);
    enqueue(exec);
  };

  // @param: ch 0..1, logic phone 1, 2
  // @param: gain 0..1, 0: 0dB, 1: +17dB
  void setLoopVol(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x51 + ch;
    buf[6] = 0x03;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };

  // @param: ch 0..1, logic phone 1, 2
  // @param: sel 1..14: 1..4: input 1..4; 5,6: input 1/2, 3/4; 7..10, play 1/2
  // ..7/8,  11..14: Mix A..D
  void setOutputSel(int16_t ch, int32_t sel, bool exec = true) {
    buf[5] = 0x35 + ch;
    buf[6] = 0x01;
    write32BE(&buf[7], sel);
    enqueue(exec);
  };

  // @param: ch 0..1, logic phone 1, 2
  // @param: gain 0..1, 0: 0dB, 1: +17dB
  void setOutputVol(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x31 + ch;
    buf[6] = 0x03;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };

  // @param: ch 0..1, logic output 1, 2
  // @param: on 0 1
  void setOutputMon(int16_t ch, bool on, bool exec = true) {
    buf[5] = 0x37;
    buf[6] = 0x01 + ch;
    write32BE(&buf[7], on ? 1 : 0);
    enqueue(exec);
  };

  // @param: ch 0..1, logic output 1, 2
  // @param: on 0,1
  void setOutputLine(int16_t ch, bool on, bool exec = true) {
    buf[5] = 0x37;
    buf[6] = 0x03 + ch;
    write32BE(&buf[7], on ? 1 : 0);
    enqueue(exec);
  };

  // @param: ch 0..1, logic phone 1, 2
  // @param: mix -100..100, -100 full input, 100 full playback
  void setPhoneMix(int16_t ch, int32_t mix, bool exec = true) {
    buf[5] = 0x35 + ch;
    buf[6] = 0x03;
    write32BE(&buf[7], (mix + 100) / 2);
    enqueue(exec);
  };

  // @param: ch 0..1, logic phone 1, 2
  // @param: gain 0..1, 0: 0dB, 1: +17dB
  void setPhoneGainBoost(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x35 + ch;
    buf[6] = 0x02;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };

  void saveDeviceDefault() {
    buf[5] = 0x11;
    buf[6] = 0x06;
    write32BE(&buf[7], 1);
    enqueue(true, false);
  };

  void requestDeviceDump() {
    buf[5] = 0x11;
    buf[6] = 0x05;
    write32BE(&buf[7], 1);
    enqueue(true, false);
  };

  void setDeviceSetting(uint8_t zone, uint8_t ctrl, int32_t value,
                        bool exec = true) {
    buf[5] = zone;
    buf[6] = ctrl;
    write32BE(&buf[7], value);
    enqueue(exec);
  };

  void pushAllSettingsToDevice() {
    for (const auto &[key, value] : settings) {
      uint8_t zone = key >> 8;
      uint8_t ctrl = key & 0xFF;

      // Skip level meters (read-only)
      if (ctrl == 0x04 && zone >= 0x21 && zone <= 0x24)
        continue;
      if ((ctrl == 0x01 || ctrl == 0x02) && zone >= 0x31 && zone <= 0x34)
        continue;

      // For E1x2, skip inactive input channels
      if (numInputs == 1 && zone >= 0x22 && zone <= 0x24)
        continue;
      if (numInputs == 2 && zone >= 0x23 && zone <= 0x24)
        continue;

      buf[5] = zone;
      buf[6] = ctrl;
      write32BE(&buf[7], value);
      enqueue(true, false);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };

  uint16_t calculate_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
      crc ^= data[i];
      for (int j = 0; j < 8; ++j) {
        if (crc & 1) {
          crc = (crc >> 1) ^ 0xA001;
        } else {
          crc >>= 1;
        }
      }
    }
    return crc ^ 0x785A;
  }

  void enqueue(bool exec = true, bool toSave = true) {
    int res = 0;
    if (pid == 0x8752 || pid == 0x8755 || pid == 0x8756) {
      uint16_t crc = calculate_crc16(buf, 11);
      buf[11] = (crc >> 8) & 0xFF;
      buf[12] = crc & 0xFF;
    } else {
      buf[11] = 0x00;
      buf[12] = 0x00;
    }
    // TODO queue
    // printBuf8(buf, 16, ">> ");
    if (NULL != handle && exec) {
      res = hid_write(handle, buf, 16);
    }
    if (toSave) {
      settings[read16BE(&buf[5])] = read32BE(&buf[7]);
    }
    (void)res;
  };

  hid_device *getHandle() { return handle; }
  ~ToppingHID() {
    if (NULL != handle) {
      hid_close(handle);
      handle = NULL;
    }
  }

protected:
  uint8_t buf[16];

  const uint8_t header[5] = {0x22, 0x33, 0x20, 0x01, 0x01};
  const uint8_t tail[5] = {0x00, 0x00, 0x66, 0x77, 0x00};
  hid_device *handle;

  void prepareBuf() {
    memcpy(&buf[0], header, 5);
    memcpy(&buf[11], tail, 5);
  };
};

struct GdkForceX11 {
  GdkForceX11() { setenv("GDK_BACKEND", "x11", 1); }
} gdk_force_x11;

class MyApp : public wxApp {
public:
  bool OnInit() override;
  int OnExit() override {
    hid_exit();
    return wxApp::OnExit();
  }
};

wxIMPLEMENT_APP(MyApp);

// wx ID range <= 32767
enum {
  ID_LOAD = 0x0001,
  ID_SAVE = 0x0010,
  ID_DEVICE_SAVE = 0x0020,

  ID_INPUT_GAIN = 0x220,
  ID_INPUT_48V = 0x240,
  ID_INPUT_MON = 0x250,
  ID_INPUT_INST = 0x260,
  ID_INPUT_SOLO = 0x270,
  ID_INPUT_MUTE = 0x280,
  ID_INPUT_PHASE = 0x290,
  ID_INPUT_PEAK = 0x2a0,

  ID_MIX_BUS_SEL = 0x610,
  ID_MIX_VOL = 0x620,
  ID_MIX_VOL_B = 0x630,
  ID_MIX_PAN = 0x640,
  ID_MIX_SOLO = 0x670,
  ID_MIX_MUTE = 0x680,
  ID_MIX_PHASE = 0x690,

  ID_OUTPUT_SEL = 0x310,
  ID_OUTPUT_VOL_L = 0x320,
  ID_OUTPUT_VOL_B = 0x330,
  ID_OUTPUT_VOL_R = 0x340,
  ID_OUTPUT_MON = 0x350,
  ID_OUTPUT_LINE = 0x360,

  ID_LOOP_SEL = 0x510,
  ID_LOOP_VOL_L = 0x520,
  ID_LOOP_VOL_B = 0x530,
  ID_LOOP_VOL_R = 0x540,
  ID_LOOP_MUTE = 0x550,

  ID_PHONE_MIX = 0x370,
  ID_PHONE_GAIN = 0x380,
};

class PanelInputs : public wxPanel {
public:
  const static int32_t N_INPUTS = 4;
  int32_t activeInputs;
  // Unit is 0.1dB
  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  // const static double LEVEL_UNIT = 0.1;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;

  const static int32_t minGain = -60;
  const static int32_t maxGain = 30; // for the int32_t ,  NEVER exceed 36!

  wxStaticText *lbTitleI[N_INPUTS];
  wxStaticText *lbGainI[N_INPUTS];
  wxSlider *slGainI[N_INPUTS];
  wxGauge *gaLevels[N_INPUTS];

  wxToggleButton *cb48V[N_INPUTS];
  wxToggleButton *cbMon[N_INPUTS];
  wxToggleButton *cbInst[N_INPUTS];
  wxToggleButton *cbSolo[N_INPUTS];
  wxToggleButton *cbMute[N_INPUTS];
  wxToggleButton *cbPhase[N_INPUTS];
  wxButton *lbPeaksI[N_INPUTS];
  int32_t PeaksI[N_INPUTS];

  PanelInputs(wxWindow *parent, int32_t activeInputs = 4, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY), activeInputs(activeInputs) {
    const auto MARGIN = FromDIP(8);

    for (size_t i = 0; i < N_INPUTS; i++) {
      PeaksI[i] = LEVEL_MIN;
      cbInst[i] = nullptr;
    }
    // 3 columns per channel, control, gain, level
    auto sizer = new wxGridBagSizer(MARGIN, MARGIN);

    // titles
    for (size_t iCh = 0; iCh < N_INPUTS; iCh++) {
      lbTitleI[iCh] =
          new wxStaticText(this, wxID_ANY, std::format("Input{}", iCh + 1));
      lbTitleI[iCh]->SetWindowStyle(wxALIGN_CENTER);
      wxFont font = lbTitleI[iCh]->GetFont();
      lbTitleI[iCh]->SetFont(font.Larger().Larger());

      if (iCh < activeInputs) {
        sizer->Add(lbTitleI[iCh], wxGBPosition(0, iCh * 3), wxGBSpan(1, 3),
                   wxALIGN_CENTER);
      } else {
        lbTitleI[iCh]->Hide();
      }
    }
    // peak level, with reset
    for (size_t iCh = 0; iCh < N_INPUTS; iCh++) {
      lbPeaksI[iCh] = new wxButton(this, ID_INPUT_PEAK + iCh, "-120.0",
                                   wxDefaultPosition, wxSize(40, 20));
      lbPeaksI[iCh]->SetWindowStyle(wxBU_RIGHT | wxALIGN_RIGHT | wxBORDER_NONE);

      lbPeaksI[iCh]->SetSize(wxSize(40, 20));
      // lbPeaksI[iCh]->SetHint("Peal level\nClick to reset");

      lbGainI[iCh] = new wxStaticText(this, wxID_ANY, wxString("0 dB"));
      lbPeaksI[iCh]->SetWindowStyle(wxALIGN_RIGHT);
      if (iCh < activeInputs) {
        sizer->Add(lbPeaksI[iCh], wxGBPosition(1, iCh * 3), wxGBSpan(1, 1),
                   wxALIGN_CENTER);
        if (pid == 0x8754) {
          sizer->Add(lbGainI[iCh], wxGBPosition(1, iCh * 3 + 1), wxGBSpan(1, 1),
                     wxALIGN_CENTER);
        } else {
          lbGainI[iCh]->Hide();
        }
      } else {
        lbPeaksI[iCh]->Hide();
        lbGainI[iCh]->Hide();
      }
    }
    for (size_t i = 0; i < N_INPUTS; i++) {
      gaLevels[i] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                wxSize(24, 100), wxGA_VERTICAL);
      gaLevels[i]->SetValue(0);

      slGainI[i] = new wxSlider(this, ID_INPUT_GAIN + i, 0, minGain, maxGain,
                                wxDefaultPosition, wxDefaultSize,
                                wxSL_VERTICAL | wxSL_LABELS | wxSL_INVERSE);
      slGainI[i]->SetPageSize(5);
      slGainI[i]->SetTickFreq(5);

      auto buttonPanel = new wxPanel(this, wxID_ANY);
      auto sizerButtons = new wxGridSizer(7, 1, 10, 10);
      buttonPanel->SetSizer(sizerButtons);

      sizerButtons->Add(
          cb48V[i] = new wxToggleButton(buttonPanel, ID_INPUT_48V + i, "48V"),
          wxEXPAND);
      sizerButtons->Add(new wxStaticText(buttonPanel, wxID_ANY, ""),
                        wxEXPAND); // spacer
      sizerButtons->Add(
          cbMon[i] = new wxToggleButton(buttonPanel, ID_INPUT_MON + i, "MON"),
          wxEXPAND);
      if (i < 2) {
        sizerButtons->Add(cbInst[i] = new wxToggleButton(
                              buttonPanel, ID_INPUT_INST + i, "INST"),
                          wxEXPAND);
      } else {
        sizerButtons->Add(new wxStaticText(buttonPanel, wxID_ANY, ""),
                          wxEXPAND); // spacer
      }

      cbSolo[i] = new wxToggleButton(buttonPanel, ID_INPUT_SOLO + i, "SOLO");
      cbMute[i] = new wxToggleButton(buttonPanel, ID_INPUT_MUTE + i, "MUTE");
      cbPhase[i] = new wxToggleButton(buttonPanel, ID_INPUT_PHASE + i, "PHASE");

      if (pid == 0x8754) {
        sizerButtons->Add(cbSolo[i], wxEXPAND);
        sizerButtons->Add(cbMute[i], wxEXPAND);
        sizerButtons->Add(cbPhase[i], wxEXPAND);
      } else {
        cbSolo[i]->Hide();
        cbMute[i]->Hide();
        cbPhase[i]->Hide();
      }

      if (i < activeInputs) {
        sizer->Add(gaLevels[i], wxGBPosition(2, i * 3), wxGBSpan(1, 1),
                   wxEXPAND);
        if (pid == 0x8754) {
          sizer->Add(slGainI[i], wxGBPosition(2, i * 3 + 1), wxGBSpan(1, 1),
                     wxEXPAND);
        } else {
          slGainI[i]->Hide();
        }
        sizer->Add(buttonPanel, wxGBPosition(2, i * 3 + 2), wxGBSpan(1, 1),
                   wxALIGN_TOP);
      } else {
        gaLevels[i]->Hide();
        slGainI[i]->Hide();
        buttonPanel->Hide();
      }
    }
    sizer->SetMinSize(500, 300);
    sizer->AddGrowableRow(2, 1);
    SetSizer(sizer);
  };
}; // PanelInputs

class PanelMixers : public wxPanel {
public:
  const static int32_t N_MIXERS = 4;
  const static int32_t N_MIX_SRCS = 12;
  wxRadioBox *rboxMixerSelBox;

  wxStaticText *lbTitle[N_MIX_SRCS / 2];
  wxButton *btPeaks[N_MIX_SRCS];
  wxGauge *gaLevel[N_MIX_SRCS];
  wxSlider *slPan[N_MIX_SRCS];
  wxSlider *slVol[N_MIX_SRCS];
  wxSlider *slVolB[N_MIX_SRCS / 2];
  wxToggleButton *ckMute[N_MIX_SRCS];
  wxToggleButton *ckSolo[N_MIX_SRCS];
  wxToggleButton *ckPhase[N_MIX_SRCS];

  int32_t PeaksI[N_MIX_SRCS];

  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  // const static double LEVEL_UNIT = 0.1;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 6; // for the int32_t ,  NEVER exceed 36!

  const wxString MixSels[N_MIXERS] = {
      wxString("MIX A"),
      wxString("MIX B"),
      wxString("MIX C"),
      wxString("MIX D"),
  };
  const wxString Titles[N_MIX_SRCS / 2] = {
      wxString("Input 1+2"),   wxString("Input 3+4"),   wxString("Playback1+2"),
      wxString("Playback3+4"), wxString("Playback5+6"), wxString("Playback7+8"),
  };

  PanelMixers(wxWindow *parent, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY) {
    const auto MARGIN = FromDIP(8);
    const auto COLS = 5 + 1;
    auto sizer = new wxGridBagSizer(MARGIN, MARGIN);

    int32_t row = 0;
    SetWindowStyle(wxBORDER_SUNKEN);

    rboxMixerSelBox =
        new wxRadioBox(this, ID_MIX_BUS_SEL, "", wxDefaultPosition,
                       wxDefaultSize, N_MIXERS, MixSels);
    sizer->Add(rboxMixerSelBox, wxGBPosition(row, 0),
               wxGBSpan(1, N_MIX_SRCS / 2 * COLS), wxALIGN_LEFT);

    row = 1;
    for (size_t i = 0; i < N_MIX_SRCS / 2; i++) {
      lbTitle[i] = new wxStaticText(this, wxID_ANY, Titles[i]);
      lbTitle[i]->SetWindowStyle(wxALIGN_CENTER);
      wxFont font = lbTitle[i]->GetFont();
      lbTitle[i]->SetFont(font.Larger().Larger());

      sizer->Add(lbTitle[i], wxGBPosition(row, i * COLS), wxGBSpan(1, COLS - 1),
                 wxALIGN_CENTER | wxEXPAND);
    }

    // pan
    row = 2;
    for (size_t i = 0; i < N_MIX_SRCS / 2; i++) {
      size_t l = i * 2;
      size_t r = l + 1;

      slPan[l] =
          new wxSlider(this, ID_MIX_PAN + l, 0, -100, 100, wxDefaultPosition,
                       wxDefaultSize, wxSL_TOP | wxSL_VALUE_LABEL);
      slPan[r] =
          new wxSlider(this, ID_MIX_PAN + r, 0, -100, 100, wxDefaultPosition,
                       wxDefaultSize, wxSL_TOP | wxSL_VALUE_LABEL);

      slPan[l]->SetTickFreq(25);
      slPan[r]->SetTickFreq(25);
      slPan[l]->SetPageSize(5);
      slPan[r]->SetPageSize(5);
      slPan[l]->SetValue(-100);
      slPan[r]->SetValue(100);

      sizer->Add(slPan[l], wxGBPosition(row, i * COLS + 0), wxGBSpan(1, 2),
                 wxEXPAND);
      sizer->Add(slPan[r], wxGBPosition(row, i * COLS + 3), wxGBSpan(1, 2),
                 wxEXPAND);
    }
    // fader
    row = 3;
    for (size_t i = 0; i < N_MIX_SRCS / 2; i++) {
      size_t l = i * 2;
      size_t r = l + 1;
      gaLevel[l] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                               FromDIP(wxSize(16, 100)), wxGA_VERTICAL);
      gaLevel[r] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                               FromDIP(wxSize(16, 100)), wxGA_VERTICAL);
      gaLevel[l]->SetValue(0);
      gaLevel[r]->SetValue(0);

      slVol[l] = new wxSlider(this, ID_MIX_VOL + l, 0, minGain, maxGain,
                              wxDefaultPosition, FromDIP(wxSize(50, 100)),
                              wxSL_VERTICAL | wxSL_RIGHT | wxSL_VALUE_LABEL |
                                  wxSL_INVERSE);
      slVol[r] = new wxSlider(this, ID_MIX_VOL + r, 0, minGain, maxGain,
                              wxDefaultPosition, FromDIP(wxSize(50, 100)),
                              wxSL_VERTICAL | wxSL_LEFT | wxSL_VALUE_LABEL |
                                  wxSL_INVERSE);
      slVolB[i] = new wxSlider(this, ID_MIX_VOL_B + i, 0, minGain, maxGain,
                               wxDefaultPosition, FromDIP(wxSize(30, 100)),
                               wxSL_VERTICAL | wxSL_INVERSE);

      slVol[l]->SetTickFreq(6);
      slVol[r]->SetTickFreq(6);
      slVol[l]->SetPageSize(3);
      slVol[r]->SetPageSize(3);
      slVolB[i]->SetPageSize(3);

      sizer->Add(gaLevel[l], wxGBPosition(row, i * COLS), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(gaLevel[r], wxGBPosition(row, i * COLS + 4), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(slVol[l], wxGBPosition(row, i * COLS + 1), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(slVol[r], wxGBPosition(row, i * COLS + 3), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(slVolB[i], wxGBPosition(row, i * COLS + 2), wxGBSpan(1, 1),
                 wxEXPAND);
    }
    sizer->AddGrowableRow(row, 1);

    row = 4;

    for (size_t i = 0; i < N_MIX_SRCS / 2; i++) {
      size_t a = i * 2;
      size_t b = a + 1;
      ckMute[a] = new wxToggleButton(this, ID_MIX_MUTE + a, "Mute",
                                     wxDefaultPosition, wxDefaultSize);
      ckMute[b] = new wxToggleButton(this, ID_MIX_MUTE + b, "Mute",
                                     wxDefaultPosition, wxDefaultSize);
      ckSolo[a] = new wxToggleButton(this, ID_MIX_SOLO + a, "Solo",
                                     wxDefaultPosition, wxDefaultSize);
      ckSolo[b] = new wxToggleButton(this, ID_MIX_SOLO + b, "Solo",
                                     wxDefaultPosition, wxDefaultSize);
      ckPhase[a] = new wxToggleButton(this, ID_MIX_PHASE + a, "Phase",
                                      wxDefaultPosition, wxDefaultSize);
      ckPhase[b] = new wxToggleButton(this, ID_MIX_PHASE + b, "Phase",
                                      wxDefaultPosition, wxDefaultSize);

      sizer->Add(ckMute[a], wxGBPosition(row, i * COLS + 0), wxGBSpan(1, 2),
                 wxLEFT);
      sizer->Add(ckMute[b], wxGBPosition(row, i * COLS + 3), wxGBSpan(1, 2),
                 wxRIGHT);
      sizer->Add(ckSolo[a], wxGBPosition(row + 1, i * COLS + 0), wxGBSpan(1, 2),
                 wxCENTER);
      sizer->Add(ckSolo[b], wxGBPosition(row + 1, i * COLS + 3), wxGBSpan(1, 2),
                 wxCENTER);
      if (pid == 0x8754) {
        sizer->Add(ckPhase[a], wxGBPosition(row + 2, i * COLS + 0),
                   wxGBSpan(1, 2), wxCENTER);
        sizer->Add(ckPhase[b], wxGBPosition(row + 2, i * COLS + 3),
                   wxGBSpan(1, 2), wxCENTER);
      } else {
        ckPhase[a]->Hide();
        ckPhase[b]->Hide();
      }
    }
    // spacer
    for (size_t i = 1; i < N_MIX_SRCS / 2; i++) {
      sizer->Add(FromDIP(24), 0, wxGBPosition(1, i * COLS - 1), wxGBSpan(3, 1));
    }
    SetSizer(sizer);
  };
}; // PanelMixers

class PanelLoopbacks : public wxPanel {
public:
  const static int32_t N_LOOPBACKS = 3;

  int32_t PeaksI[N_LOOPBACKS * 2];

  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  // const static double LEVEL_UNIT = 0.1;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 20; // for the int32_t ,  NEVER exceed 36!

  const wxString OutputSels[14] = {
      wxString("IN1"),         wxString("IN2"),         wxString("IN3"),
      wxString("IN4"),         wxString("IN1+2"),       wxString("IN3+4"),
      wxString("Playback1+2"), wxString("Playback3+4"), wxString("Playback5+6"),
      wxString("Playback7+8"), wxString("MIX A"),       wxString("MIX B"),
      wxString("MIX C"),       wxString("MIX D"),
  };

  wxStaticText *lbTitle[N_LOOPBACKS];
  wxComboBox *cbSelect[N_LOOPBACKS];
  wxGauge *gaLevelsI[N_LOOPBACKS * 2];
  wxGauge *gaLevelsO[N_LOOPBACKS * 2];
  wxSlider *slOutputL[N_LOOPBACKS];
  wxSlider *slOutputB[N_LOOPBACKS];
  wxSlider *slOutputR[N_LOOPBACKS];

  wxToggleButton *ckMute[N_LOOPBACKS * 2];

  PanelLoopbacks(wxWindow *parent) : wxPanel(parent, wxID_ANY) {
    const auto MARGIN = FromDIP(8);
    auto sizer = new wxGridBagSizer(MARGIN, MARGIN);
    const int32_t COLS = 7 + 1;
    SetWindowStyle(wxSIMPLE_BORDER);
    for (size_t i = 0; i < N_LOOPBACKS; i++) {
      lbTitle[i] = new wxStaticText(
          this, wxID_ANY, std::format("Loopback{}+{}", i * 2 + 1, i * 2 + 2));
      lbTitle[i]->SetWindowStyle(wxALIGN_CENTER);
      wxFont font = lbTitle[i]->GetFont();
      lbTitle[i]->SetFont(font.Larger().Larger());

      sizer->Add(lbTitle[i], wxGBPosition(0, i * COLS), wxGBSpan(1, COLS - 1),
                 wxALIGN_CENTER);
    }

    for (size_t i = 0; i < N_LOOPBACKS; i++) {
      cbSelect[i] = new wxComboBox(this, ID_LOOP_SEL + i, OutputSels[10 + i],
                                   wxDefaultPosition, wxDefaultSize, 14,
                                   OutputSels, wxCB_READONLY);
      cbSelect[i]->SetHint("Select Output Source");
      sizer->Add(cbSelect[i], wxGBPosition(1, i * COLS), wxGBSpan(1, COLS - 1),
                 wxALIGN_CENTER);
    }

    for (size_t i = 0; i < N_LOOPBACKS; i++) {
      int32_t l = i * 2;
      int32_t r = l + 1;

      gaLevelsI[l] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(16, 100)), wxGA_VERTICAL);
      gaLevelsI[r] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(16, 100)), wxGA_VERTICAL);
      gaLevelsO[l] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(24, 100)), wxGA_VERTICAL);
      gaLevelsO[r] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(24, 100)), wxGA_VERTICAL);

      slOutputL[i] = new wxSlider(this, ID_LOOP_VOL_L + i, 0, -90, 6,
                                  wxDefaultPosition, FromDIP(wxSize(60, 0)),
                                  wxSL_VERTICAL | wxSL_RIGHT |
                                      wxSL_VALUE_LABEL | wxSL_INVERSE);
      slOutputB[i] =
          new wxSlider(this, ID_LOOP_VOL_B + i, 0, -90, 6, wxDefaultPosition,
                       FromDIP(wxSize(30, 0)), wxSL_VERTICAL | wxSL_INVERSE);
      slOutputR[i] = new wxSlider(this, ID_LOOP_VOL_R + i, 0, -90, 6,
                                  wxDefaultPosition, FromDIP(wxSize(60, 0)),
                                  wxSL_VERTICAL | wxSL_LEFT | wxSL_VALUE_LABEL |
                                      wxSL_INVERSE);

      gaLevelsI[l]->SetValue(0);
      gaLevelsI[r]->SetValue(0);
      gaLevelsO[l]->SetValue(0);
      gaLevelsO[r]->SetValue(0);

      slOutputL[i]->SetPageSize(3);
      slOutputB[i]->SetPageSize(3);
      slOutputR[i]->SetPageSize(3);
      slOutputL[i]->SetTickFreq(6);
      // slOutputB[i]->SetTickFreq(6);
      slOutputR[i]->SetTickFreq(6);

      sizer->Add(slOutputL[i], wxGBPosition(2, i * COLS + 2), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(slOutputR[i], wxGBPosition(2, i * COLS + 4), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(slOutputB[i], wxGBPosition(2, i * COLS + 3), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(gaLevelsI[l], wxGBPosition(2, i * COLS + 0), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(gaLevelsI[r], wxGBPosition(2, i * COLS + 6), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(gaLevelsO[l], wxGBPosition(2, i * COLS + 1), wxGBSpan(1, 1),
                 wxEXPAND);
      sizer->Add(gaLevelsO[r], wxGBPosition(2, i * COLS + 5), wxGBSpan(1, 1),
                 wxEXPAND);
    }

    for (size_t i = 0; i < N_LOOPBACKS; i++) {
      int32_t l = i * 2;
      int32_t r = l + 1;

      ckMute[l] = new wxToggleButton(this, ID_LOOP_MUTE + l, "Mute",
                                     wxDefaultPosition, wxDefaultSize);
      ckMute[r] = new wxToggleButton(this, ID_LOOP_MUTE + r, "Mute",
                                     wxDefaultPosition, wxDefaultSize);

      sizer->Add(ckMute[l], wxGBPosition(3, i * COLS + 0), wxGBSpan(1, 3),
                 wxALIGN_CENTER);
      sizer->Add(ckMute[r], wxGBPosition(3, i * COLS + 4), wxGBSpan(1, 3),
                 wxALIGN_CENTER);
    }

    for (size_t i = 1; i < N_LOOPBACKS; i++) {
      sizer->Add(FromDIP(16), 0, wxGBPosition(0, i * COLS - 1), wxGBSpan(4, 1));
    }
    sizer->AddGrowableRow(2, 1);
    SetSizer(sizer);
  };
}; // PanelLoopbacks
class PanelOutputs : public wxPanel {
public:
  const static int32_t N_OUTPUTS = 2;
  wxStaticText *lbTitle[3];
  wxComboBox *cbSelect[N_OUTPUTS];
  wxGauge *gaLevelsI[N_OUTPUTS * 2];
  wxGauge *gaLevelsO[N_OUTPUTS * 2];
  wxSlider *slOutputL[N_OUTPUTS];
  wxSlider *slOutputB[N_OUTPUTS];
  wxSlider *slOutputR[N_OUTPUTS];

  wxToggleButton *ckOutputLine[N_OUTPUTS];
  wxToggleButton *ckOutputMon[N_OUTPUTS];

  int32_t PeaksI[N_OUTPUTS * 4];

  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  // const static double LEVEL_UNIT = 0.1;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 20; // for the int32_t ,  NEVER exceed 36!

  const wxString OutputSels[14] = {
      wxString("IN1"),         wxString("IN2"),         wxString("IN3"),
      wxString("IN4"),         wxString("IN1+2"),       wxString("IN3+4"),
      wxString("Playback1+2"), wxString("Playback3+4"), wxString("Playback5+6"),
      wxString("Playback7+8"), wxString("MIX A"),       wxString("MIX B"),
      wxString("MIX C"),       wxString("MIX D"),
  };
  PanelOutputs(wxWindow *parent, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY) {
    const auto MARGIN = FromDIP(8);
    auto sizer = new wxGridBagSizer(MARGIN, MARGIN);
    const int32_t COLS = 7 + 1;
    SetWindowStyle(wxBORDER_SUNKEN);

    bool isOTG = (pid == 0x8755 || pid == 0x8756);
    bool isE2x2 = (pid == 0x8752);
    bool isE4x4 = (pid == 0x8754);

    // 1. Titles
    if (isE4x4) {
      lbTitle[0] = new wxStaticText(this, wxID_ANY, "Output1");
      lbTitle[1] = new wxStaticText(this, wxID_ANY, "Output2");
    } else if (isOTG) {
      lbTitle[0] = new wxStaticText(this, wxID_ANY, "Output 1+2");
      lbTitle[1] = new wxStaticText(this, wxID_ANY, "Mobile OUT");
    } else { // E2x2 non-OTG
      lbTitle[0] = new wxStaticText(this, wxID_ANY, "Output 1+2");
      lbTitle[1] = new wxStaticText(this, wxID_ANY, "Unused");
    }

    for (size_t i = 0; i < 2; ++i) {
      lbTitle[i]->SetWindowStyle(wxALIGN_CENTER);
      lbTitle[i]->SetFont(lbTitle[i]->GetFont().Larger().Larger());
    }

    // Add titles to sizer
    sizer->Add(lbTitle[0], wxGBPosition(0, 0 * COLS), wxGBSpan(1, COLS - 1),
               wxALIGN_CENTER);
    if (!isE2x2) {
      sizer->Add(lbTitle[1], wxGBPosition(0, 1 * COLS), wxGBSpan(1, COLS - 1),
                 wxALIGN_CENTER);
    }

    // 2. Combo Boxes (cbSelect)
    for (size_t i = 0; i < N_OUTPUTS; i++) {
      cbSelect[i] = new wxComboBox(this, ID_OUTPUT_SEL + i, OutputSels[6],
                                   wxDefaultPosition, wxDefaultSize, 14,
                                   OutputSels, wxCB_READONLY);
      if (i == 0 || (i == 1 && isOTG) || (i == 1 && isE4x4)) {
        sizer->Add(cbSelect[i], wxGBPosition(1, i * COLS),
                   wxGBSpan(1, COLS - 1), wxALIGN_CENTER);
      } else {
        cbSelect[i]->Hide();
      }
    }

    // 3. Gauges & Sliders
    for (size_t i = 0; i < N_OUTPUTS; i++) {
      int32_t l = i * 2;
      int32_t r = l + 1;

      gaLevelsI[l] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(16, 100)), wxGA_VERTICAL);
      gaLevelsI[r] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(16, 100)), wxGA_VERTICAL);
      gaLevelsO[l] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(24, 100)), wxGA_VERTICAL);
      gaLevelsO[r] = new wxGauge(this, wxID_ANY, LEVEL_RANGE, wxDefaultPosition,
                                 FromDIP(wxSize(24, 100)), wxGA_VERTICAL);

      slOutputL[i] = new wxSlider(
          this, ID_OUTPUT_VOL_L + i, 0, minGain, maxGain, wxDefaultPosition,
          FromDIP(wxSize(60, 0)),
          wxSL_VERTICAL | wxSL_RIGHT | wxSL_VALUE_LABEL | wxSL_INVERSE);
      slOutputB[i] = new wxSlider(
          this, ID_OUTPUT_VOL_B + i, 0, minGain, maxGain, wxDefaultPosition,
          FromDIP(wxSize(30, 0)), wxSL_VERTICAL | wxSL_INVERSE);
      slOutputR[i] = new wxSlider(
          this, ID_OUTPUT_VOL_R + i, 0, minGain, maxGain, wxDefaultPosition,
          FromDIP(wxSize(60, 0)),
          wxSL_VERTICAL | wxSL_LEFT | wxSL_VALUE_LABEL | wxSL_INVERSE);

      gaLevelsI[l]->SetValue(0);
      gaLevelsI[r]->SetValue(0);
      gaLevelsO[l]->SetValue(0);
      gaLevelsO[r]->SetValue(0);

      slOutputL[i]->SetPageSize(3);
      slOutputB[i]->SetPageSize(3);
      slOutputR[i]->SetPageSize(3);
      slOutputL[i]->SetTickFreq(6);
      // slOutputB[i]->SetTickFreq(6);
      slOutputR[i]->SetTickFreq(6);

      if (i == 0 || (i == 1 && isOTG) || (i == 1 && isE4x4)) {
        sizer->Add(slOutputL[i], wxGBPosition(2, i * COLS + 2), wxGBSpan(1, 1),
                   wxEXPAND);
        sizer->Add(slOutputR[i], wxGBPosition(2, i * COLS + 4), wxGBSpan(1, 1),
                   wxEXPAND);
        sizer->Add(slOutputB[i], wxGBPosition(2, i * COLS + 3), wxGBSpan(1, 1),
                   wxEXPAND);
        sizer->Add(gaLevelsI[l], wxGBPosition(2, i * COLS + 0), wxGBSpan(1, 1),
                   wxEXPAND);
        sizer->Add(gaLevelsI[r], wxGBPosition(2, i * COLS + 6), wxGBSpan(1, 1),
                   wxEXPAND);
        sizer->Add(gaLevelsO[l], wxGBPosition(2, i * COLS + 1), wxGBSpan(1, 1),
                   wxEXPAND);
        sizer->Add(gaLevelsO[r], wxGBPosition(2, i * COLS + 5), wxGBSpan(1, 1),
                   wxEXPAND);
      } else {
        gaLevelsI[l]->Hide();
        gaLevelsI[r]->Hide();
        gaLevelsO[l]->Hide();
        gaLevelsO[r]->Hide();
        slOutputL[i]->Hide();
        slOutputB[i]->Hide();
        slOutputR[i]->Hide();
      }
    }

    // 4. Toggle Buttons
    for (size_t i = 0; i < N_OUTPUTS; i++) {
      if (isE4x4) {
        ckOutputMon[i] =
            new wxToggleButton(this, ID_OUTPUT_MON + i, wxString("Mon"),
                               wxDefaultPosition, wxDefaultSize);
        ckOutputLine[i] =
            new wxToggleButton(this, ID_OUTPUT_LINE + i, wxString("Line"),
                               wxDefaultPosition, wxDefaultSize);
        ckOutputMon[i]->SetValue(true);
        ckOutputLine[i]->SetValue(true);
        sizer->Add(ckOutputMon[i], wxGBPosition(3, i * COLS + 0),
                   wxGBSpan(1, 3), wxEXPAND);
        sizer->Add(ckOutputLine[i], wxGBPosition(3, i * COLS + 4),
                   wxGBSpan(1, 3), wxEXPAND);
      } else if (i == 0) {
        // Column 0 (Output 1+2): Labeled Phones (Headphones) and Line (Line
        // Out)
        ckOutputMon[i] =
            new wxToggleButton(this, ID_OUTPUT_MON + i, wxString("Phones"),
                               wxDefaultPosition, wxDefaultSize);
        ckOutputLine[i] =
            new wxToggleButton(this, ID_OUTPUT_LINE + i, wxString("Line"),
                               wxDefaultPosition, wxDefaultSize);
        ckOutputMon[i]->SetValue(true);
        ckOutputLine[i]->SetValue(true);
        sizer->Add(ckOutputMon[i], wxGBPosition(3, i * COLS + 0),
                   wxGBSpan(1, 3), wxEXPAND);
        sizer->Add(ckOutputLine[i], wxGBPosition(3, i * COLS + 4),
                   wxGBSpan(1, 3), wxEXPAND);
      } else {
        // Column 1: Labeled "MUTE" for Mobile Out
        ckOutputMon[i] =
            new wxToggleButton(this, ID_OUTPUT_MON + i, wxString("MUTE"),
                               wxDefaultPosition, wxDefaultSize);
        ckOutputLine[i] =
            new wxToggleButton(this, ID_OUTPUT_LINE + i, wxString("Unused"),
                               wxDefaultPosition, wxDefaultSize);
        ckOutputMon[i]->SetValue(true);
        ckOutputLine[i]->SetValue(true);
        ckOutputLine[i]->Hide();

        if (isOTG) {
          sizer->Add(ckOutputMon[i], wxGBPosition(3, i * COLS + 2),
                     wxGBSpan(1, 3), wxEXPAND);
        } else {
          ckOutputMon[i]->Hide();
        }
      }
    }

    sizer->Add(FromDIP(16), 0, wxGBPosition(0, COLS - 1), wxGBSpan(3, 1));
    if (isOTG || isE4x4) {
      sizer->Add(FromDIP(16), 0, wxGBPosition(0, 2 * COLS - 1), wxGBSpan(3, 1));
    }
    sizer->AddGrowableRow(2, 1);
    SetSizer(sizer);
  };
}; // PanelOutputs

class PanelPhones : public wxPanel {
public:
  const static int32_t N_PHONES = 2;
  wxStaticText *lbTitle[N_PHONES];
  wxStaticText *lbTitleMix[N_PHONES];
  wxSlider *slPhoneMix[N_PHONES];
  wxToggleButton *ckPhoneGain[N_PHONES];

  PanelPhones(wxWindow *parent) : wxPanel(parent, wxID_ANY) {
    const auto MARGIN = FromDIP(8);
    auto sizerPhones = new wxFlexGridSizer(8, 1, MARGIN, MARGIN);

    sizerPhones->AddGrowableCol(0);
    for (size_t i = 0; i < N_PHONES; i++) {
      lbTitle[i] =
          new wxStaticText(this, wxID_ANY, std::format("Phone {}", i + 1));
      sizerPhones->Add(lbTitle[i], 1, wxALIGN_CENTER);

      lbTitleMix[i] = new wxStaticText(
          this, wxID_ANY, std::format("Monitor <- Phone Mix -> Play"));
      sizerPhones->Add(lbTitleMix[i], 1, wxALIGN_CENTER);

      slPhoneMix[i] =
          new wxSlider(this, ID_PHONE_MIX + i, 0, -100, 100, wxDefaultPosition,
                       wxDefaultSize, wxSL_HORIZONTAL | wxSL_LABELS);
      slPhoneMix[i]->SetPageSize(5);

      sizerPhones->Add(slPhoneMix[i], 1, wxEXPAND);

      ckPhoneGain[i] =
          new wxToggleButton(this, ID_PHONE_GAIN + i, "Gain +17dB");
      sizerPhones->Add(ckPhoneGain[i], 1, wxALIGN_RIGHT);
    }
    SetSizer(sizerPhones);
  };
}; // PanelPhones

class PanelDevice : public wxPanel {
public:
  wxCheckBox *cbAutoStandby;
  wxCheckBox *cbOTGMode;

  PanelDevice(wxWindow *parent, uint16_t pid) : wxPanel(parent, wxID_ANY) {
    auto sizerMain = new wxBoxSizer(wxVERTICAL);

    auto sizerGroup = new wxStaticBoxSizer(wxVERTICAL, this, "Device Settings");

    cbAutoStandby =
        new wxCheckBox(sizerGroup->GetStaticBox(), wxID_ANY, "Auto Standby");
    cbAutoStandby->SetValue(true);
    sizerGroup->Add(cbAutoStandby, 0, wxALL | wxEXPAND, 8);

    bool isOTG = (pid == 0x8755 || pid == 0x8756);
    if (isOTG) {
      cbOTGMode = new wxCheckBox(sizerGroup->GetStaticBox(), wxID_ANY,
                                 "OTG Mode (Mobile Charging/Power)");
      cbOTGMode->SetValue(true);
      sizerGroup->Add(cbOTGMode, 0, wxALL | wxEXPAND, 8);
    } else {
      cbOTGMode = nullptr;
    }

    sizerMain->Add(sizerGroup, 0, wxEXPAND | wxALL, 15);
    SetSizer(sizerMain);
  };
}; // PanelDevice

class TPMixer : public wxFrame {
public:
  bool toStopHidReader = false;

  ToppingHID *hid;
  Gain *gain;

  PanelInputs *panelInputs;
  PanelMixers *panelMixers;
  PanelLoopbacks *panelLoopbacks;
  PanelOutputs *panelOutputs;
  PanelPhones *panelPhones;
  PanelDevice *panelDevice;

  TPMixer();

protected:
  const std::string pathSep = "/";
  const std::string dirConfig = ".config";
  const std::string dirApp = "toppingmixer";
  const std::string ConfigFile = "toppingmixer.settings";
  std::string dir1;
  std::string dir2;
  std::string fileCfg;

  std::thread *thReader;
  hid_device *handle;
  void scbUpdateLevels(uint16_t ch16, int32_t val);
  void pushGuiStateToDevice();

  void HidReader(hid_device *handle) {
    uint8_t bufread[256];
    uint16_t ch = 0;
    uint64_t val;
    int res = 0;
    std::set<uint16_t> known{
        0x2104,
        0x2204,
        0x2304,
        0x2404,
    };
    for (uint16_t b = 0x41; b <= 0x48; b++) {
      known.insert((b << 8) | 0x01);
    }
    for (uint16_t a = 1; a <= 2; a++) {
      for (uint16_t b = 0x31; b <= 0x34; b++) {
        known.insert((b << 8) | a);
      }
      for (uint16_t b = 0x51; b <= 0x56; b++) {
        known.insert((b << 8) | a);
      }
    }
    if (NULL != handle) {
      printf("%s(), polling\n", __func__);
      while ((NULL != handle) && (!toStopHidReader)) {
        res = hid_read(handle, bufread, 16);
        if (res < 0) {
          printf("hid_read failed: %d\n", res);
          break;
        }
        if (res > 0 && 0x22 == bufread[0]) {
          ch = read16BE(&bufread[5]);
          val = read32BE(&bufread[7]);
          CallAfter(&TPMixer::scbUpdateLevels, ch, val);

          uint8_t main_ch = ch >> 8;
          uint8_t sub = ch & 0xFF;
          bool is_level =
              (sub == 0x04) || (main_ch >= 0x31 && main_ch <= 0x34 &&
                                (sub == 0x01 || sub == 0x02));
          if (!is_level) {
            // printBuf8(bufread, 16, "<< ");
          }
        }
      }
      printf("HID Reader ended!!!!!!!!\n");

    } else {
      printf("Demonstration:\n");
      int32_t ch = 0;
      while (!toStopHidReader) {
        int32_t rndv[3] = {400};
        for (int32_t l = 0; (l < 1030) && (!toStopHidReader); l += 4) {
          rndv[l % 3] = rand() % panelInputs->LEVEL_RANGE;

          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          CallAfter(&TPMixer::scbUpdateLevels, (uint16_t)(0x2104 + (ch << 8)),
                    (rndv[0] * rndv[1] / panelInputs->LEVEL_RANGE * rndv[2] /
                         panelInputs->LEVEL_RANGE +
                     panelInputs->LEVEL_MIN));
          ch = (ch + 1) & 0x03;
          // CallAfter (&TPMixer::scbUpdateLevels, (uint16_t)0x2204, v / 2);
          // CallAfter (&TPMixer::scbUpdateLevels, (uint16_t)0x2304, (rndv *
          // rndv / panelInputs->LEVEL_RANGE * rndv / panelInputs->LEVEL_RANGE +
          // panelInputs->LEVEL_MIN)); CallAfter (&TPMixer::scbUpdateLevels,
          // (uint16_t)0x2404, (rndv * rndv / panelInputs->LEVEL_RANGE +
          // panelInputs->LEVEL_MIN));
        }
      }
      printf("Demonstration ended.\n");
    }
    (void)res;
  };
  void startHidReader() {
    handle = hid->getHandle();
    thReader = new std::thread(&TPMixer::HidReader, this, handle);
    // printf("Reader started.\n");
  };

  void setOutputVol(int32_t ch) {
    int32_t l = ch * 2;
    int32_t r = l + 1;
    hid->setOutputVol(l,
                      gain->getMonoGain(panelOutputs->slOutputL[ch]->GetValue(),
                                        false, false, false, false));
    hid->setOutputVol(r,
                      gain->getMonoGain(panelOutputs->slOutputR[ch]->GetValue(),
                                        false, false, false, false));
  };

  void setLoopVol(int32_t ch) {
    int32_t l = ch * 2;
    int32_t r = l + 1;
    hid->setLoopVol(l,
                    gain->getMonoGain(panelLoopbacks->slOutputL[ch]->GetValue(),
                                      panelLoopbacks->ckMute[l]->GetValue(),
                                      false, false, false));
    hid->setLoopVol(r,
                    gain->getMonoGain(panelLoopbacks->slOutputR[ch]->GetValue(),
                                      panelLoopbacks->ckMute[r]->GetValue(),
                                      false, false, false));
  };
  // @return muted, phase, gainDB
  std::tuple<bool, bool, int32_t> gain2dB(int32_t gain32) {
    bool muted = false;
    bool phase = false;
    int32_t gainDB = 0;
    if (0 == gain32) {
      gainDB = panelInputs->minGain; // default minimum
      muted = true;
    } else {
      if (gain32 < 0) {
        gain32 = -gain32;
        phase = true;
      }
      gainDB = round(log10((double)gain32 / (double)0x02000000) * 20);
    }
    return {muted, phase, gainDB};
  }

  // @return muted, phase, pan, gainDB
  //         pan: -100 for full left, 100 for full right
  std::tuple<bool, bool, int32_t, int32_t>
  lrGain2PandB(int32_t gainL, int32_t gainR, int32_t minGain = -90) {
    bool muted = false;
    bool phase = false;
    int32_t totalGain = gainL + gainR;
    int32_t gainDB = 0;
    int32_t pan = 0;
    if (0 == totalGain) {
      gainDB = panelMixers->minGain; // default minimum
      muted = true;
      phase = false;
      pan = 0;
    } else {
      if (totalGain < 0) {
        totalGain = -totalGain;
        phase = true;
      }
      pan = round((double)abs(gainR) / (double)totalGain * 200.0 - 100.);
      gainDB = round(log10((double)totalGain / (double)0x02000000) * 20);
      if (gainDB <= minGain) {
        muted = true;
      }
    }
    return {muted, phase, pan, gainDB};
  }
  // @param ch: 0..3 for channel 1..4, <0 to send all channels
  void sendInput(int32_t ch, bool hw = true, int16_t evtID = 0) {
    int32_t begin = ch, end = ch + 1;
    bool anySolo = false;
    int32_t gainDB = panelInputs->slGainI[ch]->GetValue();
    panelInputs->lbGainI[ch]->SetLabel(wxString(std::format("{:+}dB", gainDB)));
    SetStatusText(std::format("Input gain[{}] to {}", ch + 1, gainDB));
    for (int32_t i = 0; i < panelInputs->N_INPUTS; i++) {
      anySolo |= panelInputs->cbSolo[i]->GetValue();
    }
    if (ch < 0) {
      begin = 0;
      end = panelInputs->N_INPUTS;
    }
    for (int32_t i = begin; i < end; i++) {
      // for device safety, do not restore 48V ?
      // hid->setInput48V(i, panelInputs->cb48V[i]->GetValue());
      hid->setInputMon(i, panelInputs->cbMon[i]->GetValue());
      if (panelInputs->cbInst[i]) {
        hid->setInputInst(i, panelInputs->cbInst[i]->GetValue());
      }

      int32_t gainVal =
          gain->getMonoGain(gainDB, panelInputs->cbMute[i]->GetValue(),
                            panelInputs->cbSolo[i]->GetValue(), anySolo,
                            panelInputs->cbPhase[i]->GetValue());
      // printf(" gain %3d dB, %11d, %08x\n", gainDB, gainVal, gainVal);
      hid->setInputGainiI32(i, gainVal, hw);

      // Save to settings map
      hid->settings[0x2101 + (i << 8)] =
          panelInputs->cbMon[i]->GetValue() ? 1 : 0;
      if (panelInputs->cbInst[i]) {
        hid->settings[0x2103 + (i << 8)] =
            panelInputs->cbInst[i]->GetValue() ? 1 : 0;
      }
      hid->settings[0x2105 + (i << 8)] = gainVal;

      // Save GUI-only keys for persistence
      hid->settings[0x2106 + (i << 8)] =
          panelInputs->cbMute[i]->GetValue() ? 1 : 0;
      hid->settings[0x2107 + (i << 8)] =
          panelInputs->cbSolo[i]->GetValue() ? 1 : 0;
      hid->settings[0x2108 + (i << 8)] =
          panelInputs->cbPhase[i]->GetValue() ? 1 : 0;
      hid->settings[0x2109 + (i << 8)] = gainDB;
    }
    if (ID_INPUT_SOLO == (evtID & 0xfff0)) {
      printf(" processing SOLO control\n");
      for (int32_t i = 0; i < panelInputs->N_INPUTS; i++) {
        gainDB = panelInputs->slGainI[i]->GetValue();
        int32_t gainVal =
            gain->getMonoGain(gainDB, panelInputs->cbMute[i]->GetValue(),
                              panelInputs->cbSolo[i]->GetValue(), anySolo,
                              panelInputs->cbPhase[i]->GetValue());
        // printf(" gain %3d dB, %11d, %08x\n", gainDB, gainVal, gainVal);
        hid->setInputGainiI32(i, gainVal, hw);
      }
    }
  }

  void saveSettings();
  void loadSettings();
  void refreshMixerUi(int16_t bus = -1);
  void refreshLoopbackUi();
  void refreshOutputUi();

  void OnLoad(wxCommandEvent &event);
  void OnSave(wxCommandEvent &event);
  void OnDeviceSave(wxCommandEvent &event);
  void OnExit(wxCommandEvent &event);
  void OnAbout(wxCommandEvent &event);
  void OnDeviceToggle(wxCommandEvent &event);

  void OnUpdateLevels(wxCommandEvent &evt);
  void OnClose(wxCloseEvent &event);

  void OnInputGain(wxCommandEvent &event);
  void OnInputPeak(wxCommandEvent &event);

  void OnMixBusSel(wxCommandEvent &event);
  void OnMixVolume(wxCommandEvent &event);
  void OnMixToggle(wxCommandEvent &event);

  void OnOutputVolume(wxCommandEvent &event);
  void OnOutputToggle(wxCommandEvent &event);

  void OnLoopVolume(wxCommandEvent &event);
  void OnLoopToggle(wxCommandEvent &event);

  void OnPhoneMix(wxCommandEvent &event);
  void OnPhoneGain(wxCommandEvent &event);
  wxDECLARE_EVENT_TABLE();
}; // TPMixer

// TODO
wxBEGIN_EVENT_TABLE(TPMixer, wxFrame) EVT_MENU(ID_SAVE, TPMixer::OnSave) EVT_MENU(
    ID_LOAD,
    TPMixer::
        OnLoad) EVT_MENU(ID_DEVICE_SAVE,
                         TPMixer::
                             OnDeviceSave) EVT_MENU(wxID_ABOUT,
                                                    TPMixer::
                                                        OnAbout) EVT_MENU(wxID_EXIT,
                                                                          TPMixer::
                                                                              OnExit)

    EVT_CLOSE(TPMixer::OnClose) EVT_SIZE(TPMixer::OnSize)

        EVT_BUTTON(ID_INPUT_PEAK, TPMixer::OnInputPeak) EVT_BUTTON(
            ID_INPUT_PEAK + 1,
            TPMixer::
                OnInputPeak) EVT_BUTTON(ID_INPUT_PEAK + 2,
                                        TPMixer::
                                            OnInputPeak) EVT_BUTTON(ID_INPUT_PEAK +
                                                                        3,
                                                                    TPMixer::
                                                                        OnInputPeak)

            EVT_SLIDER(ID_INPUT_GAIN, TPMixer::OnInputGain) EVT_SLIDER(
                ID_INPUT_GAIN + 1,
                TPMixer::
                    OnInputGain) EVT_SLIDER(ID_INPUT_GAIN + 2,
                                            TPMixer::
                                                OnInputGain) EVT_SLIDER(ID_INPUT_GAIN +
                                                                            3,
                                                                        TPMixer::
                                                                            OnInputGain)

                EVT_TOGGLEBUTTON(ID_INPUT_48V, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                    ID_INPUT_48V + 1,
                    TPMixer::
                        OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_48V + 2,
                                                      TPMixer::
                                                          OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_48V +
                                                                                            3,
                                                                                        TPMixer::
                                                                                            OnInputGain)

                    EVT_TOGGLEBUTTON(ID_INPUT_MON, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_MON + 1, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                        ID_INPUT_MON + 2,
                        TPMixer::
                            OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_MON + 3, TPMixer::OnInputGain)

                        EVT_TOGGLEBUTTON(ID_INPUT_INST, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_INST + 1, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                            ID_INPUT_INST +
                                2,
                            TPMixer::
                                OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_INST + 3, TPMixer::OnInputGain)

                            EVT_TOGGLEBUTTON(ID_INPUT_SOLO, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                                ID_INPUT_SOLO +
                                    1,
                                TPMixer::OnInputGain)
                                EVT_TOGGLEBUTTON(ID_INPUT_SOLO + 2, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                                    ID_INPUT_SOLO +
                                        3,
                                    TPMixer::OnInputGain)

                                    EVT_TOGGLEBUTTON(ID_INPUT_MUTE, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                                        ID_INPUT_MUTE +
                                            1,
                                        TPMixer::
                                            OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_MUTE +
                                                                              2,
                                                                          TPMixer::OnInputGain)
                                        EVT_TOGGLEBUTTON(ID_INPUT_MUTE + 3,
                                                         TPMixer::OnInputGain)

                                            EVT_TOGGLEBUTTON(ID_INPUT_PHASE, TPMixer::OnInputGain) EVT_TOGGLEBUTTON(
                                                ID_INPUT_PHASE + 1,
                                                TPMixer::
                                                    OnInputGain) EVT_TOGGLEBUTTON(ID_INPUT_PHASE +
                                                                                      2,
                                                                                  TPMixer::OnInputGain)
                                                EVT_TOGGLEBUTTON(
                                                    ID_INPUT_PHASE + 3,
                                                    TPMixer::OnInputGain)

                                                    EVT_RADIOBOX(
                                                        ID_MIX_BUS_SEL,
                                                        TPMixer::OnMixBusSel)

                                                        EVT_SLIDER(ID_MIX_VOL, TPMixer::OnMixVolume) EVT_SLIDER(
                                                            ID_MIX_VOL + 1,
                                                            TPMixer::
                                                                OnMixVolume)
                                                            EVT_SLIDER(ID_MIX_VOL + 2, TPMixer::OnMixVolume) EVT_SLIDER(
                                                                ID_MIX_VOL + 3,
                                                                TPMixer::
                                                                    OnMixVolume)
                                                                EVT_SLIDER(ID_MIX_VOL + 4, TPMixer::OnMixVolume) EVT_SLIDER(
                                                                    ID_MIX_VOL +
                                                                        5,
                                                                    TPMixer::
                                                                        OnMixVolume)
                                                                    EVT_SLIDER(
                                                                        ID_MIX_VOL +
                                                                            6,
                                                                        TPMixer::
                                                                            OnMixVolume)
                                                                        EVT_SLIDER(
                                                                            ID_MIX_VOL +
                                                                                7,
                                                                            TPMixer::
                                                                                OnMixVolume)
                                                                            EVT_SLIDER(
                                                                                ID_MIX_VOL +
                                                                                    8,
                                                                                TPMixer::
                                                                                    OnMixVolume)
                                                                                EVT_SLIDER(
                                                                                    ID_MIX_VOL +
                                                                                        9,
                                                                                    TPMixer::
                                                                                        OnMixVolume)
                                                                                    EVT_SLIDER(
                                                                                        ID_MIX_VOL +
                                                                                            10,
                                                                                        TPMixer::
                                                                                            OnMixVolume)
                                                                                        EVT_SLIDER(
                                                                                            ID_MIX_VOL +
                                                                                                11,
                                                                                            TPMixer::
                                                                                                OnMixVolume)
                                                                                            EVT_SLIDER(
                                                                                                ID_MIX_VOL +
                                                                                                    12,
                                                                                                TPMixer::
                                                                                                    OnMixVolume)

                                                                                                EVT_SLIDER(ID_MIX_PAN, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 1, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN +
                                                                                                                                                                                                             2,
                                                                                                                                                                                                         TPMixer::
                                                                                                                                                                                                             OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 3, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 4,
                                                                                                                                                                                                                                                                                      TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN +
                                                                                                                                                                                                                                                                                                                           5,
                                                                                                                                                                                                                                                                                                                       TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 6, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                            OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 7, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                        OnMixVolume) EVT_SLIDER(ID_MIX_PAN +
                                                                                                                                                                                                                                                                                                                                                                                                                                                    8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                                                    OnMixVolume) EVT_SLIDER(ID_MIX_PAN +
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                            TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 10, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 11, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_PAN + 12, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      OnMixVolume)

                                                                                                    EVT_SLIDER(ID_MIX_VOL_B, TPMixer::
                                                                                                                                 OnMixVolume) EVT_SLIDER(ID_MIX_VOL_B + 1, TPMixer::
                                                                                                                                                                               OnMixVolume) EVT_SLIDER(ID_MIX_VOL_B + 2, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_VOL_B + 3, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_VOL_B + 4, TPMixer::OnMixVolume) EVT_SLIDER(ID_MIX_VOL_B +
                                                                                                                                                                                                                                                                                                                                                                    5,
                                                                                                                                                                                                                                                                                                                                                                TPMixer::
                                                                                                                                                                                                                                                                                                                                                                    OnMixVolume)

                                                                                                        EVT_TOGGLEBUTTON(ID_MIX_SOLO, TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO + 1, TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO +
                                                                                                                                                                                                                                         2,
                                                                                                                                                                                                                                     TPMixer::
                                                                                                                                                                                                                                         OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO + 3, TPMixer::
                                                                                                                                                                                                                                                                                            OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO +
                                                                                                                                                                                                                                                                                                                              4,
                                                                                                                                                                                                                                                                                                                          TPMixer::
                                                                                                                                                                                                                                                                                                                              OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO +
                                                                                                                                                                                                                                                                                                                                                                5,
                                                                                                                                                                                                                                                                                                                                                            TPMixer::
                                                                                                                                                                                                                                                                                                                                                                OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO + 6, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                   OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO + 7, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                                                                      OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO +
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO + 9, TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO + 10, TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_SOLO +
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           11,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           OnMixVolume)

                                                                                                            EVT_TOGGLEBUTTON(
                                                                                                                ID_MIX_MUTE,
                                                                                                                TPMixer::
                                                                                                                    OnMixVolume)
                                                                                                                EVT_TOGGLEBUTTON(ID_MIX_MUTE + 1, TPMixer::
                                                                                                                                                      OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_MUTE + 2, TPMixer::
                                                                                                                                                                                                         OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_MUTE + 3, TPMixer::
                                                                                                                                                                                                                                                            OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_MUTE + 4, TPMixer::
                                                                                                                                                                                                                                                                                                               OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_MUTE +
                                                                                                                                                                                                                                                                                                                                                 5,
                                                                                                                                                                                                                                                                                                                                             TPMixer::
                                                                                                                                                                                                                                                                                                                                                 OnMixVolume)
                                                                                                                    EVT_TOGGLEBUTTON(
                                                                                                                        ID_MIX_MUTE +
                                                                                                                            6,
                                                                                                                        TPMixer::
                                                                                                                            OnMixVolume)
                                                                                                                        EVT_TOGGLEBUTTON(
                                                                                                                            ID_MIX_MUTE +
                                                                                                                                7,
                                                                                                                            TPMixer::
                                                                                                                                OnMixVolume)
                                                                                                                            EVT_TOGGLEBUTTON(
                                                                                                                                ID_MIX_MUTE +
                                                                                                                                    8,
                                                                                                                                TPMixer::
                                                                                                                                    OnMixVolume)
                                                                                                                                EVT_TOGGLEBUTTON(ID_MIX_MUTE +
                                                                                                                                                     9,
                                                                                                                                                 TPMixer::
                                                                                                                                                     OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_MUTE + 10, TPMixer::
                                                                                                                                                                                                         OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_MUTE +
                                                                                                                                                                                                                                           11,
                                                                                                                                                                                                                                       TPMixer::
                                                                                                                                                                                                                                           OnMixVolume)

                                                                                                                                    EVT_TOGGLEBUTTON(ID_MIX_PHASE,
                                                                                                                                                     TPMixer::
                                                                                                                                                         OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 1, TPMixer::
                                                                                                                                                                                                             OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 2, TPMixer::
                                                                                                                                                                                                                                                                 OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 3, TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 4,
                                                                                                                                                                                                                                                                                                                                                        TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 5, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                     OnMixVolume)
                                                                                                                                        EVT_TOGGLEBUTTON(ID_MIX_PHASE + 6, TPMixer::
                                                                                                                                                                               OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 7, TPMixer::
                                                                                                                                                                                                                                   OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 8, TPMixer::
                                                                                                                                                                                                                                                                                       OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE +
                                                                                                                                                                                                                                                                                                                         9,
                                                                                                                                                                                                                                                                                                                     TPMixer::
                                                                                                                                                                                                                                                                                                                         OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 10, TPMixer::OnMixVolume) EVT_TOGGLEBUTTON(ID_MIX_PHASE + 11, TPMixer::
                                                                                                                                                                                                                                                                                                                                                                                                                                        OnMixVolume)

                                                                                                                                            EVT_COMBOBOX(
                                                                                                                                                ID_OUTPUT_SEL,
                                                                                                                                                TPMixer::
                                                                                                                                                    OnOutputToggle)
                                                                                                                                                EVT_COMBOBOX(
                                                                                                                                                    ID_OUTPUT_SEL +
                                                                                                                                                        1,
                                                                                                                                                    TPMixer::
                                                                                                                                                        OnOutputToggle)

                                                                                                                                                    EVT_SLIDER(
                                                                                                                                                        ID_OUTPUT_VOL_L,
                                                                                                                                                        TPMixer::
                                                                                                                                                            OnOutputVolume)
                                                                                                                                                        EVT_SLIDER(
                                                                                                                                                            ID_OUTPUT_VOL_L +
                                                                                                                                                                1,
                                                                                                                                                            TPMixer::
                                                                                                                                                                OnOutputVolume)

                                                                                                                                                            EVT_SLIDER(
                                                                                                                                                                ID_OUTPUT_VOL_B, TPMixer::
                                                                                                                                                                                     OnOutputVolume) EVT_SLIDER(ID_OUTPUT_VOL_B +
                                                                                                                                                                                                                    1,
                                                                                                                                                                                                                TPMixer::OnOutputVolume)

                                                                                                                                                                EVT_SLIDER(
                                                                                                                                                                    ID_OUTPUT_VOL_R,
                                                                                                                                                                    TPMixer::
                                                                                                                                                                        OnOutputVolume)
                                                                                                                                                                    EVT_SLIDER(
                                                                                                                                                                        ID_OUTPUT_VOL_R +
                                                                                                                                                                            1,
                                                                                                                                                                        TPMixer::
                                                                                                                                                                            OnOutputVolume)

                                                                                                                                                                        EVT_TOGGLEBUTTON(ID_OUTPUT_MON,
                                                                                                                                                                                         TPMixer::OnOutputToggle) EVT_TOGGLEBUTTON(ID_OUTPUT_MON +
                                                                                                                                                                                                                                       1,
                                                                                                                                                                                                                                   TPMixer::
                                                                                                                                                                                                                                       OnOutputToggle)

                                                                                                                                                                            EVT_TOGGLEBUTTON(
                                                                                                                                                                                ID_OUTPUT_LINE,
                                                                                                                                                                                TPMixer::
                                                                                                                                                                                    OnOutputToggle)
                                                                                                                                                                                EVT_TOGGLEBUTTON(
                                                                                                                                                                                    ID_OUTPUT_LINE +
                                                                                                                                                                                        1,
                                                                                                                                                                                    TPMixer::
                                                                                                                                                                                        OnOutputToggle)

    // Loopback events
    EVT_COMBOBOX(ID_LOOP_SEL, TPMixer::OnLoopToggle)
        EVT_COMBOBOX(ID_LOOP_SEL + 1,
                     TPMixer::OnLoopToggle) EVT_COMBOBOX(ID_LOOP_SEL + 2,
                                                         TPMixer::OnLoopToggle)

            EVT_SLIDER(ID_LOOP_VOL_L, TPMixer::OnLoopVolume) EVT_SLIDER(
                ID_LOOP_VOL_L + 1,
                TPMixer::OnLoopVolume) EVT_SLIDER(ID_LOOP_VOL_L + 2,
                                                  TPMixer::OnLoopVolume)

                EVT_SLIDER(ID_LOOP_VOL_B, TPMixer::OnLoopVolume) EVT_SLIDER(
                    ID_LOOP_VOL_B +
                        1,
                    TPMixer::OnLoopVolume) EVT_SLIDER(ID_LOOP_VOL_B + 2,
                                                      TPMixer::OnLoopVolume)

                    EVT_SLIDER(ID_LOOP_VOL_R, TPMixer::OnLoopVolume) EVT_SLIDER(
                        ID_LOOP_VOL_R +
                            1,
                        TPMixer::OnLoopVolume) EVT_SLIDER(ID_LOOP_VOL_R + 2,
                                                          TPMixer::OnLoopVolume)

                        EVT_TOGGLEBUTTON(
                            ID_LOOP_MUTE,
                            TPMixer::
                                OnLoopToggle) EVT_TOGGLEBUTTON(ID_LOOP_MUTE + 1,
                                                               TPMixer::OnLoopToggle)
                            EVT_TOGGLEBUTTON(ID_LOOP_MUTE + 2,
                                             TPMixer::OnLoopToggle)
                                EVT_TOGGLEBUTTON(ID_LOOP_MUTE + 3,
                                                 TPMixer::OnLoopToggle)
                                    EVT_TOGGLEBUTTON(ID_LOOP_MUTE + 4,
                                                     TPMixer::OnLoopToggle)
                                        EVT_TOGGLEBUTTON(ID_LOOP_MUTE + 5,
                                                         TPMixer::OnLoopToggle)

                                            EVT_SLIDER(
                                                ID_PHONE_MIX,
                                                TPMixer::OnPhoneMix)
                                                EVT_SLIDER(
                                                    ID_PHONE_MIX +
                                                        1,
                                                    TPMixer::OnPhoneMix)

                                                    EVT_TOGGLEBUTTON(
                                                        ID_PHONE_GAIN,
                                                        TPMixer::OnPhoneGain)
                                                        EVT_TOGGLEBUTTON(
                                                            ID_PHONE_GAIN + 1,
                                                            TPMixer::
                                                                OnPhoneGain)
                                                            wxEND_EVENT_TABLE();

void TPMixer::scbUpdateLevels(uint16_t ch16, int32_t val) {
  uint8_t ch = ch16 >> 8;
  uint8_t subCh = ch16 & 0xff;
  int32_t siVal = val;
  int32_t level01DB = siVal; // It is 0.1dB  // / 50000;
  int32_t cls = ch & 0xf0;
  // printf("%d: %04x %08x\n", __LINE__, ch16, val);
  switch (cls) {
  case 0x20: // input levels
  {
    int32_t logicCh = ch - 0x21;
    int32_t vGauge = level01DB - panelInputs->LEVEL_MIN;
    switch (subCh) {
    case 0x01:
      panelInputs->cbMon[logicCh]->SetValue(val);
      hid->settings[0x2101 + (logicCh << 8)] = val;
      break;
    case 0x02:
      panelInputs->cb48V[logicCh]->SetValue(val);
      hid->settings[0x2102 + (logicCh << 8)] = val;
      break;
    case 0x03:
      if (panelInputs->cbInst[logicCh]) {
        panelInputs->cbInst[logicCh]->SetValue(val);
      }
      hid->settings[0x2103 + (logicCh << 8)] = val;
      break;
    case 0x04:
      if (vGauge < 0) {
        vGauge = 0;
      } else if (vGauge > panelInputs->LEVEL_RANGE) {
        vGauge = panelInputs->LEVEL_RANGE;
      }
      if (level01DB > panelInputs->PeaksI[logicCh]) {
        panelInputs->PeaksI[logicCh] = level01DB;
        panelInputs->lbPeaksI[logicCh]->SetLabel(
            wxString(std::format("{:+.1f}", level01DB * 0.1)));
      }
      panelInputs->gaLevels[logicCh]->SetValue(vGauge);

      vGauge = level01DB - panelMixers->LEVEL_MIN;
      if (vGauge < 0) {
        vGauge = 0;
      } else if (vGauge > panelMixers->LEVEL_RANGE) {
        vGauge = panelInputs->LEVEL_RANGE;
      }
      if (level01DB > panelMixers->PeaksI[logicCh]) {
        panelMixers->PeaksI[logicCh] = level01DB;
        // panelMixers->lbPeaksI[logicCh]->SetLabel(wxString(std::format("{:+.1f}",
        // level01DB * 0.1)));
      }
      panelMixers->gaLevel[logicCh]->SetValue(vGauge);
      break;
    case 0x05: // gain
      if (hid->pid == 0x8754) {
        int32_t gainDB = 0;
        if (0 == val) {
          gainDB = panelInputs->minGain;
          panelInputs->cbMute[logicCh]->SetValue(true);
        } else {
          if (val < 0) {
            val = -val;
            panelInputs->cbPhase[logicCh]->SetValue(true);
          } else {
            panelInputs->cbPhase[logicCh]->SetValue(false);
          }
          panelInputs->cbMute[logicCh]->SetValue(false);
          gainDB = round(log10((double)val / (double)0x02000000) * 20);
        }
        panelInputs->slGainI[logicCh]->SetValue(gainDB);
      }
      break;
    case 0x06: // MUTE (GUI-only)
      panelInputs->cbMute[logicCh]->SetValue(val ? true : false);
      hid->settings[0x2106 + (logicCh << 8)] = val;
      break;
    case 0x07: // SOLO (GUI-only)
      panelInputs->cbSolo[logicCh]->SetValue(val ? true : false);
      hid->settings[0x2107 + (logicCh << 8)] = val;
      break;
    case 0x08: // PHASE (GUI-only)
      panelInputs->cbPhase[logicCh]->SetValue(val ? true : false);
      hid->settings[0x2108 + (logicCh << 8)] = val;
      break;
    case 0x09: // Slider Gain (GUI-only)
      panelInputs->slGainI[logicCh]->SetValue(val);
      panelInputs->lbGainI[logicCh]->SetLabel(
          wxString(std::format("{:+}dB", val)));
      hid->settings[0x2109 + (logicCh << 8)] = val;
      break;
    default:
      break;
    } // switch subch
  } break;
  // Output levels!
  // channel: 0x31..0x34 for 1L, 1R, 2L, 2R;
  // subch 0x01,0x02: 01 for Output level, 02 for source Level
  case 0x30: {
    int32_t vGauge = level01DB - panelOutputs->LEVEL_MIN;
    int32_t logicCh = ch - 0x31;
    int32_t index = logicCh * 2 + subCh - 1;
    if (logicCh < 0 || logicCh >= 10)
      return;
    if (subCh < 1 || subCh > 4)
      return;
    switch (logicCh) {
    case 0x04:
    case 0x05:
      // phones
      switch (subCh) {
      case 1: // output source
        if (val <= 0 || val >= 15)
          break;
        printf("select output[%1d] source %1d\n", logicCh - 4, val);
        panelOutputs->cbSelect[logicCh - 4]->SetSelection(val - 1);
        break;
      case 2: // phone gain boost
        printf("set phone[%1d] gain %1d\n", logicCh - 4, val);
        panelPhones->ckPhoneGain[logicCh - 4]->SetValue(val);
        hid->settings[0x3502 + ((logicCh - 4) << 8)] = val;
        break;
      case 3: // phone mix
      {
        int phoneMix = (val - 50) * 2;
        printf("set phone[%1d] gain %1d\n", logicCh - 4, phoneMix);
        panelPhones->slPhoneMix[logicCh - 4]->SetValue(phoneMix);
      } break;
      } // phone/output control
      break;
    case 0x06: // 37, output control
    {
      int16_t lSubCh = subCh - 1;
      if (0 == (lSubCh / 2)) { // 1, 2
        panelOutputs->ckOutputMon[lSubCh % 2]->SetValue(val);
      } else {
        panelOutputs->ckOutputLine[lSubCh % 2]->SetValue(val);
      }
      printf("set output control [%1d] gain %1d\n", lSubCh, val);
    } break;
    case 0x08: // 0x39, Auto Standby
      if (subCh == 1) {
        panelDevice->cbAutoStandby->SetValue(val ? true : false);
        hid->settings[0x3901] = val;
      }
      break;
    case 0x09: // 0x3a, OTG Mode / Mobile Port
      if (subCh == 1 && panelDevice->cbOTGMode) {
        panelDevice->cbOTGMode->SetValue(val ? true : false);
        hid->settings[0x3a01] = val;
      }
      break;
    default: // 0..3: level meter
      if (logicCh < 0 || logicCh >= panelOutputs->N_OUTPUTS * 2)
        break;
      switch (subCh) {
      case 1: // output level meter after fader
      case 2: // output level meter before fader
        if (vGauge < 0) {
          vGauge = 0;
        } else if (vGauge > panelOutputs->LEVEL_RANGE) {
          vGauge = panelOutputs->LEVEL_RANGE;
        }
        if (level01DB > panelOutputs->PeaksI[logicCh]) {
          panelOutputs->PeaksI[index] = level01DB;
          // panelMixers->btPeaksI[logicCh]->SetLabel(wxString(std::format("{:+.1f}",
          // level01DB * 0.1)));
        }
        // TODO  what about sub ch 02?
        if (1 == subCh) {
          panelOutputs->gaLevelsO[logicCh]->SetValue(vGauge);
        } else {
          panelOutputs->gaLevelsI[logicCh]->SetValue(vGauge);
        }
        break;
      case 3: // output volume
      {
        // phone mix
        auto [muted, dummy, gainDB] = gain2dB(val);
        printf("set pohone[%1d] gain %1d\n", logicCh, val);
        // panelOutputs->ckMute[logicCh]->setValue(muted);
        if (0 == (logicCh % 2)) {
          panelOutputs->slOutputL[logicCh / 2]->SetValue(gainDB);
        } else {
          panelOutputs->slOutputR[logicCh / 2]->SetValue(gainDB);
        }
      } break;
      }
      break;
    }
  } break;   // class output/phone
  case 0x40: // playback levels, saw 0x41..0x48, subch always 0x01
  {
    int32_t vGauge = level01DB - panelMixers->LEVEL_MIN;
    int32_t logicBus = ch - 0x41;
    int32_t logicCh = subCh - 1;
    if (logicCh < 0 || logicCh > 8)
      return;
    if (subCh != 1)
      return;
    // printf("level ch  0x%02x, %5.1fdB\n", ch16, level01DB * 0.1);
    if (vGauge < 0) {
      vGauge = 0;
    } else if (vGauge > panelMixers->LEVEL_RANGE) {
      vGauge = panelMixers->LEVEL_RANGE;
    }
    if (level01DB > panelMixers->PeaksI[logicCh]) {
      panelMixers->PeaksI[logicBus + 4] = level01DB;
      // panelMixers->btPeaksI[logicCh]->SetLabel(wxString(std::format("{:+.1f}",
      // level01DB * 0.1)));
    }
    panelMixers->gaLevel[logicBus + 4]->SetValue(vGauge);
  } break; // class playback
  // Loopback levels, saw 0x55..0x56, subch  0x01,0x02
  // Loopback source selction: 0x57..0x59
  case 0x50: {
    int32_t vGauge = level01DB - panelOutputs->LEVEL_MIN;
    int32_t logicCh = ch - 0x51;
    int32_t indexMeter = logicCh * 2 + logicCh;
    int32_t gainDB = 0;
    if (logicCh < 0 || logicCh > 9)
      return;
    if (subCh < 1 || subCh > 5)
      return;
    if (vGauge < 0) {
      vGauge = 0;
    } else if (vGauge > panelOutputs->LEVEL_RANGE) {
      vGauge = panelOutputs->LEVEL_RANGE;
    }
    if (level01DB > panelOutputs->PeaksI[logicCh]) {
      panelOutputs->PeaksI[indexMeter] = level01DB;
      // panelOutputs->btPeaksI[logicCh]->SetLabel(wxString(std::format("{:+.1f}",
      // level01DB * 0.1)));
    }

    if (logicCh < 6) {
      // level meter
      // printf("level ch  0x%02x, %5.1fdB\n", ch16, level01DB * 0.1);
      switch (subCh) {
      case 1:
        panelLoopbacks->gaLevelsO[logicCh]->SetValue(vGauge);
        break;
      case 2:
        panelLoopbacks->gaLevelsI[logicCh]->SetValue(vGauge);
        break;
      case 3: // volume
        if (0 == val) {
          gainDB = panelLoopbacks->minGain;
          panelLoopbacks->ckMute[logicCh]->SetValue(true);
          printf("%d:\n", __LINE__);
        } else {
          if (val < 0) {
            val = -val;
            // no phase control
            // panelLoopbacks->cbPhase[logicCh]->SetValue(true);
          }
          panelLoopbacks->ckMute[logicCh]->SetValue(false);
          gainDB = round(log10((double)val / (double)0x02000000) * 20);
          // printf("%d: %9.6f  %4d\n", __LINE__, (double)val /
          // (double)0x02000000, gainDB);
        }
        printf("%d: level ch 0x%02x, L/R %d, val %08x,  %4ddB\n", __LINE__,
               ch16, logicCh % 2, val, gainDB);
        if (0 == (logicCh % 2)) { // left
          panelLoopbacks->slOutputL[logicCh / 2]->SetValue(gainDB);
        } else {
          panelLoopbacks->slOutputR[logicCh / 2]->SetValue(gainDB);
        }
        break;
      }
    } else {
      // source select
      if (logicCh - 6 >= 0 && logicCh - 6 < panelLoopbacks->N_LOOPBACKS) {
        panelLoopbacks->cbSelect[logicCh - 6]->SetSelection(val - 1);
      }
    }
  } break;
  case 0x60: {
    int32_t logicCh = ch - 0x61;
    if (logicCh < 0 || logicCh >= 8)
      return;
    if (subCh < 1 || subCh > 12)
      return;
    hid->settings[ch16] = val;
  } break;
  default:
    printf("level ch  0x%02x, %5.1fdB\n", ch16, level01DB * 0.1);
    break;
  } // switch class
}

bool MyApp::OnInit() {
  TPMixer *frame = new TPMixer();
  frame->Show(true);
  return true;
}

TPMixer::TPMixer() : wxFrame(nullptr, wxID_ANY, "Topping E4X4 Mixer") {
#if 1
  wxMenu *menuFile = new wxMenu;
  wxMenu *menuHelp = new wxMenu;
  wxMenu *menuDevice = new wxMenu;

  menuFile->Append(ID_SAVE, "&Save Status\tCtrl-S");
  menuFile->Append(ID_LOAD, "&Load Status\tCtrl-L");
  menuFile->AppendSeparator();
  menuFile->Append(wxID_EXIT);

  menuDevice->Append(ID_DEVICE_SAVE, "&Save Defaults");

  menuHelp->Append(wxID_ABOUT, "About\tF1");

  wxMenuBar *menuBar = new wxMenuBar;
  menuBar->Append(menuFile, "&File");
  menuBar->Append(menuDevice, "&Device");
  menuBar->Append(menuHelp, "&Help");
  SetMenuBar(menuBar);
#endif

  std::string home = getenv("HOME");
  dir1 = home + pathSep + dirConfig;
  dir2 = dir1 + pathSep + dirApp;
  fileCfg = dir2 + pathSep + ConfigFile;

  CreateStatusBar();

  hid = new ToppingHID();
  if (NULL != hid->getHandle()) {
    std::string deviceName = "Topping E4X4";
    if (hid->pid == 0x8755) {
      deviceName = "Topping E1X2 OTG";
    } else if (hid->pid == 0x8752) {
      deviceName = "Topping E2X2";
    } else if (hid->pid == 0x8756) {
      deviceName = "Topping E2X2 OTG";
    }
    SetStatusText(deviceName + " HID device opened.");
    SetTitle(deviceName + " Mixer");
  }
  gain = new Gain();

  Bind(wxEVT_MENU, &TPMixer::OnAbout, this, wxID_ABOUT);
  Bind(wxEVT_MENU, &TPMixer::OnExit, this, wxID_EXIT);

  SetMinSize(wxSize(820, 500));

  auto mainSizer = new wxBoxSizer(wxVERTICAL);

  auto book = new wxNotebook(this, wxID_ANY);

  panelInputs = new PanelInputs(book, hid->numInputs, hid->pid);
  panelMixers = new PanelMixers(book, hid->pid);
  panelLoopbacks = new PanelLoopbacks(book);
  panelOutputs = new PanelOutputs(book, hid->pid);
  panelPhones = new PanelPhones(book);
  panelDevice = new PanelDevice(book, hid->pid);

  panelDevice->cbAutoStandby->Bind(wxEVT_CHECKBOX, &TPMixer::OnDeviceToggle,
                                   this);
  if (panelDevice->cbOTGMode) {
    panelDevice->cbOTGMode->Bind(wxEVT_CHECKBOX, &TPMixer::OnDeviceToggle,
                                 this);
  }

  // Dynamic Labels for Input 3+4
  // Dynamic Labels & Feature Hiding for Input 3+4 & Output/Phones
  if (hid->pid == 0x8755 || hid->pid == 0x8756) {
    panelMixers->lbTitle[1]->SetLabel("Mobile In");
    panelPhones->lbTitle[1]->SetLabel("Mobile Out");
    panelPhones->ckPhoneGain[1]->Hide();

    // Hide Phase in Mixer tab
    for (int i = 0; i < panelMixers->N_MIX_SRCS; ++i) {
      panelMixers->ckPhase[i]->Hide();
    }

    // Hide unsupported controls in Input tab
    for (int i = 0; i < hid->numInputs; ++i) {
      panelInputs->cbMute[i]->Hide();
      panelInputs->cbSolo[i]->Hide();
      panelInputs->cbPhase[i]->Hide();
      panelInputs->slGainI[i]->Hide();
      panelInputs->lbGainI[i]->Hide();
    }
  } else if (hid->pid == 0x8752) {
    panelMixers->lbTitle[1]->SetLabel("Unused");

    panelPhones->lbTitle[1]->SetLabel("Unused");
    panelPhones->ckPhoneGain[1]->Hide();
    panelPhones->slPhoneMix[1]->Hide();

    // Hide Phase in Mixer tab
    for (int i = 0; i < panelMixers->N_MIX_SRCS; ++i) {
      panelMixers->ckPhase[i]->Hide();
    }

    // Hide unsupported controls in Input tab
    for (int i = 0; i < hid->numInputs; ++i) {
      panelInputs->cbMute[i]->Hide();
      panelInputs->cbSolo[i]->Hide();
      panelInputs->cbPhase[i]->Hide();
      panelInputs->slGainI[i]->Hide();
      panelInputs->lbGainI[i]->Hide();
    }
  }

  book->AddPage(panelInputs, "Input");
  book->AddPage(panelMixers, "Mix");
  book->AddPage(panelLoopbacks, "Loopback");
  book->AddPage(panelOutputs, "Output");
  book->AddPage(panelPhones, "Phones");
  book->AddPage(panelDevice, "Device");

  mainSizer->Add(book, 1, wxEXPAND | wxALL, 8);
  SetSizer(mainSizer);

  loadSettings();
  startHidReader();
  if (NULL != hid->getHandle()) {
    if (hid->pid == 0x8754) {
      hid->requestDeviceDump();
    } else {
      CallAfter(&TPMixer::pushGuiStateToDevice);
    }
  }
}

void createDir(const std::string path) {
  printf("checking dir '%s'\n", path.c_str());
  if (!std::filesystem::exists(path)) { // Check if src folder exists
    printf("creating '%s'\n", path.c_str());
    try {
      std::filesystem::create_directory(path); // create src folder
    } catch (std::exception &e) {
      std::cout << e.what();
    }
  }
}

void TPMixer::saveSettings() {

  createDir(dir1);
  createDir(dir2);
  FILE *f = fopen(fileCfg.c_str(), "w");

  if (NULL != f) {
    printf("Setting file opened. Saving...\n");
    for (const auto &[key, value] : hid->settings) {
      fprintf(f, "%04x %08x\n", key, value);
    }
    printf("Finished\n");
    fclose(f);
  } else {
    printf("Fail to open settings file.\n");
  }
}

void TPMixer::loadSettings() {
  hid->initializeSettingsWithDefaults();
  FILE *f = fopen(fileCfg.c_str(), "r");
  uint16_t key = 0;
  int32_t value = 0;
  ssize_t n;
  size_t len = 0;
  char *line = NULL;
  if (NULL != f) {
    printf("setting file opened. reading...\n");
    while ((n = getline(&line, &len, f)) != -1) {
      sscanf(line, "%" SCNx16 " %" SCNx32, &key, &value);
      printf("0x%04x 0x%08x\n", key, value);
      hid->settings[key] = value;
      // TODO check class/channel...
      CallAfter(&TPMixer::scbUpdateLevels, key, value);
    }
    printf("Finished\n");
    if (NULL != line) {
      free(line);
    }
    fclose(f);
    CallAfter(&TPMixer::refreshMixerUi, -1);
    CallAfter(&TPMixer::refreshLoopbackUi);
    CallAfter(&TPMixer::refreshOutputUi);
  } else {
    printf("Fail to open settings file.\n");
  }
}

void TPMixer::pushGuiStateToDevice() {
  if (NULL == hid->getHandle())
    return;

  // 1. Push Inputs (MON, 48V, INST, and Gain/Mute/Solo/Phase)
  for (int i = 0; i < hid->numInputs; ++i) {
    hid->setInputMon(i, panelInputs->cbMon[i]->GetValue());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    hid->setInput48V(i, panelInputs->cb48V[i]->GetValue());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    if (panelInputs->cbInst[i]) {
      hid->setInputInst(i, panelInputs->cbInst[i]->GetValue());
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    // Pushes Gain, Mute, Solo, Phase
    sendInput(i, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // 2. Push Output configuration
  for (int i = 0; i < 2; ++i) {
    int val = panelOutputs->cbSelect[i]->GetSelection();
    hid->setOutputSel(i, val + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // Output Mon and Line
  for (int i = 0; i < 2; ++i) {
    hid->setOutputMon(i, panelOutputs->ckOutputMon[i]->GetValue());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    hid->setOutputLine(i, panelOutputs->ckOutputLine[i]->GetValue());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // 3. Push Phones mix & Phones gain
  for (int i = 0; i < 2; ++i) {
    int valGain = panelPhones->ckPhoneGain[i]->GetValue() ? 1 : 0;
    hid->setPhoneGainBoost(i, valGain);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    int valMix = panelPhones->slPhoneMix[i]->GetValue();
    int devMix = (valMix / 2) + 50; // map back to 0..100
    hid->setPhoneMix(i, devMix);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // 4. Push Output levels
  for (int i = 0; i < panelOutputs->N_OUTPUTS; ++i) {
    setOutputVol(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // 5. Push Loopback source selection and levels
  for (int i = 0; i < panelLoopbacks->N_LOOPBACKS; ++i) {
    int valSel = panelLoopbacks->cbSelect[i]->GetSelection();
    hid->setLoopSel(i, valSel + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    setLoopVol(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // 6. Push Mixers
  int bus = panelMixers->rboxMixerSelBox->GetSelection();
  for (int ch = 0; ch < panelMixers->N_MIX_SRCS; ++ch) {
    int pan = panelMixers->slPan[ch]->GetValue();
    bool mute = panelMixers->ckMute[ch]->GetValue();
    bool solo = panelMixers->ckSolo[ch]->GetValue();
    bool phase = panelMixers->ckPhase[ch]->GetValue();
    int gainDB = panelMixers->slVol[ch]->GetValue();

    bool anySolo = false;
    for (int i = 0; i < panelMixers->N_MIX_SRCS; ++i) {
      if (panelMixers->ckSolo[i]->GetValue())
        anySolo = true;
    }

    auto [gainL, gainR] =
        gain->getStereoGain(gainDB, mute, solo, anySolo, phase, pan);
    hid->setMixVol(bus, ch, gainL, gainR);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // 7. Push Device Settings
  int valStandby = panelDevice->cbAutoStandby->GetValue() ? 1 : 0;
  hid->setDeviceSetting(0x39, 0x01, valStandby);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));

  if (panelDevice->cbOTGMode) {
    int valOTG = panelDevice->cbOTGMode->GetValue() ? 1 : 0;
    hid->setDeviceSetting(0x3a, 0x01, valOTG);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
}

void TPMixer::refreshMixerUi(int16_t bus) {
  if (bus < 0) {
    bus = panelMixers->rboxMixerSelBox->GetSelection();
  }
  for (int16_t i = 0; i < panelMixers->N_MIX_SRCS; i++) {
    int32_t nGains = 0;
    int32_t gains[2] = {0};
    for (int16_t j = 0; j < 2; j++) {
      int16_t key = 0x6101 + ((j + bus * 2) << 8) + i;
      if (hid->settings.contains(key)) {
        nGains++;
        gains[j] = hid->settings[key];
      }
    }
    if (2 == nGains) {
      auto [muted, phase, pan, gainDB] =
          lrGain2PandB(gains[0], gains[1], panelMixers->minGain);
      panelMixers->slPan[i]->SetValue(pan);
      panelMixers->slVol[i]->SetValue(gainDB);
      panelMixers->ckMute[i]->SetValue(muted);
      panelMixers->ckPhase[i]->SetValue(phase);
      // printf("line %d: src %1x, %08x %08x, pan %4d, gain %4d\n", __LINE__, i,
      // gains[0], gains[1], pan, gainDB);
    }
  }
  for (int16_t i = 0; i < panelMixers->N_MIX_SRCS / 2; i++) {
    int16_t a = i * 2;
    int16_t b = a + 1;

    int32_t gainA = panelMixers->slVol[a]->GetValue();
    int32_t gainB = panelMixers->slVol[b]->GetValue();
    if (gainA > gainB) {
      panelMixers->slVolB[i]->SetValue(gainA);
    } else {
      panelMixers->slVolB[i]->SetValue(gainB);
    }
  }
}

void TPMixer::refreshLoopbackUi() {
  for (int16_t i = 0; i < panelLoopbacks->N_LOOPBACKS; i++) {
    int32_t gainL = panelLoopbacks->slOutputL[i]->GetValue();
    int32_t gainR = panelLoopbacks->slOutputR[i]->GetValue();
    int32_t max = 0;
    if (gainL > gainR) {
      max = gainL;
    } else {
      max = gainR;
    }
    panelLoopbacks->slOutputB[i]->SetValue(max);
    printf("line %d: checking [%d], %4d, %4d, set to %d\n", __LINE__, i, gainL,
           gainR, panelLoopbacks->slOutputB[i]->GetValue());
  }
}

void TPMixer::refreshOutputUi() {
  for (int16_t i = 0; i < panelOutputs->N_OUTPUTS; i++) {
    int32_t gainL = panelLoopbacks->slOutputL[i]->GetValue();
    int32_t gainR = panelLoopbacks->slOutputR[i]->GetValue();
    if (gainL > gainR) {
      panelLoopbacks->slOutputB[i]->SetValue(gainL);
    } else {
      panelLoopbacks->slOutputB[i]->SetValue(gainR);
    }
  }
}

//=========== Input handlers ==========
void TPMixer::OnInputGain(wxCommandEvent &event) {
  // int32_t val = event.GetInt();
  int32_t evtId = event.GetId();
  int32_t ch = evtId & 0x0f;
  assert(ch < panelInputs->N_INPUTS);
  // printf("%s(): event %04x, Input [%2d]\n", __func__, id, ch);
  switch (evtId & (~0xf)) {
  case ID_INPUT_GAIN:
  case ID_INPUT_SOLO:
  case ID_INPUT_MUTE:
  case ID_INPUT_PHASE:
    sendInput(ch, true, evtId);
    break;
  case ID_INPUT_48V:
    hid->setInput48V(ch, panelInputs->cb48V[ch]->GetValue());
    hid->settings[0x2102 + (ch << 8)] =
        panelInputs->cb48V[ch]->GetValue() ? 1 : 0;
    break;
  case ID_INPUT_MON:
    hid->setInputMon(ch, panelInputs->cbMon[ch]->GetValue());
    hid->settings[0x2101 + (ch << 8)] =
        panelInputs->cbMon[ch]->GetValue() ? 1 : 0;
    break;
  case ID_INPUT_INST:
    hid->setInputInst(ch, panelInputs->cbInst[ch]->GetValue());
    hid->settings[0x2103 + (ch << 8)] =
        panelInputs->cbInst[ch]->GetValue() ? 1 : 0;
    break;
  default:
    break;
  }
}

void TPMixer::OnInputPeak(wxCommandEvent &event) {
  int32_t id = event.GetId();
  uint32_t ch = id - ID_INPUT_PEAK;

  panelInputs->lbPeaksI[ch]->SetLabel(
      wxString(std::format("{:+.1f}", panelInputs->LEVEL_MIN * 0.1)));
  panelInputs->PeaksI[ch] = panelInputs->LEVEL_MIN;
}

//=========== Mixer handlers ==========
void TPMixer::OnMixBusSel(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t val = event.GetInt();
  printf("%s(): %04x, %2d\n", __func__, id, val);
  refreshMixerUi(val);
}

void TPMixer::OnMixVolume(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();
  int32_t bus = panelMixers->rboxMixerSelBox->GetSelection();

  int32_t srcs[2] = {ch, ch};

  bool update[2] = {false, false};
  assert(ch < panelMixers->N_MIX_SRCS);
  printf("%s(): %04x, %5d, bus %2d\n", __func__, id, val, bus);
  switch (id & (~0xf)) {
  case ID_MIX_MUTE:
  case ID_MIX_PHASE:
  case ID_MIX_VOL:
  case ID_MIX_PAN:
    srcs[0] = ch;
    srcs[1] = ch;
    // printf("ch [%2d] to %+4d\n", ch, val);
    update[0] = true;
    break;
  case ID_MIX_SOLO:
    srcs[0] = ch * 2;
    srcs[1] = srcs[0] + 1;
    update[0] = true;
    update[1] = true;
    break;
  case ID_MIX_VOL_B:
    srcs[0] = ch * 2;
    srcs[1] = srcs[0] + 1;
    panelMixers->slVol[srcs[0]]->SetValue(val);
    panelMixers->slVol[srcs[1]]->SetValue(val);
    update[0] = true;
    update[1] = true;
    // printf("set left and right [%2d,%2d] to %+4d\n", srcs[0], srcs[1], val);
    break;
  default:
    break;
  }
  if (update[0] || update[1]) {

    int32_t gainL = 0, gainR = 0;
    int32_t pan = 0;
    bool mute = false, solo = false, anySolo = false, phase = false;
    int gainDB = 0;
    for (int32_t i = 0; !anySolo && (i < panelMixers->N_MIX_SRCS); i++) {
      if (panelMixers->ckSolo[i]->GetValue()) {
        anySolo = true;
      }
    }
    for (int32_t i = 0; i < 2; i++) {
      if (update[i]) {
        pan = panelMixers->slPan[srcs[i]]->GetValue();
        mute = panelMixers->ckMute[srcs[i]]->GetValue();
        solo = panelMixers->ckSolo[srcs[i]]->GetValue();
        phase = panelMixers->ckPhase[srcs[i]]->GetValue();
        gainDB = panelMixers->slVol[srcs[i]]->GetValue();
        std::tie(gainL, gainR) =
            gain->getStereoGain(gainDB, mute, solo, anySolo, phase, pan);
        hid->setMixVol(bus, srcs[i], gainL, gainR);
        hid->settings[((0x61 + bus * 2) << 8) | (srcs[i] + 1)] = gainL;
        hid->settings[((0x62 + bus * 2) << 8) | (srcs[i] + 1)] = gainR;
      }
    }
  }
}

void TPMixer::OnMixToggle(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();
  assert(ch < panelMixers->N_MIX_SRCS);

  printf("%s(): %04x, %5d\n", __func__, id, val);
  switch (id & (~0xf)) {
  case ID_MIX_SOLO:
  case ID_MIX_MUTE: {
  } break;
  case ID_MIX_PHASE: {
    panelOutputs->slOutputL[ch]->SetValue(val);
    panelOutputs->slOutputR[ch]->SetValue(val);
  } break;
  }
}

//=========== Loopback handlers ==========
void TPMixer::OnLoopVolume(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();
  assert(ch < panelLoopbacks->N_LOOPBACKS * 2);
  printf("%s(): %04x, %5d\n", __func__, id, val);
  bool toSetVolume = false;

  // printf("%s(): %04x, %5d\n", __func__, id, val);
  switch (id & (~0xf)) {
  case ID_LOOP_VOL_L:
    toSetVolume = true;
    break;
  case ID_LOOP_VOL_R:
    toSetVolume = true;
    break;
  case ID_LOOP_VOL_B:
    panelLoopbacks->slOutputL[ch]->SetValue(val);
    panelLoopbacks->slOutputR[ch]->SetValue(val);
    toSetVolume = true;
    break;
  default:
    break;
  }
  if (toSetVolume) {
    setLoopVol(ch);
  }
}

void TPMixer::OnLoopToggle(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();
  assert(ch < panelLoopbacks->N_LOOPBACKS * 2);

  printf("%s(): %04x, %5d\n", __func__, id, val);
  switch (id & (~0xf)) {
  case ID_LOOP_SEL:
    hid->setLoopSel(ch, val + 1);
    break;
  case ID_LOOP_MUTE:
    setLoopVol(ch / 2);
    break;
  default:
    break;
  }
}

//=========== Output  handlers ==========
void TPMixer::OnOutputVolume(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();
  assert(ch < panelOutputs->N_OUTPUTS);
  printf("%s(): %04x, %5d\n", __func__, id, val);
  bool toSetVolume = false;
  switch (id & (~0xf)) {
  case ID_OUTPUT_VOL_L:
    toSetVolume = true;
    break;
  case ID_OUTPUT_VOL_R:
    toSetVolume = true;
    break;
  case ID_OUTPUT_VOL_B:
    panelOutputs->slOutputL[ch]->SetValue(val);
    panelOutputs->slOutputR[ch]->SetValue(val);
    toSetVolume = true;
    break;
  default:
    break;
  }
  // TODO phase
  if (toSetVolume) {
    setOutputVol(ch);
  }
}

void TPMixer::OnOutputToggle(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();
  assert(ch < panelOutputs->N_OUTPUTS);

  // printf("%s(): %04x, %5d\n", __func__, id, val);
  switch (id & (~0xf)) {
  case ID_OUTPUT_SEL:
    hid->setOutputSel(ch, val + 1);
    break;
  case ID_OUTPUT_MON:
    hid->setOutputMon(ch, val);
    break;
  case ID_OUTPUT_LINE:
    hid->setOutputLine(ch, val);
    break;
  }
}

//=========== Phone handlers ==========
void TPMixer::OnPhoneMix(wxCommandEvent &event) {
  int32_t val = event.GetInt();
  int32_t id = event.GetId();
  uint32_t ch = id - ID_PHONE_MIX;

  SetStatusText(std::format("Phone mix[{}] to {}", ch, val));
  hid->setPhoneMix(ch, val);
}

void TPMixer::OnPhoneGain(wxCommandEvent &event) {
  int32_t val = event.GetInt();
  int32_t id = event.GetId();
  uint32_t ch = id - ID_PHONE_GAIN;

  SetStatusText(std::format("Phone gain[{}] to {}", ch, val));
  hid->setPhoneGainBoost(ch, val);
}

void TPMixer::OnExit(wxCommandEvent &event) { Close(true); }

void TPMixer::OnClose(wxCloseEvent &event) {
  // printf("Stopping thread %p\n", thReader);

  toStopHidReader = true;
  if (thReader && thReader->joinable()) {
    thReader->join();
  }
  delete thReader;
  thReader = nullptr;

  saveSettings();

  delete hid;
  hid = nullptr;

  delete gain;
  gain = nullptr;

  // printf("veto?\n");
  // event.Veto();
  event.Skip();
}

void TPMixer::OnLoad(wxCommandEvent &event) {
  loadSettings();
  if (NULL != hid->getHandle()) {
    if (hid->pid == 0x8754) {
      hid->requestDeviceDump();
    } else {
      CallAfter(&TPMixer::pushGuiStateToDevice);
    }
  }
}

void TPMixer::OnSave(wxCommandEvent &event) { saveSettings(); }

void TPMixer::OnDeviceSave(wxCommandEvent &event) { hid->saveDeviceDefault(); }

void TPMixer::OnAbout(wxCommandEvent &event) {
  wxMessageBox("This is Topping Mixer GUI Controller with wxWidgets",
               "About tpmix", wxOK | wxICON_INFORMATION);
}

void TPMixer::OnDeviceToggle(wxCommandEvent &event) {
  if (NULL == hid->getHandle())
    return;

  bool val = event.IsChecked();
  int32_t val32 = val ? 1 : 0;

  if (event.GetEventObject() == panelDevice->cbAutoStandby) {
    hid->setDeviceSetting(0x39, 0x01, val32);
    hid->settings[0x3901] = val32;
  } else if (panelDevice->cbOTGMode &&
             event.GetEventObject() == panelDevice->cbOTGMode) {
    hid->setDeviceSetting(0x3a, 0x01, val32);
    hid->settings[0x3a01] = val32;
  }
}
