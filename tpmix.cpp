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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <math.h>
#include <set>
#include <thread>
#include <tuple>

#include <wx/combobox.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/statline.h>
#include <wx/tglbtn.h>
#include <wx/wx.h>

#include <hidapi/hidapi.h>
#include <inttypes.h>

// Helper functions for HID buffer manipulation
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

double g_uiScale = 1.0;
void ScaleUIElements(wxWindow *win, double scale);

// Gain calculation class
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

// HID communication class
class ToppingHID {
public:
  uint16_t vid = 0x152a;
  uint16_t pid = 0x8754;
  int32_t numInputs = 4;
  std::map<uint16_t, int32_t> settings;
  uint8_t phoneRegOffset() const { return 0x35; }
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

  void initializeSettingsWithDefaults(bool force = false) {
    // 1. Inputs (ch 0..3)
    for (int i = 0; i < 4; ++i) {
      uint16_t base = 0x2100 + (i << 8);
      if (force || !settings.contains(base + 1))
        settings[base + 1] = 0; // MON
      if (force || !settings.contains(base + 2))
        settings[base + 2] = 0; // 48V
      if (force || !settings.contains(base + 3))
        settings[base + 3] = 0; // INST
      if (force || !settings.contains(base + 5))
        settings[base + 5] = 0x02000000; // Gain (0 dB)
      if (force || !settings.contains(base + 6))
        settings[base + 6] = 0; // MUTE (Off)
      if (force || !settings.contains(base + 7))
        settings[base + 7] = 0; // SOLO (Off)
      if (force || !settings.contains(base + 8))
        settings[base + 8] = 0; // PHASE (Off)
      if (force || !settings.contains(base + 9))
        settings[base + 9] = 0; // LOOP Gain
    }

    // 2. Mixers (bus 0..3, src 0..11)
    for (int bus = 0; bus < 4; ++bus) {
      for (int src = 0; src < 12; ++src) {
        uint16_t keyL = ((0x61 + bus * 2) << 8) | (src + 1);
        uint16_t keyR = ((0x62 + bus * 2) << 8) | (src + 1);
        int32_t defaultL = (src % 2 == 0) ? 0x02000000 : 0;
        int32_t defaultR = (src % 2 == 1) ? 0x02000000 : 0;
        if (force || !settings.contains(keyL))
          settings[keyL] = defaultL;
        if (force || !settings.contains(keyR))
          settings[keyR] = defaultR;
      }
    }

    // 3. Loopbacks (ch 0..5)
    for (int i = 0; i < 6; ++i) {
      uint16_t key = ((0x51 + i) << 8) | 3;
      if (force || !settings.contains(key))
        settings[key] = 0x02000000; // Loopback Vol (0 dB)
    }
    // Loopback source select
    for (int i = 0; i < 3; ++i) {
      uint16_t key = ((0x57 + i) << 8) | 1;
      if (force || !settings.contains(key))
        settings[key] = 1;
    }

    // 4. Outputs (ch 0..3)
    for (int i = 0; i < 4; ++i) {
      uint16_t key = ((0x31 + i) << 8) | 3;
      if (force || !settings.contains(key))
        settings[key] = 0x02000000; // Output Vol (0 dB)
    }
    // Output select for phones (ch 4..5)
    uint8_t phoneBase = phoneRegOffset();
    for (int i = 0; i < 2; ++i) {
      uint16_t key = ((phoneBase + i) << 8) | 1;
      if (force || !settings.contains(key))
        settings[key] = 7; // Default source 7 (Playback 1+2)

      uint16_t keyGain = ((phoneBase + i) << 8) | 2;
      if (force || !settings.contains(keyGain))
        settings[keyGain] = 0; // Phone gain boost off

      uint16_t keyMix = ((phoneBase + i) << 8) | 3;
      if (force || !settings.contains(keyMix))
        settings[keyMix] = 50; // Default Monitor Mix: 50 (Center, 0 dB)
    }

    // 5. Device Settings
    if (force || !settings.contains(0x3701))
      settings[0x3701] = 1; // Phone 1 default: ON
    if (force || !settings.contains(0x3702))
      settings[0x3702] = 1; // Phone 2 default: ON
    if (force || !settings.contains(0x3703))
      settings[0x3703] = 1; // TRS default: ON
    if (force || !settings.contains(0x3704))
      settings[0x3704] = 1; // AUX default: ON

    if (force || !settings.contains(0x3901))
      settings[0x3901] = 1; // Auto Standby default: ON
    if (force || !settings.contains(0x1101))
      settings[0x1101] = 1; // Auto Standby default: ON for non-E4X4
    if (force || !settings.contains(0x3a01))
      settings[0x3a01] = 0; // OTG Mode default: OFF
    if (force || !settings.contains(0x1103))
      settings[0x1103] = 0; // OTG Mode default: OFF for non-E4X4
    if (force || !settings.contains(0x1104))
      settings[0x1104] = 1; // LED Brightness default: Medium (1)

    // 6. GUI Link Settings (default values)
    if (!settings.contains(0x9000))
      settings[0x9000] = 1;
    if (!settings.contains(0x9001))
      settings[0x9001] = 1; // Auto save workspace default: ON
    for (int i = 0; i < 6; ++i) {
      if (!settings.contains(0x9100 + i)) {
        settings[0x9100 + i] =
            (i == 0) ? 0 : 1; // Column 0 (IN 1) unlinked by default!
      }
    }
    for (int i = 0; i < 3; ++i) {
      if (!settings.contains(0x9200 + i))
        settings[0x9200 + i] = 1;
      if (!settings.contains(0x9300 + i))
        settings[0x9300 + i] = 1;
    }
  }

  void setInputGainiI32(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x05;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };
  void setInput48V(int16_t ch, bool pOn, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x02;
    int32_t on32 = pOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };
  void setInputMon(int16_t ch, bool MonOn, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x01;
    int32_t on32 = MonOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };
  void setInputInst(int16_t ch, bool InstOn, bool exec = true) {
    buf[5] = 0x21 + ch;
    buf[6] = 0x03;
    int32_t on32 = InstOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };
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
  void setLoopSel(int16_t ch, int32_t sel, bool exec = true) {
    buf[5] = 0x57 + ch;
    buf[6] = 0x01;
    write32BE(&buf[7], sel);
    enqueue(exec);
  };
  void setLoopVol(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x51 + ch;
    buf[6] = 0x03;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };
  void setOutputSel(int16_t ch, int32_t sel, bool exec = true) {
    buf[5] = 0x35 + ch;
    buf[6] = 0x01;
    write32BE(&buf[7], sel);
    enqueue(exec);
  };
  void setOutputVol(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = 0x31 + ch;
    buf[6] = 0x03;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };
  void setOutputMon(int16_t ch, bool on, bool exec = true) {
    buf[5] = 0x37;
    buf[6] = 0x01 + ch;
    write32BE(&buf[7], on ? 1 : 0);
    enqueue(exec);
  };
  void setOutputLine(int16_t ch, bool on, bool exec = true) {
    buf[5] = 0x37;
    buf[6] = 0x03 + ch;
    write32BE(&buf[7], on ? 1 : 0);
    enqueue(exec);
  };
  void setPhoneMix(int16_t ch, int32_t mix, bool exec = true) {
    buf[5] = phoneRegOffset() + ch;
    buf[6] = 0x03;
    write32BE(&buf[7], (mix + 100) / 2);
    enqueue(exec);
  };
  void setPhoneGainBoost(int16_t ch, int32_t gain, bool exec = true) {
    buf[5] = phoneRegOffset() + ch;
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

  void setDeviceSetting(uint8_t zone, uint8_t ctrl, int32_t value,
                        bool exec = true) {
    buf[5] = zone;
    buf[6] = ctrl;
    write32BE(&buf[7], value);
    enqueue(exec);
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

// IDs range
enum {
  ID_SETTINGS = 0x0002,
  ID_SAVE = 0x0010,
  ID_LOAD = 0x0001,
  ID_DEVICE_SAVE = 0x0020,
  ID_RESET_DEFAULTS = 0x9990,

  ID_INPUT_GAIN = 0x220,
  ID_INPUT_48V = 0x240,
  ID_INPUT_MON = 0x250,
  ID_INPUT_INST = 0x260,
  ID_INPUT_SOLO = 0x270,
  ID_INPUT_MUTE = 0x280,
  ID_INPUT_PHASE = 0x290,
  ID_INPUT_PEAK = 0x2a0,
  ID_INPUT_LINK = 0x2b0,

  ID_MIX_BUS_SEL = 0x610,
  ID_MIX_VOL = 0x620,
  ID_MIX_VOL_B = 0x630,
  ID_MIX_PAN = 0x640,
  ID_MIX_SOLO = 0x670,
  ID_MIX_MUTE = 0x680,
  ID_MIX_PHASE = 0x690,
  ID_MIX_LINK = 0x6b0,

  ID_OUTPUT_SEL = 0x310,
  ID_OUTPUT_VOL_L = 0x320,
  ID_OUTPUT_VOL_B = 0x330,
  ID_OUTPUT_VOL_R = 0x340,
  ID_OUTPUT_MON = 0x350,
  ID_OUTPUT_LINE = 0x360,
  ID_OUTPUT_LINK = 0x3b0,

  ID_LOOP_SEL = 0x510,
  ID_LOOP_VOL_L = 0x520,
  ID_LOOP_VOL_B = 0x530,
  ID_LOOP_VOL_R = 0x540,
  ID_LOOP_MUTE = 0x550,
  ID_LOOP_LINK = 0x5b0,

  ID_PHONE_MIX = 0x370,
  ID_PHONE_GAIN = 0x380,
  ID_PHONE_SEL = 0x390,
};

// Directory helper
void createDir(const std::string path) {
  if (!std::filesystem::exists(path)) {
    try {
      std::filesystem::create_directory(path);
    } catch (...) {
    }
  }
}

// ----------------------------------------------------------------------------
// Custom Controls
// ----------------------------------------------------------------------------

enum IconType {
  ICON_NONE,
  ICON_GEAR,
  ICON_SAVE,
  ICON_DOWNLOAD,
  ICON_HEADPHONE
};

class CustomButton : public wxWindow {
public:
  wxSize m_designSize;
  double m_designFontPoint;

  CustomButton(wxWindow *parent, wxWindowID id, const wxString &label,
               bool isToggle = false, const wxSize &size = wxDefaultSize,
               const wxColour &activeCol = wxColour(29, 115, 201),
               IconType iconType = ICON_NONE)
      : wxWindow(parent, id, wxDefaultPosition, size, wxBORDER_NONE),
        m_label(label), m_isToggle(isToggle), m_isChecked(false),
        m_isPressed(false), m_isHovered(false), m_activeCol(activeCol),
        m_iconType(iconType) {
    m_designSize = size;
    if (m_designSize.x <= 0 || m_designSize.y <= 0) {
      m_designSize = wxSize(45, 14);
    }
    m_designFontPoint = 8.0;
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT, &CustomButton::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &CustomButton::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &CustomButton::OnLeftUp, this);
    Bind(wxEVT_MOTION, &CustomButton::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &CustomButton::OnLeave, this);
    Bind(wxEVT_ENTER_WINDOW, &CustomButton::OnEnter, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &CustomButton::OnMouseCaptureLost, this);
  }

  void SetValue(bool value) {
    if (m_isChecked != value) {
      m_isChecked = value;
      Refresh();
    }
  }

  bool GetValue() const { return m_isChecked; }

  void SetLabel(const wxString &label) override {
    m_label = label;
    wxWindow::SetLabel(label);
    Refresh();
  }

  void Rescale(double scale) {
    SetMinSize(wxSize(m_designSize.x * scale, m_designSize.y * scale));
    if (m_designSize.x <= 18) {
      SetMaxSize(wxSize(-1, m_designSize.y * scale));
    } else {
      SetMaxSize(wxSize(m_designSize.x * scale, m_designSize.y * scale));
    }
  }

private:
  wxString m_label;
  bool m_isToggle;
  bool m_isChecked;
  bool m_isPressed;
  bool m_isHovered;
  wxColour m_activeCol;
  IconType m_iconType;

  void OnPaint(wxPaintEvent &evt) {
    if (GetParent()) {
      SetBackgroundColour(GetParent()->GetBackgroundColour());
    }
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
    if (!gc)
      return;

    gc->Scale(g_uiScale, g_uiScale);
    wxSize sz = GetSize();
    double dx = sz.x / g_uiScale;
    double dy = sz.y / g_uiScale;

    // Background based on state
    wxColour bgCol;
    if (m_isToggle && m_isChecked) {
      bgCol = m_activeCol;
    } else if (m_isPressed) {
      bgCol = wxColour(40, 40, 40);
    } else if (m_isHovered) {
      bgCol = wxColour(60, 60, 60);
    } else {
      bgCol = wxColour(44, 44, 44);
    }
    if (!IsEnabled()) {
      bgCol = wxColour((bgCol.Red() + 33) / 2, (bgCol.Green() + 33) / 2,
                       (bgCol.Blue() + 33) / 2);
    }

    gc->SetBrush(wxBrush(bgCol));
    gc->SetPen(wxPen(wxColour(24, 24, 24), 1));
    gc->DrawRoundedRectangle(0, 0, dx, dy, 4);

    double cx = dx / 2.0;
    double cy = dy / 2.0;

    if (m_iconType == ICON_NONE) {
      // Draw standard text label
      wxColour textCol = (m_isToggle && m_isChecked) ? wxColour(255, 255, 255)
                                                     : wxColour(180, 180, 180);
      if (!IsEnabled()) {
        textCol = wxColour(100, 100, 100);
      }
      wxFont font = GetFont();
      double fontScale = 1.0 + (g_uiScale - 1.0) * 0.4;
      if (fontScale < 0.8)
        fontScale = 0.8;
      if (fontScale > 1.25)
        fontScale = 1.25;

      int ptSize = std::max(6, (int)(8 * fontScale));
      if (dy < 14.0 * g_uiScale)
        ptSize = std::max(5, (int)(7 * fontScale));
      font.SetPointSize(ptSize);
      gc->SetFont(font, textCol);

      double tw, th, td, tel;
      gc->GetTextExtent(m_label, &tw, &th, &td, &tel);
      gc->DrawText(m_label, (dx - tw) / 2.0, (dy - th) / 2.0);
    } else {
      // Draw Vector Icon
      wxColour iconCol =
          (m_isToggle && m_isChecked) ? *wxWHITE : wxColour(220, 220, 220);
      if (!IsEnabled()) {
        iconCol = wxColour(100, 100, 100);
      }
      gc->SetPen(wxPen(iconCol, 1.5));
      gc->SetBrush(*wxTRANSPARENT_BRUSH);

      if (m_iconType == ICON_GEAR) {
        double rOuter = 5.5;
        double rInner = 2.5;
        gc->DrawEllipse(cx - rOuter, cy - rOuter, rOuter * 2, rOuter * 2);
        gc->SetBrush(wxBrush(bgCol));
        gc->DrawEllipse(cx - rInner, cy - rInner, rInner * 2, rInner * 2);

        gc->SetPen(wxPen((m_isToggle && m_isChecked) ? *wxWHITE
                                                     : wxColour(220, 220, 220),
                         1.6));
        for (int i = 0; i < 8; ++i) {
          double angle = i * M_PI / 4.0;
          double x1 = cx + rOuter * cos(angle);
          double y1 = cy + rOuter * sin(angle);
          double x2 = cx + (rOuter + 2.0) * cos(angle);
          double y2 = cy + (rOuter + 2.0) * sin(angle);
          gc->StrokeLine(x1, y1, x2, y2);
        }
      } else if (m_iconType == ICON_SAVE) {
        double w = 10.0;
        double h = 10.0;
        double x = cx - w / 2.0;
        double y = cy - h / 2.0;

        wxGraphicsPath path = gc->CreatePath();
        path.MoveToPoint(x, y + h);
        path.AddLineToPoint(x + w, y + h);
        path.AddLineToPoint(x + w, y + 2.0);
        path.AddLineToPoint(x + w - 2.0, y);
        path.AddLineToPoint(x, y);
        path.CloseSubpath();
        gc->StrokePath(path);

        gc->DrawRectangle(cx - 2.5, cy + 1.0, 5.0, 3.5);
        gc->DrawRectangle(cx - 2.5, cy - 4.5, 4.0, 2.5);
      } else if (m_iconType == ICON_DOWNLOAD) {
        gc->StrokeLine(cx - 4.5, cy + 4.5, cx + 4.5, cy + 4.5);
        gc->StrokeLine(cx, cy - 5.0, cx, cy + 2.0);
        gc->StrokeLine(cx - 2.5, cy - 0.5, cx, cy + 2.0);
        gc->StrokeLine(cx + 2.5, cy - 0.5, cx, cy + 2.0);
      } else if (m_iconType == ICON_HEADPHONE) {
        double r = 4.5;
        wxGraphicsPath path = gc->CreatePath();
        path.AddArc(cx, cy + 1.0, r, M_PI, 0, true);
        gc->StrokePath(path);

        gc->SetBrush(wxBrush(iconCol));
        gc->DrawRectangle(cx - r - 1.0, cy, 1.8, 3.5);
        gc->DrawRectangle(cx + r - 0.8, cy, 1.8, 3.5);
      }
    }

    delete gc;
  }

  void OnLeftDown(wxMouseEvent &evt) {
    if (!IsEnabled())
      return;
    m_isPressed = true;
    if (m_isToggle) {
      m_isChecked = !m_isChecked;
    }
    CaptureMouse();
    Refresh();
    SendClickEvent();
  }

  void OnLeftUp(wxMouseEvent &evt) {
    if (HasCapture()) {
      ReleaseMouse();
    }
    m_isPressed = false;
    Refresh();
    if (!m_isToggle) {
      SendClickEvent();
    }
  }

  void OnMouseCaptureLost(wxMouseCaptureLostEvent &evt) {
    m_isPressed = false;
    Refresh();
  }

  void OnMotion(wxMouseEvent &evt) {
    wxPoint pos = evt.GetPosition();
    wxSize sz = GetSize();
    bool inside = (pos.x >= 0 && pos.x < sz.x && pos.y >= 0 && pos.y < sz.y);
    if (inside != m_isHovered) {
      m_isHovered = inside;
      Refresh();
    }
  }

  void OnEnter(wxMouseEvent &evt) {
    m_isHovered = true;
    Refresh();
  }

  void OnLeave(wxMouseEvent &evt) {
    m_isHovered = false;
    m_isPressed = false;
    Refresh();
  }

  void SendClickEvent() {
    wxCommandEvent event(wxEVT_TOGGLEBUTTON, GetId());
    event.SetEventObject(this);
    event.SetInt(m_isChecked ? 1 : 0);
    ProcessWindowEvent(event);
  }
};

class KnobControl : public wxWindow {
public:
  wxSize m_designSize;

  KnobControl(wxWindow *parent, wxWindowID id, const wxString &title,
              int minVal, int maxVal, int defaultVal,
              const wxSize &size = wxSize(40, 50))
      : wxWindow(parent, id, wxDefaultPosition, size, wxBORDER_NONE),
        m_title(title), m_val(defaultVal), m_min(minVal), m_max(maxVal),
        m_default(defaultVal), m_isDragging(false) {
    m_designSize = size;
    if (m_designSize.x <= 0 || m_designSize.y <= 0) {
      m_designSize = wxSize(40, 50);
    }
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &KnobControl::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &KnobControl::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &KnobControl::OnLeftUp, this);
    Bind(wxEVT_MOTION, &KnobControl::OnMotion, this);
    Bind(wxEVT_LEFT_DCLICK, &KnobControl::OnDoubleClick, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &KnobControl::OnMouseCaptureLost, this);
  }

  void SetValue(int val) {
    val = std::clamp(val, m_min, m_max);
    if (m_val != val) {
      m_val = val;
      Refresh();
    }
  }

  int GetValue() const { return m_val; }

  void Rescale(double scale) {
    SetMinSize(wxSize(m_designSize.x * scale, m_designSize.y * scale));
    SetMaxSize(wxSize(m_designSize.x * scale, m_designSize.y * scale));
  }

private:
  wxString m_title;
  int m_val;
  int m_min;
  int m_max;
  int m_default;
  bool m_isDragging;
  wxPoint m_dragStartPos;
  int m_dragStartVal;

  void OnPaint(wxPaintEvent &evt) {
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();
    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
    if (!gc)
      return;

    gc->Scale(g_uiScale, g_uiScale);
    wxSize sz = GetSize();
    double dx = sz.x / g_uiScale;
    double dy = sz.y / g_uiScale;

    gc->SetBrush(wxBrush(wxColour(30, 30, 30)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, dx, dy);

    double knobRadius = std::min(dx, dy - 12) / 2.0 - 2;
    double knobCenterX = dx / 2.0;
    double knobCenterY = (dy - 12) / 2.0 + 1;

    gc->SetBrush(wxBrush(wxColour(16, 16, 16)));
    gc->DrawEllipse(knobCenterX - knobRadius, knobCenterY - knobRadius,
                    knobRadius * 2, knobRadius * 2);

    gc->SetBrush(wxBrush(wxColour(46, 46, 46)));
    gc->SetPen(wxPen(wxColour(75, 75, 75), 1));
    gc->DrawEllipse(knobCenterX - knobRadius + 1, knobCenterY - knobRadius + 1,
                    (knobRadius - 1) * 2, (knobRadius - 1) * 2);

    double pct = (double)(m_val - m_min) / (m_max - m_min);
    double angleDeg = -135.0 + pct * 270.0;
    double angleRad = angleDeg * M_PI / 180.0;

    double px = knobCenterX + (knobRadius - 3) * sin(angleRad);
    double py = knobCenterY - (knobRadius - 3) * cos(angleRad);
    gc->SetPen(wxPen(
        IsEnabled() ? wxColour(255, 255, 255) : wxColour(100, 100, 100), 1.8));
    gc->StrokeLine(knobCenterX, knobCenterY, px, py);

    wxFont font = GetFont();
    font.SetPointSize(8);
    gc->SetFont(font,
                IsEnabled() ? wxColour(150, 150, 150) : wxColour(80, 80, 80));
    wxString valStr = std::format("{:+}", m_val);
    if (m_val == 0)
      valStr = "+0";

    double tw, th, td, tel;
    gc->GetTextExtent(valStr, &tw, &th, &td, &tel);
    gc->DrawText(valStr, (dx - tw) / 2.0, dy - th - 1);

    delete gc;
  }

  void OnLeftDown(wxMouseEvent &evt) {
    if (!IsEnabled())
      return;
    m_isDragging = true;
    m_dragStartPos = evt.GetPosition();
    m_dragStartVal = m_val;
    CaptureMouse();
  }

  void OnLeftUp(wxMouseEvent &evt) {
    if (HasCapture()) {
      ReleaseMouse();
    }
    m_isDragging = false;
  }

  void OnMouseCaptureLost(wxMouseCaptureLostEvent &evt) {
    m_isDragging = false;
    Refresh();
  }

  void OnMotion(wxMouseEvent &evt) {
    if (m_isDragging) {
      wxPoint currPos = evt.GetPosition();
      int dy = m_dragStartPos.y - currPos.y;
      int valRange = m_max - m_min;

      int delta = dy * (valRange / 120.0);
      if (delta == 0 && dy != 0) {
        delta = (dy > 0) ? 1 : -1;
      }

      int newVal = std::clamp(m_dragStartVal + delta, m_min, m_max);
      if (newVal != m_val) {
        m_val = newVal;
        Refresh();

        wxCommandEvent event(wxEVT_SLIDER, GetId());
        event.SetEventObject(this);
        event.SetInt(m_val);
        ProcessWindowEvent(event);
      }
    }
  }

  void OnDoubleClick(wxMouseEvent &evt) {
    if (!IsEnabled())
      return;
    if (m_val != m_default) {
      m_val = m_default;
      Refresh();

      wxCommandEvent event(wxEVT_SLIDER, GetId());
      event.SetEventObject(this);
      event.SetInt(m_val);
      ProcessWindowEvent(event);
    }
  }
};

class LevelMeter : public wxWindow {
public:
  wxSize m_designSize;

  LevelMeter(wxWindow *parent, wxWindowID id, bool stereo = false,
             const wxSize &size = wxSize(14, 100))
      : wxWindow(parent, id, wxDefaultPosition, size, wxBORDER_NONE),
        m_stereo(stereo), m_leftVal(-960), m_rightVal(-960), m_leftPeak(-960),
        m_rightPeak(-960) {
    m_designSize = size;
    if (m_designSize.x <= 0 || m_designSize.y <= 0) {
      m_designSize = wxSize(14, 100);
    }
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &LevelMeter::OnPaint, this);
  }

  void SetValue(int val) {
    m_leftVal = std::clamp(val, -960, 10);
    if (m_leftVal > m_leftPeak)
      m_leftPeak = m_leftVal;
    Refresh();
  }

  void SetValues(int left, int right) {
    m_leftVal = std::clamp(left, -960, 10);
    m_rightVal = std::clamp(right, -960, 10);
    if (m_leftVal > m_leftPeak)
      m_leftPeak = m_leftVal;
    if (m_rightVal > m_rightPeak)
      m_rightPeak = m_rightVal;
    Refresh();
  }

  void ResetPeak() {
    m_leftPeak = -960;
    m_rightPeak = -960;
    Refresh();
  }

  int GetLeftVal() const { return m_leftVal; }
  int GetRightVal() const { return m_rightVal; }

  void Rescale(double scale) {
    SetMinSize(wxSize(m_designSize.x * scale, m_designSize.y * scale));
    SetMaxSize(wxSize(-1, -1));
  }

private:
  bool m_stereo;
  int m_leftVal;
  int m_rightVal;
  int m_leftPeak;
  int m_rightPeak;

  void OnPaint(wxPaintEvent &evt) {
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();
    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
    if (!gc)
      return;

    gc->Scale(g_uiScale, g_uiScale);
    wxSize sz = GetSize();
    double dx = sz.x / g_uiScale;
    double dy = sz.y / g_uiScale;

    gc->SetBrush(wxBrush(wxColour(30, 30, 30)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, dx, dy);

    int numBars = m_stereo ? 2 : 1;
    double spacing = 1.0;
    double barWidth = (dx - 2.0 - (numBars - 1) * spacing) / numBars;

    int vals[2] = {m_leftVal, m_rightVal};
    int peaks[2] = {m_leftPeak, m_rightPeak};

    for (int i = 0; i < numBars; ++i) {
      double x = 1.0 + i * (barWidth + spacing);
      double ySlot = 2.0;
      double hSlot = dy - 4.0;

      gc->SetBrush(wxBrush(wxColour(14, 14, 14)));
      gc->DrawRectangle(x, ySlot, barWidth, hSlot);

      double pct = (double)(vals[i] - (-960)) / (10 - (-960));
      pct = std::clamp(pct, 0.0, 1.0);
      double hBar = hSlot * pct;
      double yBar = ySlot + hSlot - hBar;

      if (hBar > 0) {
        gc->SetBrush(wxBrush(wxColour(89, 155, 34)));
        gc->DrawRectangle(x, yBar, barWidth, hBar);

        double yellowDb = -120;
        if (vals[i] > yellowDb) {
          double pctY = (double)(yellowDb - (-960)) / (10 - (-960));
          double yYellow = ySlot + hSlot - hSlot * pctY;
          double hYellow = yYellow - yBar;
          gc->SetBrush(wxBrush(wxColour(241, 196, 15)));
          gc->DrawRectangle(x, yBar, barWidth, hYellow);
        }

        double redDb = -30;
        if (vals[i] > redDb) {
          double pctR = (double)(redDb - (-960)) / (10 - (-960));
          double yRed = ySlot + hSlot - hSlot * pctR;
          double hRed = yRed - yBar;
          gc->SetBrush(wxBrush(wxColour(192, 57, 43)));
          gc->DrawRectangle(x, yBar, barWidth, hRed);
        }
      }

      double pctP = (double)(peaks[i] - (-960)) / (10 - (-960));
      pctP = std::clamp(pctP, 0.0, 1.0);
      double yPeak = ySlot + hSlot - hSlot * pctP;
      gc->SetPen(wxPen(wxColour(255, 0, 0), 1));
      gc->StrokeLine(x, yPeak, x + barWidth, yPeak);
    }

    delete gc;
  }
};

class FaderStrip : public wxWindow {
public:
  wxSize m_designSize;

  FaderStrip(wxWindow *parent, wxWindowID id, int minVal, int maxVal,
             bool stereo = false, const wxSize &size = wxSize(45, 120))
      : wxWindow(parent, id, wxDefaultPosition, size,
                 wxBORDER_NONE | wxWANTS_CHARS),
        m_val(0), m_min(minVal), m_max(maxVal), m_stereo(stereo),
        m_leftVal(-960), m_rightVal(-960), m_leftPeak(-960), m_rightPeak(-960),
        m_isDragging(false), m_meterOnRight(false) {
    m_designSize = size;
    if (m_designSize.x <= 0 || m_designSize.y <= 0) {
      m_designSize = wxSize(45, 120);
    }
    SetCanFocus(true);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &FaderStrip::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &FaderStrip::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &FaderStrip::OnLeftUp, this);
    Bind(wxEVT_MOTION, &FaderStrip::OnMotion, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &FaderStrip::OnMouseCaptureLost, this);
    Bind(wxEVT_KEY_DOWN, &FaderStrip::OnKeyDown, this);
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &evt) {
      Refresh();
      evt.Skip();
    });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
      Refresh();
      evt.Skip();
    });
  }

  void OnKeyDown(wxKeyEvent &evt) {
    int key = evt.GetKeyCode();
    if (key == WXK_UP || key == WXK_RIGHT) {
      SetValue(m_val + 1);
      SendScrollEvent();
    } else if (key == WXK_DOWN || key == WXK_LEFT) {
      SetValue(m_val - 1);
      SendScrollEvent();
    } else {
      evt.Skip();
    }
  }

  void SetValue(int val) {
    val = std::clamp(val, m_min, m_max);
    if (m_val != val) {
      m_val = val;
      Refresh();
    }
  }

  int GetValue() const { return m_val; }

  void SetMeterLevels(int left, int right) {
    m_leftVal = std::clamp(left, -960, 10);
    m_rightVal = std::clamp(right, -960, 10);
    if (m_leftVal > m_leftPeak)
      m_leftPeak = m_leftVal;
    if (m_rightVal > m_rightPeak)
      m_rightPeak = m_rightVal;
    Refresh();
  }

  void ResetPeak() {
    m_leftPeak = -960;
    m_rightPeak = -960;
    Refresh();
  }
  void SetMeterOnRight(bool onRight) {
    m_meterOnRight = onRight;
    Refresh();
  }
  void Rescale(double scale) {
    SetMinSize(wxSize(m_designSize.x * scale, m_designSize.y * scale));
    SetMaxSize(wxSize(-1, -1));
  }

private:
  int m_val;
  int m_min;
  int m_max;
  bool m_stereo;
  int m_leftVal;
  int m_rightVal;
  int m_leftPeak;
  int m_rightPeak;
  bool m_isDragging;
  int m_dragStartVal;
  wxPoint m_dragStartPos;
  bool m_meterOnRight;

  double ValToY(int val, double hSlot, double ySlot) {
    double pct = (double)(val - m_min) / (m_max - m_min);
    pct = std::clamp(pct, 0.0, 1.0);
    return ySlot + hSlot - hSlot * pct;
  }

  int YToVal(double y, double hSlot, double ySlot) {
    double pct = (ySlot + hSlot - y) / hSlot;
    pct = std::clamp(pct, 0.0, 1.0);
    return m_min + std::round(pct * (m_max - m_min));
  }

  void OnPaint(wxPaintEvent &evt) {
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();
    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
    if (!gc)
      return;

    gc->Scale(g_uiScale, g_uiScale);
    wxSize sz = GetSize();
    double dx = sz.x / g_uiScale;
    double dy = sz.y / g_uiScale;

    gc->SetBrush(wxBrush(wxColour(30, 30, 30)));
    if (HasFocus()) {
      gc->SetPen(wxPen(wxColour(29, 115, 201), 1));
    } else {
      gc->SetPen(*wxTRANSPARENT_PEN);
    }
    gc->DrawRectangle(0, 0, dx, dy);

    double meterWidth = 5.0;
    double faderSlotWidth = 3.0;
    double ySlot = 4.0;
    double hSlot = dy - 8.0;

    double xCenter = dx / 2.0;
    double offset = m_stereo ? 26.0 : 20.0;
    double xL = xCenter - offset;
    double xR = xCenter + offset - meterWidth;

    // 1. Draw Level Meters
    if (m_stereo) {
      // Left Meter (stereo)
      gc->SetBrush(wxBrush(wxColour(12, 12, 12)));
      gc->DrawRectangle(xL, ySlot, meterWidth, hSlot);
      double pctL = (double)(m_leftVal - (-960)) / (10 - (-960));
      pctL = std::clamp(pctL, 0.0, 1.0);
      double hL = hSlot * pctL;
      double yL = ySlot + hSlot - hL;
      if (hL > 0) {
        gc->SetBrush(wxBrush(wxColour(89, 155, 34)));
        gc->DrawRectangle(xL, yL, meterWidth, hL);
        double yellowDb = -120;
        if (m_leftVal > yellowDb) {
          double pctY = (double)(yellowDb - (-960)) / (10 - (-960));
          double yYellow = ySlot + hSlot - hSlot * pctY;
          double hYellow = yYellow - yL;
          gc->SetBrush(wxBrush(wxColour(241, 196, 15)));
          gc->DrawRectangle(xL, yL, meterWidth, hYellow);
        }
        double redDb = -30;
        if (m_leftVal > redDb) {
          double pctR = (double)(redDb - (-960)) / (10 - (-960));
          double yRed = ySlot + hSlot - hSlot * pctR;
          double hRed = yRed - yL;
          gc->SetBrush(wxBrush(wxColour(192, 57, 43)));
          gc->DrawRectangle(xL, yL, meterWidth, hRed);
        }
      }

      // Right Meter (stereo)
      gc->SetBrush(wxBrush(wxColour(12, 12, 12)));
      gc->DrawRectangle(xR, ySlot, meterWidth, hSlot);
      double pctR = (double)(m_rightVal - (-960)) / (10 - (-960));
      pctR = std::clamp(pctR, 0.0, 1.0);
      double hR = hSlot * pctR;
      double yR = ySlot + hSlot - hR;
      if (hR > 0) {
        gc->SetBrush(wxBrush(wxColour(89, 155, 34)));
        gc->DrawRectangle(xR, yR, meterWidth, hR);
        double yellowDb = -120;
        if (m_rightVal > yellowDb) {
          double pctY = (double)(yellowDb - (-960)) / (10 - (-960));
          double yYellow = ySlot + hSlot - hSlot * pctY;
          double hYellow = yYellow - yR;
          gc->SetBrush(wxBrush(wxColour(241, 196, 15)));
          gc->DrawRectangle(xR, yR, meterWidth, hYellow);
        }
        double redDb = -30;
        if (m_rightVal > redDb) {
          double pctR = (double)(redDb - (-960)) / (10 - (-960));
          double yRed = ySlot + hSlot - hSlot * pctR;
          double hRed = yRed - yR;
          gc->SetBrush(wxBrush(wxColour(192, 57, 43)));
          gc->DrawRectangle(xR, yR, meterWidth, hRed);
        }
      }
    } else {
      // Mono Meter (uses m_leftVal at xL or xR)
      double xMeter = m_meterOnRight ? xR : xL;
      gc->SetBrush(wxBrush(wxColour(12, 12, 12)));
      gc->DrawRectangle(xMeter, ySlot, meterWidth, hSlot);
      double pct = (double)(m_leftVal - (-960)) / (10 - (-960));
      pct = std::clamp(pct, 0.0, 1.0);
      double h = hSlot * pct;
      double y = ySlot + hSlot - h;
      if (h > 0) {
        gc->SetBrush(wxBrush(wxColour(89, 155, 34)));
        gc->DrawRectangle(xMeter, y, meterWidth, h);
        double yellowDb = -120;
        if (m_leftVal > yellowDb) {
          double pctY = (double)(yellowDb - (-960)) / (10 - (-960));
          double yYellow = ySlot + hSlot - hSlot * pctY;
          double hYellow = yYellow - y;
          gc->SetBrush(wxBrush(wxColour(241, 196, 15)));
          gc->DrawRectangle(xMeter, y, meterWidth, hYellow);
        }
        double redDb = -30;
        if (m_leftVal > redDb) {
          double pctR = (double)(redDb - (-960)) / (10 - (-960));
          double yRed = ySlot + hSlot - hSlot * pctR;
          double hRed = yRed - y;
          gc->SetBrush(wxBrush(wxColour(192, 57, 43)));
          gc->DrawRectangle(xMeter, y, meterWidth, hRed);
        }
      }
    }

    // 2. Draw Fader Track
    gc->SetBrush(wxBrush(wxColour(10, 10, 10)));
    gc->DrawRectangle(xCenter - faderSlotWidth / 2.0, ySlot, faderSlotWidth,
                      hSlot);

    // 3. Draw Ticks & Labels
    gc->SetPen(wxPen(wxColour(70, 70, 70), 1));
    for (int db : {0, -6, -12, -24, -36, -60}) {
      if (db >= m_min && db <= m_max) {
        double yTick = ValToY(db, hSlot, ySlot);
        if (m_stereo) {
          gc->StrokeLine(xCenter - 8, yTick, xCenter - 4, yTick);
          gc->StrokeLine(xCenter + 4, yTick, xCenter + 8, yTick);
        } else {
          if (m_meterOnRight) {
            gc->StrokeLine(xCenter - 8, yTick, xCenter - 4, yTick);
          } else {
            gc->StrokeLine(xCenter + 4, yTick, xCenter + 8, yTick);
          }
        }

        wxFont font = GetFont();
        font.SetPointSize(6);
        gc->SetFont(font, wxColour(110, 110, 110));
        wxString lbl = std::format("{}", db);
        double tw, th, td, tel;
        gc->GetTextExtent(lbl, &tw, &th, &td, &tel);
        if (m_stereo) {
          gc->DrawText(lbl, xCenter - 8 - tw, yTick - th / 2.0);
        } else {
          if (m_meterOnRight) {
            gc->DrawText(lbl, xCenter - 8 - tw, yTick - th / 2.0);
          } else {
            gc->DrawText(lbl, xCenter + 10, yTick - th / 2.0);
          }
        }
      }
    }

    // 4. Draw Fader Cap (Handle)
    double handleHeight = 12.0;
    double handleWidth = m_stereo ? 18.0 : 16.0;
    double yHandle = ValToY(m_val, hSlot, ySlot) - handleHeight / 2.0;

    gc->SetBrush(wxBrush(wxColour(245, 245, 245)));
    gc->SetPen(wxPen(wxColour(90, 90, 90), 1));
    gc->DrawRoundedRectangle(xCenter - handleWidth / 2.0, yHandle, handleWidth,
                             handleHeight, 1.5);

    // Black line across the center of the handle
    gc->SetPen(wxPen(wxColour(20, 20, 20), 1.5));
    gc->StrokeLine(
        xCenter - handleWidth / 2.0 + 1, yHandle + handleHeight / 2.0,
        xCenter + handleWidth / 2.0 - 1, yHandle + handleHeight / 2.0);

    // 5. Draw Peaks
    if (m_stereo) {
      double pctPL = (double)(m_leftPeak - (-960)) / (10 - (-960));
      pctPL = std::clamp(pctPL, 0.0, 1.0);
      gc->SetPen(wxPen(wxColour(255, 0, 0), 1));
      gc->StrokeLine(xL, ySlot + hSlot - hSlot * pctPL, xL + meterWidth,
                     ySlot + hSlot - hSlot * pctPL);

      double pctPR = (double)(m_rightPeak - (-960)) / (10 - (-960));
      pctPR = std::clamp(pctPR, 0.0, 1.0);
      gc->StrokeLine(xR, ySlot + hSlot - hSlot * pctPR, xR + meterWidth,
                     ySlot + hSlot - hSlot * pctPR);
    } else {
      double xMeter = m_meterOnRight ? xR : xL;
      double pctPL = (double)(m_leftPeak - (-960)) / (10 - (-960));
      pctPL = std::clamp(pctPL, 0.0, 1.0);
      gc->SetPen(wxPen(wxColour(255, 0, 0), 1));
      gc->StrokeLine(xMeter, ySlot + hSlot - hSlot * pctPL, xMeter + meterWidth,
                     ySlot + hSlot - hSlot * pctPL);
    }

    delete gc;
  }

  void OnLeftDown(wxMouseEvent &evt) {
    SetFocus();
    m_isDragging = true;
    m_dragStartPos = evt.GetPosition();
    m_dragStartVal = m_val;
    CaptureMouse();

    double ySlot = 4.0;
    double hSlot = GetSize().y - 8.0;
    int newVal = YToVal(m_dragStartPos.y, hSlot, ySlot);
    if (newVal != m_val) {
      m_val = newVal;
      m_dragStartVal = newVal;
      Refresh();
      SendScrollEvent();
    }
  }

  void OnLeftUp(wxMouseEvent &evt) {
    if (HasCapture()) {
      ReleaseMouse();
    }
    m_isDragging = false;
  }

  void OnMouseCaptureLost(wxMouseCaptureLostEvent &evt) {
    m_isDragging = false;
    Refresh();
  }

  void OnMotion(wxMouseEvent &evt) {
    if (m_isDragging) {
      double ySlot = 4.0;
      double hSlot = GetSize().y - 8.0;
      wxPoint currPos = evt.GetPosition();

      int dy = currPos.y - m_dragStartPos.y;
      double startY = ValToY(m_dragStartVal, hSlot, ySlot);
      int newVal = YToVal(startY + dy, hSlot, ySlot);

      newVal = std::clamp(newVal, m_min, m_max);
      if (newVal != m_val) {
        m_val = newVal;
        Refresh();
        SendScrollEvent();
      }
    }
  }

  void SendScrollEvent() {
    wxCommandEvent event(wxEVT_SLIDER, GetId());
    event.SetEventObject(this);
    event.SetInt(m_val);
    ProcessWindowEvent(event);
  }
};

// ----------------------------------------------------------------------------
// Dialogs
// ----------------------------------------------------------------------------

class SettingsDialog : public wxDialog {
public:
  SettingsDialog(wxWindow *parent, uint16_t pid, ToppingHID *hid)
      : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE),
        m_hid(hid) {
    SetBackgroundColour(wxColour(30, 30, 30));
    SetForegroundColour(wxColour(220, 220, 220));

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *colsSizer = new wxBoxSizer(wxHORIZONTAL);

    // Left Column: System settings
    wxBoxSizer *leftCol = new wxBoxSizer(wxVERTICAL);
    wxStaticText *lblSysTitle =
        new wxStaticText(this, wxID_ANY, "System settings");
    lblSysTitle->SetFont(lblSysTitle->GetFont().Bold().Larger());
    leftCol->Add(lblSysTitle, 0, wxALL, 5);

    leftCol->Add(new wxStaticText(this, wxID_ANY, "Workspace storage:"), 0,
                 wxTOP | wxLEFT, 5);
    wxBoxSizer *dirSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *lblDir =
        new wxStaticText(this, wxID_ANY, "~/.config/topping...");
    lblDir->SetForegroundColour(wxColour(140, 140, 140));
    dirSizer->Add(lblDir, 1, wxALIGN_CENTER_VERTICAL);
    wxButton *btnChangeDir = new wxButton(this, wxID_ANY, "Change",
                                          wxDefaultPosition, wxSize(55, 20));
    dirSizer->Add(btnChangeDir, 0, wxLEFT, 5);
    leftCol->Add(dirSizer, 0, wxALL | wxEXPAND, 5);

    cbSaveWorkspace = new wxCheckBox(this, wxID_ANY, "Auto save workspace");
    bool saveWsVal = m_hid->settings.contains(0x9001)
                         ? (m_hid->settings[0x9001] != 0)
                         : true;
    cbSaveWorkspace->SetValue(saveWsVal);
    leftCol->Add(cbSaveWorkspace, 0, wxALL, 5);

    colsSizer->Add(leftCol, 1, wxALL | wxEXPAND, 8);

    wxStaticLine *divLine = new wxStaticLine(this, wxID_ANY, wxDefaultPosition,
                                             wxDefaultSize, wxLI_VERTICAL);
    colsSizer->Add(divLine, 0, wxEXPAND | wxTOP | wxBOTTOM, 8);

    // Right Column: Device settings
    wxBoxSizer *rightCol = new wxBoxSizer(wxVERTICAL);
    wxStaticText *lblDevTitle =
        new wxStaticText(this, wxID_ANY, "Device settings");
    lblDevTitle->SetFont(lblDevTitle->GetFont().Bold().Larger());
    rightCol->Add(lblDevTitle, 0, wxALL, 5);

    wxBoxSizer *brightSizer = new wxBoxSizer(wxHORIZONTAL);
    brightSizer->Add(new wxStaticText(this, wxID_ANY, "Brightness:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    choiceBrightness = new wxChoice(this, wxID_ANY);
    choiceBrightness->Append("Low");
    choiceBrightness->Append("Normal");
    choiceBrightness->Append("High");

    int brightVal =
        m_hid->settings.contains(0x1104) ? m_hid->settings[0x1104] : 1;
    choiceBrightness->SetSelection(std::clamp(brightVal, 0, 2));
    brightSizer->Add(choiceBrightness, 1, wxEXPAND);
    rightCol->Add(brightSizer, 0, wxALL | wxEXPAND, 5);

    cbAutoStandby = new wxCheckBox(this, wxID_ANY, "Automatic standby");
    int standbyVal = 1;
    if (pid == 0x8754) {
      standbyVal =
          m_hid->settings.contains(0x3901) ? m_hid->settings[0x3901] : 1;
    } else {
      standbyVal =
          m_hid->settings.contains(0x1101) ? m_hid->settings[0x1101] : 1;
    }
    cbAutoStandby->SetValue(standbyVal ? true : false);
    rightCol->Add(cbAutoStandby, 0, wxALL, 5);

    bool isOTG = (pid == 0x8755 || pid == 0x8756);
    if (isOTG) {
      cbOTGMode = new wxCheckBox(this, wxID_ANY, "Mobile applications");
      int otgVal =
          m_hid->settings.contains(0x1103) ? m_hid->settings[0x1103] : 0;
      cbOTGMode->SetValue(otgVal ? true : false);
      rightCol->Add(cbOTGMode, 0, wxALL, 5);
    } else if (pid == 0x8754) {
      cbOTGMode = new wxCheckBox(this, wxID_ANY, "Mobile applications");
      int otgVal =
          m_hid->settings.contains(0x3a01) ? m_hid->settings[0x3a01] : 0;
      cbOTGMode->SetValue(otgVal ? true : false);
      rightCol->Add(cbOTGMode, 0, wxALL, 5);
    } else {
      cbOTGMode = nullptr;
    }

    colsSizer->Add(rightCol, 1, wxALL | wxEXPAND, 8);
    mainSizer->Add(colsSizer, 0, wxEXPAND);

    wxBoxSizer *okCancelSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *btnReset = new wxButton(this, wxID_ANY, "Reset Defaults");
    okCancelSizer->Add(btnReset, 0, wxALL, 8);
    wxButton *btnOK = new wxButton(this, wxID_OK, "OK");
    okCancelSizer->Add(btnOK, 0, wxALL, 8);
    mainSizer->Add(okCancelSizer, 0, wxALIGN_RIGHT);

    SetSizer(mainSizer);
    Layout();
    Fit();

    choiceBrightness->Bind(wxEVT_CHOICE, &SettingsDialog::OnDeviceChange, this);
    cbAutoStandby->Bind(wxEVT_CHECKBOX, &SettingsDialog::OnDeviceChange, this);
    if (cbOTGMode) {
      cbOTGMode->Bind(wxEVT_CHECKBOX, &SettingsDialog::OnDeviceChange, this);
    }
    cbSaveWorkspace->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &evt) {
      m_hid->settings[0x9001] = cbSaveWorkspace->GetValue() ? 1 : 0;
    });
    btnReset->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
      m_hid->initializeSettingsWithDefaults(true);
      cbSaveWorkspace->SetValue(true);
      m_hid->settings[0x9001] = 1;

      choiceBrightness->SetSelection(1);
      m_hid->setDeviceSetting(0x11, 0x04, 1);
      m_hid->settings[0x1104] = 1;

      int standbyReg = (m_hid->pid == 0x8754) ? 0x3901 : 0x1101;
      m_hid->setDeviceSetting(standbyReg >> 8, standbyReg & 0xff, 1);
      m_hid->settings[standbyReg] = 1;
      cbAutoStandby->SetValue(true);

      if (cbOTGMode) {
        int otgReg = (m_hid->pid == 0x8754) ? 0x3a01 : 0x1103;
        m_hid->setDeviceSetting(otgReg >> 8, otgReg & 0xff, 0);
        m_hid->settings[otgReg] = 0;
        cbOTGMode->SetValue(false);
      }

      wxCommandEvent resetEvt(wxEVT_BUTTON, ID_RESET_DEFAULTS);
      GetParent()->GetEventHandler()->AddPendingEvent(resetEvt);

      wxMessageBox("Settings reset to defaults and applied to device!",
                   "Reset Success", wxOK | wxICON_INFORMATION, this);
    });
  }

  wxCheckBox *cbAutoStandby;
  wxCheckBox *cbOTGMode;
  wxChoice *choiceBrightness;

private:
  ToppingHID *m_hid;
  wxCheckBox *cbSaveWorkspace;

  void OnDeviceChange(wxCommandEvent &evt) {
    if (NULL == m_hid->getHandle())
      return;

    int sel = choiceBrightness->GetSelection();
    if (sel != wxNOT_FOUND) {
      m_hid->setDeviceSetting(0x11, 0x04, sel);
      m_hid->settings[0x1104] = sel;
    }

    bool val = cbAutoStandby->GetValue();
    int val32 = val ? 1 : 0;
    if (m_hid->pid == 0x8754) {
      m_hid->setDeviceSetting(0x39, 0x01, val32);
      m_hid->settings[0x3901] = val32;
    } else {
      m_hid->setDeviceSetting(0x11, 0x01, val32);
      m_hid->settings[0x1101] = val32;
    }

    if (cbOTGMode) {
      bool otgVal = cbOTGMode->GetValue();
      int otgVal32 = otgVal ? 1 : 0;
      if (m_hid->pid == 0x8754) {
        m_hid->setDeviceSetting(0x3a, 0x01, otgVal32);
        m_hid->settings[0x3a01] = otgVal32;
      } else {
        m_hid->setDeviceSetting(0x11, 0x03, otgVal32);
        m_hid->settings[0x1103] = otgVal32;
      }
    }
  }
};

class DownloadDialog : public wxDialog {
public:
  DownloadDialog(wxWindow *parent)
      : wxDialog(parent, wxID_ANY, "Download", wxDefaultPosition,
                 wxSize(380, 180), wxDEFAULT_DIALOG_STYLE) {
    SetBackgroundColour(wxColour(30, 30, 30));
    SetForegroundColour(wxColour(220, 220, 220));

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(new wxStaticText(this, wxID_ANY, ""), 0, wxTOP, 10);

    rbOption1 = new wxRadioButton(
        this, wxID_ANY, "Save current workspace and download to device",
        wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    rbOption2 = new wxRadioButton(this, wxID_ANY,
                                  "Save as workspace and download to device");
    rbOption3 = new wxRadioButton(this, wxID_ANY, "Download to device only");

    rbOption1->SetForegroundColour(*wxWHITE);
    rbOption2->SetForegroundColour(*wxWHITE);
    rbOption3->SetForegroundColour(*wxWHITE);

    mainSizer->Add(rbOption1, 0, wxALL, 6);
    mainSizer->Add(rbOption2, 0, wxALL, 6);
    mainSizer->Add(rbOption3, 0, wxALL, 6);

    wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *btnCancel =
        new wxButton(this, wxID_CANCEL, "Cancel", wxDefaultPosition,
                     wxDefaultSize, wxBORDER_NONE);
    btnCancel->SetBackgroundColour(wxColour(45, 45, 45));
    btnCancel->SetForegroundColour(*wxWHITE);

    wxButton *btnOK = new wxButton(this, wxID_OK, "OK", wxDefaultPosition,
                                   wxDefaultSize, wxBORDER_NONE);
    btnOK->SetBackgroundColour(wxColour(55, 55, 55));
    btnOK->SetForegroundColour(*wxWHITE);

    btnSizer->Add(btnCancel, 0, wxRIGHT, 10);
    btnSizer->Add(btnOK, 0);

    mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 10);

    SetSizer(mainSizer);
    Layout();
  }

  wxRadioButton *rbOption1;
  wxRadioButton *rbOption2;
  wxRadioButton *rbOption3;
};

// ----------------------------------------------------------------------------
// Panels Redesign
// ----------------------------------------------------------------------------

class PanelInputs : public wxPanel {
public:
  const static int32_t N_INPUTS = 4;
  int32_t activeInputs;
  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 30;

  wxStaticText *lbTitleI[N_INPUTS];
  CustomButton *lbPeaksI[N_INPUTS];
  wxStaticText *lbGainVal[N_INPUTS];

  FaderStrip *slGainI[N_INPUTS];
  FaderStrip *slGainICombined;
  CustomButton *btnLink;
  KnobControl *slPanI[N_INPUTS];

  CustomButton *cb48V[N_INPUTS];
  CustomButton *cbMon[N_INPUTS];
  CustomButton *cbInst[N_INPUTS];
  CustomButton *cbSolo[N_INPUTS];
  CustomButton *cbMute[N_INPUTS];
  CustomButton *cbPhase[N_INPUTS];

  // Split buttons for channel 3+4
  CustomButton *cbSoloL;
  CustomButton *cbSoloR;
  CustomButton *cbMuteL;
  CustomButton *cbMuteR;
  CustomButton *cbPhaseL;
  CustomButton *cbPhaseR;

  wxBoxSizer *combBtnSizer;
  wxBoxSizer *splitBtnSizer;

  int32_t PeaksI[N_INPUTS];
  int32_t inLevelVal[N_INPUTS];

  PanelInputs(wxWindow *parent, int32_t activeInputs = 4, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY), activeInputs(activeInputs) {
    SetBackgroundColour(wxColour(33, 33, 33));
    Bind(wxEVT_PAINT, [this](wxPaintEvent &evt) {
      wxPaintDC dc(this);
      dc.SetBackground(wxBrush(wxColour(33, 33, 33)));
      dc.Clear();
    });

    // Safety null initializations
    for (size_t i = 0; i < N_INPUTS; i++) {
      lbTitleI[i] = nullptr;
      lbPeaksI[i] = nullptr;
      lbGainVal[i] = nullptr;
      slGainI[i] = nullptr;
      slPanI[i] = nullptr;
      cb48V[i] = nullptr;
      cbMon[i] = nullptr;
      cbInst[i] = nullptr;
      cbSolo[i] = nullptr;
      cbMute[i] = nullptr;
      cbPhase[i] = nullptr;
    }
    slGainICombined = nullptr;
    btnLink = nullptr;
    cbSoloL = cbSoloR = cbMuteL = cbMuteR = cbPhaseL = cbPhaseR = nullptr;
    combBtnSizer = splitBtnSizer = nullptr;

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Top Panel Title
    wxBoxSizer *titleRow = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *lblTitle = new wxStaticText(this, wxID_ANY, "Input");
    lblTitle->SetFont(lblTitle->GetFont().Bold().Larger());
    lblTitle->SetForegroundColour(*wxWHITE);
    titleRow->Add(lblTitle, 0, wxLEFT | wxTOP, 6);

    wxStaticText *lblBar = new wxStaticText(this, wxID_ANY, "  |");
    lblBar->SetFont(lblBar->GetFont().Bold().Larger());
    lblBar->SetForegroundColour(wxColour(80, 80, 80));
    titleRow->Add(lblBar, 0, wxTOP, 6);

    mainSizer->Add(titleRow, 0, wxBOTTOM, 4);

    wxBoxSizer *colsSizer = new wxBoxSizer(wxHORIZONTAL);

    int numCols = 0;
    if (pid == 0x8754)
      numCols = 3;
    else if (pid == 0x8752 || pid == 0x8756)
      numCols = 2;
    else
      numCols = 1;

    for (int col = 0; col < numCols; ++col) {
      wxPanel *colPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, 0, "colPanel");
      colPanel->SetBackgroundColour(wxColour(25, 25, 25));
      colPanel->Bind(wxEVT_PAINT, [colPanel](wxPaintEvent &evt) {
        wxPaintDC dc(colPanel);
        dc.SetBackground(wxBrush(wxColour(25, 25, 25)));
        dc.Clear();
      });
      wxBoxSizer *colSizer = new wxBoxSizer(wxVERTICAL);

      wxString colName;
      if (col == 0)
        colName = "IN 1";
      else if (col == 1)
        colName = "IN 2";
      else
        colName = "Disconnected";

      lbTitleI[col] =
          new wxStaticText(colPanel, wxID_ANY, colName, wxDefaultPosition,
                           wxDefaultSize, wxALIGN_CENTER);
      lbTitleI[col]->SetFont(lbTitleI[col]->GetFont().Bold());
      lbTitleI[col]->SetForegroundColour(wxColour(180, 180, 180));
      colSizer->Add(lbTitleI[col], 0, wxALIGN_CENTER | wxTOP, 4);

      PeaksI[col] = LEVEL_MIN;
      inLevelVal[col] = LEVEL_MIN;
      if (col == 2)
        inLevelVal[3] = LEVEL_MIN;

      // Gain display label
      lbGainVal[col] =
          new wxStaticText(colPanel, wxID_ANY, "-60 dB", wxDefaultPosition,
                           wxDefaultSize, wxALIGN_CENTER);
      lbGainVal[col]->SetForegroundColour(wxColour(220, 220, 220));
      lbGainVal[col]->SetFont(lbGainVal[col]->GetFont().Smaller());
      colSizer->Add(lbGainVal[col], 0, wxALIGN_CENTER | wxTOP, 2);

      // Middle Part (Fader(s) & Buttons)
      wxBoxSizer *midSizer = new wxBoxSizer(wxHORIZONTAL);
      wxBoxSizer *faderSizer = new wxBoxSizer(wxHORIZONTAL);

      if (col < 2) {
        slGainI[col] = new FaderStrip(colPanel, ID_INPUT_GAIN + col, minGain,
                                      maxGain, false, wxSize(40, 90));
        faderSizer->Add(slGainI[col], 1, wxEXPAND);
      } else {
        // Stereo channel IN 3+4
        slGainI[2] = new FaderStrip(colPanel, ID_INPUT_GAIN + 2, minGain,
                                    maxGain, false, wxSize(32, 90));
        slGainI[2]->SetMeterOnRight(true);
        slGainI[3] = new FaderStrip(colPanel, ID_INPUT_GAIN + 3, minGain,
                                    maxGain, false, wxSize(32, 90));
        slGainICombined = new FaderStrip(colPanel, ID_INPUT_GAIN + 2, minGain,
                                         maxGain, true, wxSize(50, 90));

        faderSizer->Add(slGainI[2], 1, wxEXPAND);
        faderSizer->Add(slGainI[3], 1, wxEXPAND);
        faderSizer->Add(slGainICombined, 1, wxEXPAND);

        slGainI[2]->Hide();
        slGainI[3]->Hide();
      }

      midSizer->Add(faderSizer, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

      wxBoxSizer *btnCol = new wxBoxSizer(wxVERTICAL);

      if (col < 2) {
        cbMon[col] = new CustomButton(colPanel, ID_INPUT_MON + col, "MON", true,
                                      wxSize(44, 15), wxColour(89, 155, 34));
        cb48V[col] = new CustomButton(colPanel, ID_INPUT_48V + col, "48V", true,
                                      wxSize(44, 15), wxColour(192, 57, 43));
        cbInst[col] =
            new CustomButton(colPanel, ID_INPUT_INST + col, "INST", true,
                             wxSize(44, 15), wxColour(89, 155, 34));

        btnCol->Add(cbMon[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        btnCol->Add(cb48V[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        btnCol->Add(cbInst[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);

        cbSolo[col] =
            new CustomButton(colPanel, ID_INPUT_SOLO + col, "SOLO", true,
                             wxSize(44, 15), wxColour(241, 196, 15));
        cbMute[col] =
            new CustomButton(colPanel, ID_INPUT_MUTE + col, "MUTE", true,
                             wxSize(44, 15), wxColour(192, 57, 43));
        cbPhase[col] =
            new CustomButton(colPanel, ID_INPUT_PHASE + col, "PHASE", true,
                             wxSize(44, 15), wxColour(89, 155, 34));

        btnCol->Add(cbSolo[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        btnCol->Add(cbMute[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        btnCol->Add(cbPhase[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
      } else {
        btnLink = new CustomButton(colPanel, ID_INPUT_LINK, "Link", true,
                                   wxSize(36, 15), wxColour(29, 115, 201));
        btnLink->SetValue(true);
        btnCol->Add(btnLink, 0, wxBOTTOM, 4);

        // Combined Buttons Container
        combBtnSizer = new wxBoxSizer(wxVERTICAL);
        cbSolo[col] =
            new CustomButton(colPanel, ID_INPUT_SOLO + 2, "SOLO", true,
                             wxSize(44, 15), wxColour(241, 196, 15));
        cbMute[col] =
            new CustomButton(colPanel, ID_INPUT_MUTE + 2, "MUTE", true,
                             wxSize(44, 15), wxColour(192, 57, 43));
        cbPhase[col] =
            new CustomButton(colPanel, ID_INPUT_PHASE + 2, "PHASE", true,
                             wxSize(44, 15), wxColour(89, 155, 34));
        combBtnSizer->Add(cbSolo[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        combBtnSizer->Add(cbMute[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        combBtnSizer->Add(cbPhase[col], 0, wxALIGN_CENTER | wxBOTTOM, 1);
        btnCol->Add(combBtnSizer, 0, wxEXPAND);

        // Split Buttons Container
        splitBtnSizer = new wxBoxSizer(wxHORIZONTAL);

        wxBoxSizer *leftBtnCol = new wxBoxSizer(wxVERTICAL);
        cbSoloL = new CustomButton(colPanel, ID_INPUT_SOLO + 2, "SOLO", true,
                                   wxSize(18, 15), wxColour(241, 196, 15));
        cbMuteL = new CustomButton(colPanel, ID_INPUT_MUTE + 2, "MUTE", true,
                                   wxSize(18, 15), wxColour(192, 57, 43));
        cbPhaseL = new CustomButton(colPanel, ID_INPUT_PHASE + 2, "PHASE", true,
                                    wxSize(18, 15), wxColour(89, 155, 34));
        leftBtnCol->Add(cbSoloL, 0, wxEXPAND | wxBOTTOM, 1);
        leftBtnCol->Add(cbMuteL, 0, wxEXPAND | wxBOTTOM, 1);
        leftBtnCol->Add(cbPhaseL, 0, wxEXPAND | wxBOTTOM, 1);

        wxBoxSizer *rightBtnCol = new wxBoxSizer(wxVERTICAL);
        cbSoloR = new CustomButton(colPanel, ID_INPUT_SOLO + 3, "SOLO", true,
                                   wxSize(18, 15), wxColour(241, 196, 15));
        cbMuteR = new CustomButton(colPanel, ID_INPUT_MUTE + 3, "MUTE", true,
                                   wxSize(18, 15), wxColour(192, 57, 43));
        cbPhaseR = new CustomButton(colPanel, ID_INPUT_PHASE + 3, "PHASE", true,
                                    wxSize(18, 15), wxColour(89, 155, 34));
        rightBtnCol->Add(cbSoloR, 0, wxEXPAND | wxBOTTOM, 1);
        rightBtnCol->Add(cbMuteR, 0, wxEXPAND | wxBOTTOM, 1);
        rightBtnCol->Add(cbPhaseR, 0, wxEXPAND | wxBOTTOM, 1);

        splitBtnSizer->Add(leftBtnCol, 1, wxEXPAND | wxRIGHT, 1);
        splitBtnSizer->Add(rightBtnCol, 1, wxEXPAND | wxLEFT, 1);

        btnCol->Add(splitBtnSizer, 0, wxEXPAND);
        splitBtnSizer->Show(false);
      }

      midSizer->Add(btnCol, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
      colSizer->Add(midSizer, 1, wxEXPAND | wxTOP, 4);

      // Peak Reset Button
      lbPeaksI[col] =
          new CustomButton(colPanel, ID_INPUT_PEAK + col, "-120.0", false,
                           wxSize(45, 14), wxColour(50, 50, 50));
      lbPeaksI[col]->SetFont(lbPeaksI[col]->GetFont().Smaller());
      colSizer->Add(lbPeaksI[col], 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 2);

      // Bottom Panning Knob
      slPanI[col] = new KnobControl(colPanel, wxID_ANY, "Pan", -100, 100, 0,
                                    wxSize(35, 42));
      colSizer->Add(slPanI[col], 0, wxALIGN_CENTER | wxBOTTOM, 2);

      colPanel->SetSizer(colSizer);
      colPanel->SetMinSize(wxSize(120, -1));
      colsSizer->Add(colPanel, 0, wxEXPAND | wxALL, 2);
    }
    colsSizer->AddStretchSpacer(1);
    mainSizer->Add(colsSizer, 1, wxEXPAND);
    int panelWidth = (numCols == 1) ? 128 : ((numCols == 2) ? 248 : 368);
    SetMinSize(wxSize(panelWidth, -1));
    SetMaxSize(wxSize(panelWidth, -1));
    SetSizer(mainSizer);
  }
};

class PanelMixers : public wxPanel {
public:
  const static int32_t N_MIXERS = 4;
  const static int32_t N_MIX_SRCS = 12;

  wxPanel *tabBar;
  CustomButton *tabButtons[N_MIXERS];

  wxStaticText *lbTitle[N_MIX_SRCS / 2];
  wxStaticText *lbVolVal[N_MIX_SRCS / 2];
  KnobControl *slPan[N_MIX_SRCS];

  FaderStrip *slVol[N_MIX_SRCS / 2];
  FaderStrip *slVolL[N_MIX_SRCS / 2];
  FaderStrip *slVolR[N_MIX_SRCS / 2];
  CustomButton *btnLink[N_MIX_SRCS / 2];

  // Combined Buttons
  CustomButton *ckMute[N_MIX_SRCS / 2];
  CustomButton *ckSolo[N_MIX_SRCS / 2];
  CustomButton *ckPhaseCombined[N_MIX_SRCS / 2];

  // Split Buttons
  CustomButton *ckMuteL[N_MIX_SRCS / 2];
  CustomButton *ckMuteR[N_MIX_SRCS / 2];
  CustomButton *ckSoloL[N_MIX_SRCS / 2];
  CustomButton *ckSoloR[N_MIX_SRCS / 2];
  CustomButton *ckPhase[N_MIX_SRCS];

  wxBoxSizer *combBtnSizer[N_MIX_SRCS / 2];
  wxBoxSizer *splitBtnSizer[N_MIX_SRCS / 2];

  int32_t PeaksI[N_MIX_SRCS];
  int32_t mixLevelVal[N_MIX_SRCS];

  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 6;

  const wxString Titles[N_MIX_SRCS / 2] = {
      "IN1+2",        "Disconnected", "Playback 1/2",
      "Playback 3/4", "Playback 5/6", "Playback 7/8",
  };

  PanelMixers(wxWindow *parent, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY) {
    SetBackgroundColour(wxColour(33, 33, 33));
    Bind(wxEVT_PAINT, [this](wxPaintEvent &evt) {
      wxPaintDC dc(this);
      dc.SetBackground(wxBrush(wxColour(33, 33, 33)));
      dc.Clear();
    });

    for (size_t i = 0; i < N_MIX_SRCS; i++) {
      slPan[i] = nullptr;
      ckPhase[i] = nullptr;
    }
    for (size_t i = 0; i < N_MIX_SRCS / 2; i++) {
      lbTitle[i] = nullptr;
      lbVolVal[i] = nullptr;
      slVol[i] = nullptr;
      slVolL[i] = nullptr;
      slVolR[i] = nullptr;
      btnLink[i] = nullptr;
      ckMute[i] = nullptr;
      ckSolo[i] = nullptr;
      ckPhaseCombined[i] = nullptr;
      ckMuteL[i] = ckMuteR[i] = ckSoloL[i] = ckSoloR[i] = nullptr;
      combBtnSizer[i] = splitBtnSizer[i] = nullptr;
    }

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Title & Tab row
    wxBoxSizer *titleRow = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *lblTitle = new wxStaticText(this, wxID_ANY, "Mixer");
    lblTitle->SetFont(lblTitle->GetFont().Bold().Larger());
    lblTitle->SetForegroundColour(*wxWHITE);
    titleRow->Add(lblTitle, 0, wxLEFT | wxTOP, 6);

    wxStaticText *lblBar = new wxStaticText(this, wxID_ANY, "  |");
    lblBar->SetFont(lblBar->GetFont().Bold().Larger());
    lblBar->SetForegroundColour(wxColour(80, 80, 80));
    titleRow->Add(lblBar, 0, wxTOP, 6);

    tabBar = new wxPanel(this, wxID_ANY);
    tabBar->SetBackgroundColour(wxColour(33, 33, 33));
    tabBar->Bind(wxEVT_PAINT, [this](wxPaintEvent &evt) {
      wxPaintDC dc(tabBar);
      dc.SetBackground(wxBrush(wxColour(33, 33, 33)));
      dc.Clear();
    });
    wxBoxSizer *tabSizer = new wxBoxSizer(wxHORIZONTAL);

    wxColour activeColors[4] = {wxColour(89, 155, 34), wxColour(29, 115, 201),
                                wxColour(241, 196, 15), wxColour(211, 84, 0)};
    wxString mixNames[4] = {"Mix A", "Mix B", "Mix C", "Mix D"};

    for (int i = 0; i < N_MIXERS; ++i) {
      tabButtons[i] = new CustomButton(tabBar, ID_MIX_BUS_SEL + i, mixNames[i],
                                       true, wxSize(45, 18), activeColors[i]);
      tabSizer->Add(tabButtons[i], 0, wxLEFT | wxRIGHT, 4);
    }
    tabButtons[0]->SetValue(true);
    tabBar->SetSizer(tabSizer);
    titleRow->Add(tabBar, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 10);

    mainSizer->Add(titleRow, 0, wxBOTTOM, 4);

    wxBoxSizer *stripsSizer = new wxBoxSizer(wxHORIZONTAL);

    for (int i = 0; i < N_MIX_SRCS / 2; ++i) {
      wxPanel *stripPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, 0, "stripPanel");
      stripPanel->SetBackgroundColour(wxColour(25, 25, 25));
      stripPanel->Bind(wxEVT_PAINT, [stripPanel](wxPaintEvent &evt) {
        wxPaintDC dc(stripPanel);
        dc.SetBackground(wxBrush(wxColour(25, 25, 25)));
        dc.Clear();
      });
      wxBoxSizer *stripSizer = new wxBoxSizer(wxVERTICAL);

      lbTitle[i] =
          new wxStaticText(stripPanel, wxID_ANY, Titles[i], wxDefaultPosition,
                           wxDefaultSize, wxALIGN_CENTER);
      lbTitle[i]->SetFont(lbTitle[i]->GetFont().Bold());
      lbTitle[i]->SetForegroundColour(wxColour(180, 180, 180));
      stripSizer->Add(lbTitle[i], 0, wxALIGN_CENTER | wxTOP, 4);

      int l = i * 2;
      int r = l + 1;
      PeaksI[l] = LEVEL_MIN;
      PeaksI[r] = LEVEL_MIN;
      mixLevelVal[l] = LEVEL_MIN;
      mixLevelVal[r] = LEVEL_MIN;

      wxBoxSizer *panRow = new wxBoxSizer(wxHORIZONTAL);
      slPan[l] = new KnobControl(stripPanel, ID_MIX_PAN + l, "Pan L", -100, 100,
                                 -100, wxSize(20, 26));
      slPan[r] = new KnobControl(stripPanel, ID_MIX_PAN + r, "Pan R", -100, 100,
                                 100, wxSize(20, 26));
      panRow->Add(slPan[l], 1, wxEXPAND | wxRIGHT, 1);
      panRow->Add(slPan[r], 1, wxEXPAND | wxLEFT, 1);
      stripSizer->Add(panRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 2);

      // Link Button
      btnLink[i] = new CustomButton(stripPanel, ID_MIX_LINK + i, "Link", true,
                                    wxSize(36, 14), wxColour(29, 115, 201));
      btnLink[i]->SetValue(i == 0 ? false : true);
      stripSizer->Add(btnLink[i], 0, wxALIGN_CENTER | wxTOP, 2);

      // Volume display text
      lbVolVal[i] =
          new wxStaticText(stripPanel, wxID_ANY, "-inf  -inf",
                           wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
      lbVolVal[i]->SetForegroundColour(wxColour(220, 220, 220));
      lbVolVal[i]->SetFont(lbVolVal[i]->GetFont().Smaller());
      stripSizer->Add(lbVolVal[i], 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 2);

      // Horizontal fader sizer to hold linked fader OR split faders
      wxBoxSizer *faderSizer = new wxBoxSizer(wxHORIZONTAL);
      slVol[i] = new FaderStrip(stripPanel, ID_MIX_VOL_B + i, minGain, maxGain,
                                true, wxSize(50, 90));
      slVolL[i] = new FaderStrip(stripPanel, ID_MIX_VOL + l, minGain, maxGain,
                                 false, wxSize(32, 90));
      slVolL[i]->SetMeterOnRight(true);
      slVolR[i] = new FaderStrip(stripPanel, ID_MIX_VOL + r, minGain, maxGain,
                                 false, wxSize(32, 90));

      faderSizer->Add(slVolL[i], 1, wxEXPAND);
      faderSizer->Add(slVolR[i], 1, wxEXPAND);
      faderSizer->Add(slVol[i], 1, wxEXPAND);

      if (i == 0) {
        slVol[i]->Hide();
      } else {
        slVolL[i]->Hide();
        slVolR[i]->Hide();
      }

      stripSizer->Add(faderSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

      // Buttons Container Zone
      wxBoxSizer *btnZoneSizer = new wxBoxSizer(wxVERTICAL);

      // 1. Combined Buttons
      combBtnSizer[i] = new wxBoxSizer(wxVERTICAL);
      ckPhaseCombined[i] =
          new CustomButton(stripPanel, ID_MIX_PHASE + l, "PHASE", true,
                           wxSize(44, 15), wxColour(89, 155, 34));
      ckSolo[i] = new CustomButton(stripPanel, ID_MIX_SOLO + i, "SOLO", true,
                                   wxSize(44, 15), wxColour(241, 196, 15));
      ckMute[i] = new CustomButton(stripPanel, ID_MIX_MUTE + i, "MUTE", true,
                                   wxSize(44, 15), wxColour(192, 57, 43));

      combBtnSizer[i]->Add(ckPhaseCombined[i], 0, wxALIGN_CENTER | wxBOTTOM, 2);

      combBtnSizer[i]->Add(ckSolo[i], 0, wxALIGN_CENTER | wxBOTTOM, 1);
      combBtnSizer[i]->Add(ckMute[i], 0, wxALIGN_CENTER | wxBOTTOM, 1);
      if (i == 0) {
        combBtnSizer[i]->Show(false);
      }
      btnZoneSizer->Add(combBtnSizer[i], 0, wxEXPAND);

      // 2. Split Buttons
      splitBtnSizer[i] = new wxBoxSizer(wxHORIZONTAL);

      wxBoxSizer *lBtnCol = new wxBoxSizer(wxVERTICAL);
      ckPhase[l] = new CustomButton(stripPanel, ID_MIX_PHASE + l, "PHASE", true,
                                    wxSize(18, 15), wxColour(89, 155, 34));
      ckSoloL[i] = new CustomButton(stripPanel, ID_MIX_SOLO + i, "SOLO", true,
                                    wxSize(18, 15), wxColour(241, 196, 15));
      ckMuteL[i] = new CustomButton(stripPanel, ID_MIX_MUTE + i, "MUTE", true,
                                    wxSize(18, 15), wxColour(192, 57, 43));
      lBtnCol->Add(ckPhase[l], 0, wxEXPAND | wxBOTTOM, 1);
      lBtnCol->Add(ckSoloL[i], 0, wxEXPAND | wxBOTTOM, 1);
      lBtnCol->Add(ckMuteL[i], 0, wxEXPAND | wxBOTTOM, 1);

      wxBoxSizer *rBtnCol = new wxBoxSizer(wxVERTICAL);
      ckPhase[r] = new CustomButton(stripPanel, ID_MIX_PHASE + r, "PHASE", true,
                                    wxSize(18, 15), wxColour(89, 155, 34));
      ckSoloR[i] =
          new CustomButton(stripPanel, ID_MIX_SOLO + (i + 10), "SOLO", true,
                           wxSize(18, 15), wxColour(241, 196, 15));
      ckMuteR[i] =
          new CustomButton(stripPanel, ID_MIX_MUTE + (i + 10), "MUTE", true,
                           wxSize(18, 15), wxColour(192, 57, 43));
      rBtnCol->Add(ckPhase[r], 0, wxEXPAND | wxBOTTOM, 1);
      rBtnCol->Add(ckSoloR[i], 0, wxEXPAND | wxBOTTOM, 1);
      rBtnCol->Add(ckMuteR[i], 0, wxEXPAND | wxBOTTOM, 1);

      splitBtnSizer[i]->Add(lBtnCol, 1, wxEXPAND | wxRIGHT, 1);
      splitBtnSizer[i]->Add(rBtnCol, 1, wxEXPAND | wxLEFT, 1);
      btnZoneSizer->Add(splitBtnSizer[i], 0, wxEXPAND);
      splitBtnSizer[i]->Show(i == 0 ? true : false);

      stripSizer->Add(btnZoneSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                      2);

      stripPanel->SetSizer(stripSizer);
      stripsSizer->Add(stripPanel, 1, wxEXPAND | wxALL, 2);
    }
    mainSizer->Add(stripsSizer, 1, wxEXPAND);
    SetMinSize(wxSize(760, -1));
    SetMaxSize(wxSize(760, -1));
    SetSizer(mainSizer);
  }
};

class PanelLoopbacks : public wxPanel {
public:
  const static int32_t N_LOOPBACKS = 3;
  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 20;

  wxStaticText *lbTitle[N_LOOPBACKS];
  wxComboBox *cbSelect[N_LOOPBACKS];
  wxStaticText *lbLoopVolVal[N_LOOPBACKS];

  FaderStrip *slOutput[N_LOOPBACKS];
  FaderStrip *slOutputL[N_LOOPBACKS];
  FaderStrip *slOutputR[N_LOOPBACKS];
  CustomButton *btnLink[N_LOOPBACKS];

  // Combined Mute
  CustomButton *ckMute[N_LOOPBACKS];

  // Split Mutes
  CustomButton *ckMuteL[N_LOOPBACKS];
  CustomButton *ckMuteR[N_LOOPBACKS];

  wxBoxSizer *combBtnSizer[N_LOOPBACKS];
  wxBoxSizer *splitBtnSizer[N_LOOPBACKS];

  int32_t loopLevelVal[N_LOOPBACKS * 2];

  const wxString OutputSels[14] = {
      "IN1",   "IN2",         "IN3",         "IN4",         "IN1+2",
      "IN3+4", "Playback1+2", "Playback3+4", "Playback5+6", "Playback7+8",
      "MIX A", "MIX B",       "MIX C",       "MIX D",
  };

  PanelLoopbacks(wxWindow *parent, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY) {
    SetBackgroundColour(wxColour(33, 33, 33));
    Bind(wxEVT_PAINT, [this](wxPaintEvent &evt) {
      wxPaintDC dc(this);
      dc.SetBackground(wxBrush(wxColour(33, 33, 33)));
      dc.Clear();
    });

    for (size_t i = 0; i < N_LOOPBACKS; i++) {
      lbTitle[i] = nullptr;
      cbSelect[i] = nullptr;
      lbLoopVolVal[i] = nullptr;
      slOutput[i] = nullptr;
      slOutputL[i] = nullptr;
      slOutputR[i] = nullptr;
      btnLink[i] = nullptr;
      ckMute[i] = nullptr;
      ckMuteL[i] = ckMuteR[i] = nullptr;
      combBtnSizer[i] = splitBtnSizer[i] = nullptr;
    }

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Title Row
    wxBoxSizer *titleRow = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *lblTitle = new wxStaticText(this, wxID_ANY, "Loopback");
    lblTitle->SetFont(lblTitle->GetFont().Bold().Larger());
    lblTitle->SetForegroundColour(*wxWHITE);
    titleRow->Add(lblTitle, 0, wxLEFT | wxTOP, 6);

    wxStaticText *lblBar = new wxStaticText(this, wxID_ANY, "  |");
    lblBar->SetFont(lblBar->GetFont().Bold().Larger());
    lblBar->SetForegroundColour(wxColour(80, 80, 80));
    titleRow->Add(lblBar, 0, wxTOP, 6);

    mainSizer->Add(titleRow, 0, wxBOTTOM, 4);

    wxBoxSizer *stripsSizer = new wxBoxSizer(wxHORIZONTAL);

    int numStrips = 3;
    for (int i = 0; i < numStrips; ++i) {
      wxPanel *stripPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, 0, "stripPanel");
      stripPanel->SetBackgroundColour(wxColour(25, 25, 25));
      stripPanel->Bind(wxEVT_PAINT, [stripPanel](wxPaintEvent &evt) {
        wxPaintDC dc(stripPanel);
        dc.SetBackground(wxBrush(wxColour(25, 25, 25)));
        dc.Clear();
      });
      wxBoxSizer *stripSizer = new wxBoxSizer(wxVERTICAL);

      lbTitle[i] =
          new wxStaticText(stripPanel, wxID_ANY,
                           std::format("Loopback {}+{}", i * 2 + 1, i * 2 + 2),
                           wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
      lbTitle[i]->SetFont(lbTitle[i]->GetFont().Bold());
      lbTitle[i]->SetForegroundColour(wxColour(180, 180, 180));
      stripSizer->Add(lbTitle[i], 0, wxALIGN_CENTER | wxTOP, 4);

      cbSelect[i] = new wxComboBox(
          stripPanel, ID_LOOP_SEL + i, OutputSels[10 + i], wxDefaultPosition,
          wxSize(80, -1), 14, OutputSels, wxCB_READONLY);
      stripSizer->Add(cbSelect[i], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 2);

      // Link Button
      btnLink[i] = new CustomButton(stripPanel, ID_LOOP_LINK + i, "Link", true,
                                    wxSize(36, 14), wxColour(29, 115, 201));
      btnLink[i]->SetValue(true);
      stripSizer->Add(btnLink[i], 0, wxALIGN_CENTER | wxTOP, 2);

      // Volume display text
      lbLoopVolVal[i] =
          new wxStaticText(stripPanel, wxID_ANY, "-inf  -inf",
                           wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
      lbLoopVolVal[i]->SetForegroundColour(wxColour(220, 220, 220));
      lbLoopVolVal[i]->SetFont(lbLoopVolVal[i]->GetFont().Smaller());
      stripSizer->Add(lbLoopVolVal[i], 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 2);

      wxBoxSizer *faderSizer = new wxBoxSizer(wxHORIZONTAL);
      slOutput[i] = new FaderStrip(stripPanel, ID_LOOP_VOL_B + i, minGain,
                                   maxGain, true, wxSize(50, 95));
      slOutputL[i] = new FaderStrip(stripPanel, ID_LOOP_VOL_L + i, minGain,
                                    maxGain, false, wxSize(32, 95));
      slOutputL[i]->SetMeterOnRight(true);
      slOutputR[i] = new FaderStrip(stripPanel, ID_LOOP_VOL_R + i, minGain,
                                    maxGain, false, wxSize(32, 95));

      faderSizer->Add(slOutputL[i], 1, wxEXPAND);

      faderSizer->Add(slOutputR[i], 1, wxEXPAND);
      faderSizer->Add(slOutput[i], 1, wxEXPAND);

      slOutputL[i]->Hide();
      slOutputR[i]->Hide();

      stripSizer->Add(faderSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

      // Mute Buttons Container Zone
      wxBoxSizer *btnZoneSizer = new wxBoxSizer(wxVERTICAL);

      combBtnSizer[i] = new wxBoxSizer(wxVERTICAL);
      ckMute[i] = new CustomButton(stripPanel, ID_LOOP_MUTE + i, "MUTE", true,
                                   wxSize(36, 16), wxColour(192, 57, 43));
      combBtnSizer[i]->Add(ckMute[i], 0, wxALIGN_CENTER);
      btnZoneSizer->Add(combBtnSizer[i], 0, wxEXPAND);

      splitBtnSizer[i] = new wxBoxSizer(wxHORIZONTAL);
      ckMuteL[i] = new CustomButton(stripPanel, ID_LOOP_MUTE + i, "MUTE", true,
                                    wxSize(18, 16), wxColour(192, 57, 43));
      ckMuteR[i] =
          new CustomButton(stripPanel, ID_LOOP_MUTE + (i + 10), "MUTE", true,
                           wxSize(18, 16), wxColour(192, 57, 43));
      splitBtnSizer[i]->Add(ckMuteL[i], 1, wxEXPAND | wxRIGHT, 1);
      splitBtnSizer[i]->Add(ckMuteR[i], 1, wxEXPAND | wxLEFT, 1);
      btnZoneSizer->Add(splitBtnSizer[i], 0, wxEXPAND);
      splitBtnSizer[i]->Show(false);

      stripSizer->Add(btnZoneSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                      4);

      loopLevelVal[i * 2] = LEVEL_MIN;
      loopLevelVal[i * 2 + 1] = LEVEL_MIN;

      stripPanel->SetSizer(stripSizer);
      stripPanel->SetMinSize(wxSize(120, -1));
      stripsSizer->Add(stripPanel, 0, wxEXPAND | wxALL, 4);
    }
    stripsSizer->AddStretchSpacer(1);
    mainSizer->Add(stripsSizer, 1, wxEXPAND);
    int panelWidth = numStrips * 120 + 8;
    SetMinSize(wxSize(panelWidth, -1));
    SetMaxSize(wxSize(panelWidth, -1));
    SetSizer(mainSizer);
  }
};

class PanelOutputs : public wxPanel {
public:
  const static int32_t N_OUTPUTS = 3;
  const static int32_t LEVEL_MIN = -960;
  const static int32_t LEVEL_MAX = 10;
  const static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  const static int32_t minGain = -60;
  const static int32_t maxGain = 20;

  wxStaticText *lbTitle[N_OUTPUTS];
  wxComboBox *cbSelect[N_OUTPUTS];
  wxStaticText *lbOutVolVal[N_OUTPUTS];

  FaderStrip *slOutput[N_OUTPUTS];
  FaderStrip *slOutputL[N_OUTPUTS];
  FaderStrip *slOutputR[N_OUTPUTS];
  CustomButton *btnLink[N_OUTPUTS];

  // Combined Mutes
  CustomButton *ckMute[N_OUTPUTS];

  // Split Mutes
  CustomButton *ckMuteL[N_OUTPUTS];
  CustomButton *ckMuteR[N_OUTPUTS];

  wxBoxSizer *combBtnSizer[N_OUTPUTS];
  wxBoxSizer *splitBtnSizer[N_OUTPUTS];

  CustomButton *btnTRS[2];
  CustomButton *btnAUX[2];
  CustomButton *btnPhoneIcon[2];

  KnobControl *mixKnob;
  CustomButton *btnPhoneGain;
  wxChoice *choicePhoneOut;
  wxStaticText *lblMixVal;

  void updatePhoneMixLabel(int val) {
    if (!lblMixVal)
      return;
    int inputPct = 50 - val / 2;
    int playPct = 50 + val / 2;
    lblMixVal->SetLabel(std::format("Level: {}/{}", inputPct, playPct));
  }

  int32_t outLevelVal[N_OUTPUTS * 2];
  int m_phoneMix[2];
  bool m_phoneGain[2];

  const wxString OutputSels[14] = {
      "IN1",   "IN2",         "IN3",         "IN4",         "IN1+2",
      "IN3+4", "Playback1+2", "Playback3+4", "Playback5+6", "Playback7+8",
      "MIX A", "MIX B",       "MIX C",       "MIX D",
  };

  PanelOutputs(wxWindow *parent, uint16_t pid = 0x8754)
      : wxPanel(parent, wxID_ANY) {
    SetBackgroundColour(wxColour(33, 33, 33));
    Bind(wxEVT_PAINT, [this](wxPaintEvent &evt) {
      wxPaintDC dc(this);
      dc.SetBackground(wxBrush(wxColour(33, 33, 33)));
      dc.Clear();
    });

    // Safety null initializations
    for (size_t i = 0; i < N_OUTPUTS; i++) {
      lbTitle[i] = nullptr;
      cbSelect[i] = nullptr;
      lbOutVolVal[i] = nullptr;
      slOutput[i] = nullptr;
      slOutputL[i] = nullptr;
      slOutputR[i] = nullptr;
      btnLink[i] = nullptr;
      ckMute[i] = nullptr;
      ckMuteL[i] = ckMuteR[i] = nullptr;
      combBtnSizer[i] = splitBtnSizer[i] = nullptr;
    }
    for (size_t i = 0; i < 2; i++) {
      btnTRS[i] = nullptr;
      btnAUX[i] = nullptr;
      btnPhoneIcon[i] = nullptr;
    }

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Title Row
    wxBoxSizer *titleRow = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *lblTitle = new wxStaticText(this, wxID_ANY, "Output");
    lblTitle->SetFont(lblTitle->GetFont().Bold().Larger());
    lblTitle->SetForegroundColour(*wxWHITE);
    titleRow->Add(lblTitle, 0, wxLEFT | wxTOP, 6);

    wxStaticText *lblBar = new wxStaticText(this, wxID_ANY, "  |");
    lblBar->SetFont(lblBar->GetFont().Bold().Larger());
    lblBar->SetForegroundColour(wxColour(80, 80, 80));
    titleRow->Add(lblBar, 0, wxTOP, 6);

    mainSizer->Add(titleRow, 0, wxBOTTOM, 4);

    wxBoxSizer *stripsSizer = new wxBoxSizer(wxHORIZONTAL);

    bool isOTG = (pid == 0x8755 || pid == 0x8756);
    bool isE2x2 = (pid == 0x8752);

    int numStrips = 3;
    if (isE2x2)
      numStrips = 1;
    else if (isOTG)
      numStrips = 2;

    for (int i = 0; i < numStrips; ++i) {
      wxPanel *stripPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, 0, "stripPanel");
      stripPanel->SetBackgroundColour(wxColour(25, 25, 25));
      stripPanel->Bind(wxEVT_PAINT, [stripPanel](wxPaintEvent &evt) {
        wxPaintDC dc(stripPanel);
        dc.SetBackground(wxBrush(wxColour(25, 25, 25)));
        dc.Clear();
      });
      wxBoxSizer *stripSizer = new wxBoxSizer(wxVERTICAL);

      wxString name;
      if (i == 0)
        name = "Output 1+2";
      else if (i == 1 && isOTG)
        name = "Mobile OUT";
      else if (i == 1)
        name = "Disconnected";
      else
        name = "S/PDIF OUT";

      lbTitle[i] =
          new wxStaticText(stripPanel, wxID_ANY, name, wxDefaultPosition,
                           wxDefaultSize, wxALIGN_CENTER);
      lbTitle[i]->SetFont(lbTitle[i]->GetFont().Bold());
      lbTitle[i]->SetForegroundColour(wxColour(180, 180, 180));
      stripSizer->Add(lbTitle[i], 0, wxALIGN_CENTER | wxTOP, 4);

      cbSelect[i] = new wxComboBox(stripPanel, ID_OUTPUT_SEL + i, OutputSels[6],
                                   wxDefaultPosition, wxSize(80, -1), 14,
                                   OutputSels, wxCB_READONLY);
      stripSizer->Add(cbSelect[i], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 2);

      // Link Button
      btnLink[i] =
          new CustomButton(stripPanel, ID_OUTPUT_LINK + i, "Link", true,
                           wxSize(36, 14), wxColour(29, 115, 201));
      btnLink[i]->SetValue(true);
      stripSizer->Add(btnLink[i], 0, wxALIGN_CENTER | wxTOP, 2);

      // Volume display text
      lbOutVolVal[i] =
          new wxStaticText(stripPanel, wxID_ANY, "-inf  -inf",
                           wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
      lbOutVolVal[i]->SetForegroundColour(wxColour(220, 220, 220));
      lbOutVolVal[i]->SetFont(lbOutVolVal[i]->GetFont().Smaller());
      stripSizer->Add(lbOutVolVal[i], 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 2);

      wxBoxSizer *faderSizer = new wxBoxSizer(wxHORIZONTAL);
      slOutput[i] = new FaderStrip(stripPanel, ID_OUTPUT_VOL_B + i, minGain,
                                   maxGain, true, wxSize(50, 90));
      slOutputL[i] = new FaderStrip(stripPanel, ID_OUTPUT_VOL_L + i, minGain,
                                    maxGain, false, wxSize(32, 90));
      slOutputL[i]->SetMeterOnRight(true);
      slOutputR[i] = new FaderStrip(stripPanel, ID_OUTPUT_VOL_R + i, minGain,
                                    maxGain, false, wxSize(32, 90));

      faderSizer->Add(slOutputL[i], 1, wxEXPAND);

      faderSizer->Add(slOutputR[i], 1, wxEXPAND);
      faderSizer->Add(slOutput[i], 1, wxEXPAND);

      slOutputL[i]->Hide();
      slOutputR[i]->Hide();

      stripSizer->Add(faderSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

      if (i == 0) {
        wxBoxSizer *btnRow = new wxBoxSizer(wxHORIZONTAL);

        btnPhoneIcon[0] = new CustomButton(
            stripPanel, ID_OUTPUT_MON + 0, "", true, wxSize(14, 16),
            wxColour(29, 115, 201), ICON_HEADPHONE);
        btnPhoneIcon[0]->SetValue(true);

        btnTRS[0] = new CustomButton(stripPanel, ID_OUTPUT_LINE + 0, "TRS",
                                     true, wxSize(14, 16));
        btnTRS[0]->SetValue(true);

        btnRow->Add(btnPhoneIcon[0], 1, wxEXPAND | wxRIGHT, 1);
        btnRow->Add(btnTRS[0], 1, wxEXPAND | wxRIGHT, 1);

        if (pid != 0x8755 && pid != 0x8752) {
          btnAUX[0] = new CustomButton(stripPanel, ID_OUTPUT_LINE + 1, "AUX",
                                       true, wxSize(14, 16));
          btnAUX[0]->SetValue(true);
          btnRow->Add(btnAUX[0], 1, wxEXPAND);
        } else {
          btnAUX[0] = nullptr;
        }

        stripSizer->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
      } else {
        // Mute Buttons Container Zone
        wxBoxSizer *btnZoneSizer = new wxBoxSizer(wxVERTICAL);

        combBtnSizer[i] = new wxBoxSizer(wxVERTICAL);
        ckMute[i] =
            new CustomButton(stripPanel, ID_OUTPUT_MON + i, "MUTE", true,
                             wxSize(36, 16), wxColour(192, 57, 43));
        combBtnSizer[i]->Add(ckMute[i], 0, wxALIGN_CENTER);
        btnZoneSizer->Add(combBtnSizer[i], 0, wxEXPAND);

        splitBtnSizer[i] = new wxBoxSizer(wxHORIZONTAL);
        ckMuteL[i] =
            new CustomButton(stripPanel, ID_OUTPUT_MON + i, "MUTE", true,
                             wxSize(18, 16), wxColour(192, 57, 43));
        ckMuteR[i] =
            new CustomButton(stripPanel, ID_OUTPUT_MON + (i + 10), "MUTE", true,
                             wxSize(18, 16), wxColour(192, 57, 43));
        splitBtnSizer[i]->Add(ckMuteL[i], 1, wxEXPAND | wxRIGHT, 1);
        splitBtnSizer[i]->Add(ckMuteR[i], 1, wxEXPAND | wxLEFT, 1);
        btnZoneSizer->Add(splitBtnSizer[i], 0, wxEXPAND);
        splitBtnSizer[i]->Show(false);

        stripSizer->Add(btnZoneSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                        4);
      }

      outLevelVal[i * 2] = LEVEL_MIN;
      outLevelVal[i * 2 + 1] = LEVEL_MIN;

      stripPanel->SetSizer(stripSizer);
      stripPanel->SetMinSize(wxSize(120, -1));
      stripsSizer->Add(stripPanel, 0, wxEXPAND | wxALL, 4);
    }

    stripsSizer->AddStretchSpacer(1);

    wxPanel *monMixPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, 0, "monMixPanel");
    monMixPanel->SetBackgroundColour(wxColour(33, 33, 33));
    monMixPanel->Bind(wxEVT_PAINT, [monMixPanel](wxPaintEvent &evt) {
      wxPaintDC dc(monMixPanel);
      dc.SetBackground(wxBrush(wxColour(33, 33, 33)));
      dc.Clear();
    });
    wxBoxSizer *monMixSizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *lblMonTitle =
        new wxStaticText(monMixPanel, wxID_ANY, "Monitor Mix",
                         wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    lblMonTitle->SetFont(lblMonTitle->GetFont().Bold());
    lblMonTitle->SetForegroundColour(wxColour(180, 180, 180));
    monMixSizer->Add(lblMonTitle, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);

    mixKnob = new KnobControl(monMixPanel, ID_PHONE_MIX, "Mix", -100, 100, 0,
                              wxSize(60, 70));
    monMixSizer->Add(mixKnob, 0, wxALIGN_CENTER | wxBOTTOM, 2);

    wxBoxSizer *lblRow = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *lblIn = new wxStaticText(monMixPanel, wxID_ANY, "Input");
    lblIn->SetForegroundColour(wxColour(120, 120, 120));
    lblIn->SetFont(lblIn->GetFont().Smaller());
    wxStaticText *lblPlay = new wxStaticText(monMixPanel, wxID_ANY, "Playback");
    lblPlay->SetForegroundColour(wxColour(120, 120, 120));
    lblPlay->SetFont(lblPlay->GetFont().Smaller());
    lblRow->Add(lblIn, 0, wxRIGHT, 10);
    lblRow->Add(lblPlay, 0);
    monMixSizer->Add(lblRow, 0, wxALIGN_CENTER | wxBOTTOM, 6);

    choicePhoneOut = new wxChoice(monMixPanel, ID_PHONE_SEL, wxDefaultPosition,
                                  wxSize(80, -1));
    choicePhoneOut->Append("Phone 1");
    choicePhoneOut->Append("Phone 2");
    choicePhoneOut->SetSelection(0);
    monMixSizer->Add(choicePhoneOut, 0, wxALIGN_CENTER | wxBOTTOM, 6);

    lblMixVal =
        new wxStaticText(monMixPanel, wxID_ANY, "Level: 0dBu",
                         wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    lblMixVal->SetForegroundColour(wxColour(140, 140, 140));
    lblMixVal->SetFont(lblMixVal->GetFont().Smaller());
    monMixSizer->Add(lblMixVal, 0, wxALIGN_CENTER | wxBOTTOM, 4);

    btnPhoneGain = new CustomButton(monMixPanel, ID_PHONE_GAIN, "Gain", true,
                                    wxSize(50, 18), wxColour(29, 115, 201));
    monMixSizer->Add(btnPhoneGain, 0, wxALIGN_CENTER | wxBOTTOM, 6);

    m_phoneMix[0] = 0;
    m_phoneMix[1] = 0;
    m_phoneGain[0] = false;
    m_phoneGain[1] = false;

    mixKnob->Enable(pid == 0x8754);

    monMixPanel->SetSizer(monMixSizer);
    stripsSizer->Add(monMixPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 2);

    mainSizer->Add(stripsSizer, 1, wxEXPAND);
    int panelWidth = numStrips * 120 + 120;
    SetMinSize(wxSize(panelWidth, -1));
    SetMaxSize(wxSize(panelWidth, -1));
    SetSizer(mainSizer);

    choicePhoneOut->Bind(wxEVT_CHOICE, [this](wxCommandEvent &evt) {
      int idx = choicePhoneOut->GetSelection();
      if (idx == wxNOT_FOUND)
        return;
      mixKnob->SetValue(m_phoneMix[idx]);
      updatePhoneMixLabel(m_phoneMix[idx]);
      btnPhoneGain->SetValue(m_phoneGain[idx]);
    });
  }
};

// ----------------------------------------------------------------------------
// Main Frame redesign
// ----------------------------------------------------------------------------

class TPMixer : public wxFrame {
public:
  bool toStopHidReader = false;

  ToppingHID *hid;
  Gain *gain;

  PanelInputs *panelInputs;
  PanelMixers *panelMixers;
  PanelLoopbacks *panelLoopbacks;
  PanelOutputs *panelOutputs;

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
  void HidReader(hid_device *handle);
  void startHidReader() {
    handle = hid->getHandle();
    thReader = new std::thread(&TPMixer::HidReader, this, handle);
  };

  void setOutputVol(int32_t ch) {
    int32_t l = ch * 2;
    int32_t r = l + 1;
    bool linked = panelOutputs->btnLink[ch]
                      ? panelOutputs->btnLink[ch]->GetValue()
                      : true;
    bool mute = false, muteL = false, muteR = false;
    if (ch > 0) {
      mute = panelOutputs->ckMute[ch] ? panelOutputs->ckMute[ch]->GetValue()
                                      : false;
      muteL = panelOutputs->ckMuteL[ch] ? panelOutputs->ckMuteL[ch]->GetValue()
                                        : false;
      muteR = panelOutputs->ckMuteR[ch] ? panelOutputs->ckMuteR[ch]->GetValue()
                                        : false;
    }
    if (linked) {
      if (panelOutputs->slOutput[ch]) {
        hid->setOutputVol(
            l, gain->getMonoGain(panelOutputs->slOutput[ch]->GetValue(), mute,
                                 false, false, false));
        hid->setOutputVol(
            r, gain->getMonoGain(panelOutputs->slOutput[ch]->GetValue(), mute,
                                 false, false, false));
      }
    } else {
      if (panelOutputs->slOutputL[ch] && panelOutputs->slOutputR[ch]) {
        hid->setOutputVol(
            l, gain->getMonoGain(panelOutputs->slOutputL[ch]->GetValue(), muteL,
                                 false, false, false));
        hid->setOutputVol(
            r, gain->getMonoGain(panelOutputs->slOutputR[ch]->GetValue(), muteR,
                                 false, false, false));
      }
    }
  };

  void setLoopVol(int32_t ch) {
    int32_t l = ch * 2;
    int32_t r = l + 1;
    bool linked = panelLoopbacks->btnLink[ch]
                      ? panelLoopbacks->btnLink[ch]->GetValue()
                      : true;
    if (linked) {
      bool mute = panelLoopbacks->ckMute[ch]
                      ? panelLoopbacks->ckMute[ch]->GetValue()
                      : false;
      if (panelLoopbacks->slOutput[ch]) {
        hid->setLoopVol(
            l, gain->getMonoGain(panelLoopbacks->slOutput[ch]->GetValue(), mute,
                                 false, false, false));
        hid->setLoopVol(
            r, gain->getMonoGain(panelLoopbacks->slOutput[ch]->GetValue(), mute,
                                 false, false, false));
      }
    } else {
      bool muteL = panelLoopbacks->ckMuteL[ch]
                       ? panelLoopbacks->ckMuteL[ch]->GetValue()
                       : false;
      bool muteR = panelLoopbacks->ckMuteR[ch]
                       ? panelLoopbacks->ckMuteR[ch]->GetValue()
                       : false;
      if (panelLoopbacks->slOutputL[ch] && panelLoopbacks->slOutputR[ch]) {
        hid->setLoopVol(
            l, gain->getMonoGain(panelLoopbacks->slOutputL[ch]->GetValue(),
                                 muteL, false, false, false));
        hid->setLoopVol(
            r, gain->getMonoGain(panelLoopbacks->slOutputR[ch]->GetValue(),
                                 muteR, false, false, false));
      }
    }
  };

  std::tuple<bool, bool, int32_t> gain2dB(int32_t gain32) {
    bool muted = false;
    bool phase = false;
    int32_t gainDB = 0;
    if (0 == gain32) {
      gainDB = panelInputs->minGain;
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

  std::tuple<bool, bool, int32_t, int32_t>
  lrGain2PandB(int32_t gainL, int32_t gainR, int32_t minGain = -90) {
    bool muted = false;
    bool phase = false;
    int32_t totalGain = gainL + gainR;
    int32_t gainDB = 0;
    int32_t pan = 0;
    if (0 == totalGain) {
      gainDB = panelMixers->minGain;
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

  void sendInput(int32_t ch, bool hw = true, int16_t evtID = 0) {
    int32_t begin = ch, end = ch + 1;
    bool anySolo = false;

    int col = ch;
    if (ch >= 2)
      col = 2;

    bool linked = (col == 2 && panelInputs->btnLink)
                      ? panelInputs->btnLink->GetValue()
                      : true;
    int32_t gainDB = 0;
    if (col < 2) {
      if (panelInputs->slGainI[col])
        gainDB = panelInputs->slGainI[col]->GetValue();
    } else {
      if (linked) {
        if (panelInputs->slGainICombined)
          gainDB = panelInputs->slGainICombined->GetValue();
      } else {
        if (panelInputs->slGainI[ch])
          gainDB = panelInputs->slGainI[ch]->GetValue();
      }
    }

    for (int32_t i = 0; i < 3; i++) {
      if (panelInputs->cbSolo[i]) {
        anySolo |= panelInputs->cbSolo[i]->GetValue();
      }
    }
    // Also include split solo keys
    if (panelInputs->cbSoloL && panelInputs->cbSoloL->GetValue())
      anySolo = true;
    if (panelInputs->cbSoloR && panelInputs->cbSoloR->GetValue())
      anySolo = true;

    if (ch < 0) {
      begin = 0;
      end = panelInputs->N_INPUTS;
    } else if (ch == 2 && linked) {
      begin = 2;
      end = 4;
    }

    for (int32_t i = begin; i < end; i++) {
      int colMap = (i >= 2) ? 2 : i;

      bool mon = panelInputs->cbMon[colMap]
                     ? panelInputs->cbMon[colMap]->GetValue()
                     : false;
      hid->setInputMon(i, mon);

      if (panelInputs->cbInst[colMap]) {
        hid->setInputInst(i, panelInputs->cbInst[colMap]->GetValue());
      }

      bool mute = false;
      bool solo = false;
      bool phase = false;

      if (i < 2) {
        if (panelInputs->cbMute[colMap])
          mute = panelInputs->cbMute[colMap]->GetValue();
        if (panelInputs->cbSolo[colMap])
          solo = panelInputs->cbSolo[colMap]->GetValue();
        if (panelInputs->cbPhase[colMap])
          phase = panelInputs->cbPhase[colMap]->GetValue();
      } else {
        if (linked) {
          if (panelInputs->cbMute[2])
            mute = panelInputs->cbMute[2]->GetValue();
          if (panelInputs->cbSolo[2])
            solo = panelInputs->cbSolo[2]->GetValue();
          if (panelInputs->cbPhase[2])
            phase = panelInputs->cbPhase[2]->GetValue();
        } else {
          if (i == 2) {
            if (panelInputs->cbMuteL)
              mute = panelInputs->cbMuteL->GetValue();
            if (panelInputs->cbSoloL)
              solo = panelInputs->cbSoloL->GetValue();
            if (panelInputs->cbPhaseL)
              phase = panelInputs->cbPhaseL->GetValue();
          } else {
            if (panelInputs->cbMuteR)
              mute = panelInputs->cbMuteR->GetValue();
            if (panelInputs->cbSoloR)
              solo = panelInputs->cbSoloR->GetValue();
            if (panelInputs->cbPhaseR)
              phase = panelInputs->cbPhaseR->GetValue();
          }
        }
      }

      int32_t loopGainDB = gainDB;
      if (i >= 2 && !linked) {
        if (panelInputs->slGainI[i])
          loopGainDB = panelInputs->slGainI[i]->GetValue();
      }

      int32_t gainVal =
          gain->getMonoGain(loopGainDB, mute, solo, anySolo, phase);
      hid->setInputGainiI32(i, gainVal, hw);

      hid->settings[0x2101 + (i << 8)] = mon ? 1 : 0;
      if (panelInputs->cbInst[colMap]) {
        hid->settings[0x2103 + (i << 8)] =
            panelInputs->cbInst[colMap]->GetValue() ? 1 : 0;
      }
      hid->settings[0x2105 + (i << 8)] = gainVal;

      hid->settings[0x2106 + (i << 8)] = mute ? 1 : 0;
      hid->settings[0x2107 + (i << 8)] = solo ? 1 : 0;
      hid->settings[0x2108 + (i << 8)] = phase ? 1 : 0;
      hid->settings[0x2109 + (i << 8)] = loopGainDB;
    }
  }

  void saveSettings();
  void pushAllSettingsToDevice();
  bool loadSettings();
  void refreshInputsUi();
  void refreshMixerUi(int16_t bus = -1);
  void refreshLoopbackUi();
  void refreshOutputUi();

  void OnLoad(wxCommandEvent &event);
  void OnSave(wxCommandEvent &event);
  void OnDeviceSave(wxCommandEvent &event);
  void OnResetDefaults(wxCommandEvent &event);

  void OnSettings(wxCommandEvent &event) {
    SettingsDialog dlg(this, hid->pid, hid);
    ScaleUIElements(&dlg, g_uiScale);
    dlg.Fit();
    dlg.ShowModal();
  }

  void OnInputGain(wxCommandEvent &event);
  void OnMixBusSel(wxCommandEvent &event);
  void OnMixVolume(wxCommandEvent &event);
  void OnOutputVolume(wxCommandEvent &event);
  void OnOutputToggle(wxCommandEvent &event);
  void OnLoopVolume(wxCommandEvent &event);
  void OnLoopToggle(wxCommandEvent &event);
  void OnPhoneMix(wxCommandEvent &event);
  void OnPhoneGain(wxCommandEvent &event);
  void OnClose(wxCloseEvent &event);
  void OnInputPeak(wxCommandEvent &event);
  void OnLinkToggle(wxCommandEvent &event);
  void OnMixVolumeChanged(int stripIdx);
  void RescaleUI(double scale);
  void OnSize(wxSizeEvent &event);
};

// ----------------------------------------------------------------------------
// TPMixer constructor
// ----------------------------------------------------------------------------

TPMixer::TPMixer()
    : wxFrame(nullptr, wxID_ANY, "TOPPING Professional Control Center",
              wxDefaultPosition, wxDefaultSize) {
  SetBackgroundColour(wxColour(17, 17, 17));
  Bind(wxEVT_PAINT, [this](wxPaintEvent &evt) {
    wxPaintDC dc(this);
    dc.SetBackground(wxBrush(wxColour(17, 17, 17)));
    dc.Clear();
  });
  Bind(wxEVT_SIZE, &TPMixer::OnSize, this);

  std::string home = getenv("HOME");
  dir1 = home + pathSep + dirConfig;
  dir2 = dir1 + pathSep + dirApp;
  fileCfg = dir2 + pathSep + ConfigFile;

  hid = new ToppingHID();
  gain = new Gain();

  if (NULL != hid->getHandle()) {
    std::string deviceName = "Topping E4X4";
    if (hid->pid == 0x8755)
      deviceName = "Topping E1X2 OTG";
    else if (hid->pid == 0x8752)
      deviceName = "Topping E2X2";
    else if (hid->pid == 0x8756)
      deviceName = "Topping E2X2 OTG";
    SetTitle(deviceName + " Control Center");
  }

  wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

  // 1. TOP HEADER PANEL
  wxPanel *headerPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                     wxSize(-1, 32), 0, "headerPanel");
  headerPanel->SetBackgroundColour(wxColour(44, 44, 44));
  headerPanel->Bind(wxEVT_PAINT, [headerPanel](wxPaintEvent &evt) {
    wxPaintDC dc(headerPanel);
    dc.SetBackground(wxBrush(wxColour(44, 44, 44)));
    dc.Clear();
  });
  wxBoxSizer *headerSizer = new wxBoxSizer(wxHORIZONTAL);

  CustomButton *btnSettings =
      new CustomButton(headerPanel, ID_SETTINGS, "", false, wxSize(20, 20),
                       wxColour(29, 115, 201), ICON_GEAR);
  headerSizer->Add(btnSettings, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

  headerSizer->Add(0, 0, 1, wxEXPAND);

  wxStaticText *lblWork =
      new wxStaticText(headerPanel, wxID_ANY, "Workspace | ");
  lblWork->SetForegroundColour(wxColour(140, 140, 140));
  headerSizer->Add(lblWork, 0, wxALIGN_CENTER_VERTICAL);

  wxStaticText *valWork =
      new wxStaticText(headerPanel, wxID_ANY, "New20260716  ");
  valWork->SetForegroundColour(*wxWHITE);
  headerSizer->Add(valWork, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

  CustomButton *btnSave =
      new CustomButton(headerPanel, ID_SAVE, "", false, wxSize(20, 20),
                       wxColour(29, 115, 201), ICON_SAVE);
  CustomButton *btnLoad =
      new CustomButton(headerPanel, ID_LOAD, "", false, wxSize(20, 20),
                       wxColour(29, 115, 201), ICON_DOWNLOAD);
  headerSizer->Add(btnSave, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  headerSizer->Add(btnLoad, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

  headerPanel->SetSizer(headerSizer);
  mainSizer->Add(headerPanel, 0, wxEXPAND);

  // 2. MAIN GRID WITH BALANCED COLUMNS
  panelInputs = new PanelInputs(this, hid->numInputs, hid->pid);
  panelMixers = new PanelMixers(this, hid->pid);
  panelLoopbacks = new PanelLoopbacks(this, hid->pid);
  panelOutputs = new PanelOutputs(this, hid->pid);

  int leftWidth = 400;

  panelInputs->SetMinSize(wxSize(leftWidth, -1));
  panelInputs->SetMaxSize(wxSize(leftWidth, -1));
  panelLoopbacks->SetMinSize(wxSize(leftWidth, -1));
  panelLoopbacks->SetMaxSize(wxSize(leftWidth, -1));

  wxBoxSizer *col1 = new wxBoxSizer(wxVERTICAL);
  col1->Add(panelInputs, 1, wxEXPAND | wxBOTTOM, 2);
  col1->Add(panelLoopbacks, 1, wxEXPAND);

  wxBoxSizer *col2 = new wxBoxSizer(wxVERTICAL);
  col2->Add(panelMixers, 1, wxEXPAND | wxBOTTOM, 2);
  col2->Add(panelOutputs, 1, wxEXPAND);

  wxBoxSizer *contentSizer = new wxBoxSizer(wxHORIZONTAL);
  contentSizer->Add(col1, 0, wxEXPAND | wxRIGHT, 2);
  contentSizer->Add(col2, 0, wxEXPAND);
  contentSizer->AddStretchSpacer(1);

  mainSizer->Add(contentSizer, 1, wxEXPAND | wxALL, 2);
  SetSizer(mainSizer);

  // leftWidth is already computed above
  int rightWidth = 760;
  int totalWidth = leftWidth + rightWidth + 24;
  int totalHeight = 580;

  SetMinSize(wxSize(totalWidth, totalHeight));
  SetSize(wxSize(totalWidth, totalHeight));

  // Apply initial scale once the window is shown and layout is stable
  CallAfter(&TPMixer::RescaleUI, g_uiScale);

  btnSettings->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnSettings, this);
  btnSave->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnSave, this);
  btnLoad->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnLoad, this);
  Bind(wxEVT_BUTTON, &TPMixer::OnResetDefaults, this, ID_RESET_DEFAULTS);

  int numCols = (hid->pid == 0x8754)
                    ? 3
                    : ((hid->pid == 0x8752 || hid->pid == 0x8756) ? 2 : 1);
  for (int col = 0; col < numCols; ++col) {
    if (col < 2) {
      if (panelInputs->cbMon[col])
        panelInputs->cbMon[col]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                      this);
      if (panelInputs->cb48V[col])
        panelInputs->cb48V[col]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                      this);
      if (panelInputs->cbInst[col])
        panelInputs->cbInst[col]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnInputGain, this);
      if (panelInputs->slGainI[col])
        panelInputs->slGainI[col]->Bind(wxEVT_SLIDER, &TPMixer::OnInputGain,
                                        this);
      if (panelInputs->cbSolo[col])
        panelInputs->cbSolo[col]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnInputGain, this);
      if (panelInputs->cbMute[col])
        panelInputs->cbMute[col]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnInputGain, this);
      if (panelInputs->cbPhase[col])
        panelInputs->cbPhase[col]->Bind(wxEVT_TOGGLEBUTTON,
                                        &TPMixer::OnInputGain, this);
    } else {
      if (panelInputs->slGainI[2])
        panelInputs->slGainI[2]->Bind(wxEVT_SLIDER, &TPMixer::OnInputGain,
                                      this);
      if (panelInputs->slGainI[3])
        panelInputs->slGainI[3]->Bind(wxEVT_SLIDER, &TPMixer::OnInputGain,
                                      this);
      if (panelInputs->slGainICombined)
        panelInputs->slGainICombined->Bind(wxEVT_SLIDER, &TPMixer::OnInputGain,
                                           this);
      if (panelInputs->btnLink)
        panelInputs->btnLink->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnLinkToggle,
                                   this);

      // Bind Input split controls
      if (panelInputs->cbSoloL)
        panelInputs->cbSoloL->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                   this);
      if (panelInputs->cbSoloR)
        panelInputs->cbSoloR->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                   this);
      if (panelInputs->cbMuteL)
        panelInputs->cbMuteL->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                   this);
      if (panelInputs->cbMuteR)
        panelInputs->cbMuteR->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                   this);
      if (panelInputs->cbPhaseL)
        panelInputs->cbPhaseL->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                    this);
      if (panelInputs->cbPhaseR)
        panelInputs->cbPhaseR->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                    this);

      // Combined
      if (panelInputs->cbSolo[2])
        panelInputs->cbSolo[2]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                     this);
      if (panelInputs->cbMute[2])
        panelInputs->cbMute[2]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                     this);
      if (panelInputs->cbPhase[2])
        panelInputs->cbPhase[2]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnInputGain,
                                      this);
    }
    if (panelInputs->lbPeaksI[col])
      panelInputs->lbPeaksI[col]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnInputPeak, this);
  }

  for (int i = 0; i < panelMixers->N_MIXERS; ++i) {
    if (panelMixers->tabButtons[i]) {
      panelMixers->tabButtons[i]->Bind(
          wxEVT_TOGGLEBUTTON, [this, i](wxCommandEvent &evt) {
            for (int t = 0; t < panelMixers->N_MIXERS; ++t) {
              if (panelMixers->tabButtons[t])
                panelMixers->tabButtons[t]->SetValue(t == i);
            }
            wxCommandEvent fakeEvt(wxEVT_RADIOBOX, ID_MIX_BUS_SEL);
            fakeEvt.SetInt(i);
            this->OnMixBusSel(fakeEvt);
          });
    }
  }

  // Robust Lambda Event Bindings for Mixer channels
  for (int i = 0; i < panelMixers->N_MIX_SRCS / 2; ++i) {
    int l = i * 2;
    int r = l + 1;

    if (panelMixers->slPan[l]) {
      panelMixers->slPan[l]->Bind(wxEVT_SLIDER, [this, i](wxCommandEvent &evt) {
        this->OnMixVolumeChanged(i);
      });
    }
    if (panelMixers->slPan[r]) {
      panelMixers->slPan[r]->Bind(wxEVT_SLIDER, [this, i](wxCommandEvent &evt) {
        this->OnMixVolumeChanged(i);
      });
    }

    if (panelMixers->slVol[i]) {
      panelMixers->slVol[i]->Bind(wxEVT_SLIDER, [this, i](wxCommandEvent &evt) {
        this->OnMixVolumeChanged(i);
      });
    }
    if (panelMixers->slVolL[i]) {
      panelMixers->slVolL[i]->Bind(
          wxEVT_SLIDER,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->slVolR[i]) {
      panelMixers->slVolR[i]->Bind(
          wxEVT_SLIDER,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }

    if (panelMixers->btnLink[i]) {
      panelMixers->btnLink[i]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnLinkToggle,
                                    this);
    }

    if (panelMixers->ckSolo[i]) {
      panelMixers->ckSolo[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->ckMute[i]) {
      panelMixers->ckMute[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->ckPhaseCombined[i]) {
      panelMixers->ckPhaseCombined[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }

    if (panelMixers->ckSoloL[i]) {
      panelMixers->ckSoloL[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->ckSoloR[i]) {
      panelMixers->ckSoloR[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->ckMuteL[i]) {
      panelMixers->ckMuteL[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->ckMuteR[i]) {
      panelMixers->ckMuteR[i]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }

    if (panelMixers->ckPhase[l]) {
      panelMixers->ckPhase[l]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
    if (panelMixers->ckPhase[r]) {
      panelMixers->ckPhase[r]->Bind(
          wxEVT_TOGGLEBUTTON,
          [this, i](wxCommandEvent &evt) { this->OnMixVolumeChanged(i); });
    }
  }

  for (int i = 0; i < panelLoopbacks->N_LOOPBACKS; ++i) {
    if (panelLoopbacks->cbSelect[i])
      panelLoopbacks->cbSelect[i]->Bind(wxEVT_COMBOBOX, &TPMixer::OnLoopToggle,
                                        this);
    if (panelLoopbacks->slOutput[i])
      panelLoopbacks->slOutput[i]->Bind(wxEVT_SLIDER, &TPMixer::OnLoopVolume,
                                        this);
    if (panelLoopbacks->slOutputL[i])
      panelLoopbacks->slOutputL[i]->Bind(wxEVT_SLIDER, &TPMixer::OnLoopVolume,
                                         this);
    if (panelLoopbacks->slOutputR[i])
      panelLoopbacks->slOutputR[i]->Bind(wxEVT_SLIDER, &TPMixer::OnLoopVolume,
                                         this);
    if (panelLoopbacks->btnLink[i])
      panelLoopbacks->btnLink[i]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnLinkToggle, this);
    if (panelLoopbacks->ckMute[i])
      panelLoopbacks->ckMute[i]->Bind(wxEVT_TOGGLEBUTTON,
                                      &TPMixer::OnLoopToggle, this);
    if (panelLoopbacks->ckMuteL[i])
      panelLoopbacks->ckMuteL[i]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnLoopToggle, this);
    if (panelLoopbacks->ckMuteR[i])
      panelLoopbacks->ckMuteR[i]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnLoopToggle, this);
  }

  int numOuts = (hid->pid == 0x8752)
                    ? 1
                    : ((hid->pid == 0x8755 || hid->pid == 0x8756) ? 2 : 3);
  for (int i = 0; i < numOuts; ++i) {
    if (panelOutputs->cbSelect[i])
      panelOutputs->cbSelect[i]->Bind(wxEVT_COMBOBOX, &TPMixer::OnOutputToggle,
                                      this);
    if (panelOutputs->slOutput[i])
      panelOutputs->slOutput[i]->Bind(wxEVT_SLIDER, &TPMixer::OnOutputVolume,
                                      this);
    if (panelOutputs->slOutputL[i])
      panelOutputs->slOutputL[i]->Bind(wxEVT_SLIDER, &TPMixer::OnOutputVolume,
                                       this);
    if (panelOutputs->slOutputR[i])
      panelOutputs->slOutputR[i]->Bind(wxEVT_SLIDER, &TPMixer::OnOutputVolume,
                                       this);
    if (panelOutputs->btnLink[i])
      panelOutputs->btnLink[i]->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnLinkToggle,
                                     this);
    if (i == 0) {
      if (panelOutputs->btnPhoneIcon[0])
        panelOutputs->btnPhoneIcon[0]->Bind(wxEVT_TOGGLEBUTTON,
                                            &TPMixer::OnOutputToggle, this);
      if (panelOutputs->btnTRS[0])
        panelOutputs->btnTRS[0]->Bind(wxEVT_TOGGLEBUTTON,
                                      &TPMixer::OnOutputToggle, this);
      if (panelOutputs->btnAUX[0])
        panelOutputs->btnAUX[0]->Bind(wxEVT_TOGGLEBUTTON,
                                      &TPMixer::OnOutputToggle, this);
    } else {
      if (panelOutputs->ckMute[i])
        panelOutputs->ckMute[i]->Bind(wxEVT_TOGGLEBUTTON,
                                      &TPMixer::OnOutputToggle, this);
      if (panelOutputs->ckMuteL[i])
        panelOutputs->ckMuteL[i]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnOutputToggle, this);
      if (panelOutputs->ckMuteR[i])
        panelOutputs->ckMuteR[i]->Bind(wxEVT_TOGGLEBUTTON,
                                       &TPMixer::OnOutputToggle, this);
    }
  }

  if (panelOutputs->mixKnob)
    panelOutputs->mixKnob->Bind(wxEVT_SLIDER, &TPMixer::OnPhoneMix, this);
  if (panelOutputs->btnPhoneGain)
    panelOutputs->btnPhoneGain->Bind(wxEVT_TOGGLEBUTTON, &TPMixer::OnPhoneGain,
                                     this);

  Bind(wxEVT_CLOSE_WINDOW, &TPMixer::OnClose, this);

  if (hid->pid == 0x8755 || hid->pid == 0x8756) {
    if (panelMixers->lbTitle[0])
      panelMixers->lbTitle[0]->SetLabel("IN 1");
    if (panelMixers->lbTitle[1])
      panelMixers->lbTitle[1]->SetLabel("Mobile In");
  } else if (hid->pid == 0x8752) {
    if (panelMixers->lbTitle[1])
      panelMixers->lbTitle[1]->SetLabel("Unused");
  }

  loadSettings();
  refreshInputsUi();
  refreshMixerUi(-1);
  refreshLoopbackUi();
  refreshOutputUi();
  if (panelInputs)
    panelInputs->Layout();
  if (panelOutputs)
    panelOutputs->Layout();
  if (panelLoopbacks)
    panelLoopbacks->Layout();
  Layout();

  startHidReader();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (NULL != hid->getHandle()) {
    pushGuiStateToDevice();
  }
}

// ----------------------------------------------------------------------------
// scbUpdateLevels callback routing
// ----------------------------------------------------------------------------

void TPMixer::scbUpdateLevels(uint16_t ch16, int32_t val) {
  uint8_t ch = ch16 >> 8;
  uint8_t subCh = ch16 & 0xff;
  int32_t level01DB = val;
  int32_t cls = ch & 0xf0;

  bool isLevelMeter = false;
  if (cls == 0x40) {
    isLevelMeter = true;
  } else if (cls == 0x20 && subCh == 0x04) {
    isLevelMeter = true;
  } else if (cls == 0x30 && (ch - 0x31) < 6 && subCh == 0x01) {
    isLevelMeter = true;
  } else if (cls == 0x50 && subCh == 0x01) {
    isLevelMeter = true;
  }

  if (!isLevelMeter) {
    hid->settings[ch16] = val;
  }

  switch (cls) {
  case 0x10:
    break;
  case 0x20: {
    int32_t logicCh = ch - 0x21;
    int32_t vGauge = level01DB - panelInputs->LEVEL_MIN;
    int colMap = (logicCh >= 2) ? 2 : logicCh;
    bool inputLinked =
        (panelInputs->btnLink) ? panelInputs->btnLink->GetValue() : true;

    switch (subCh) {
    case 0x01:
      if (panelInputs->cbMon[colMap]) {
        panelInputs->cbMon[colMap]->SetValue(val);
      }
      hid->settings[0x2101 + (logicCh << 8)] = val;
      break;
    case 0x02:
      if (panelInputs->cb48V[colMap]) {
        panelInputs->cb48V[colMap]->SetValue(val);
      }
      hid->settings[0x2102 + (logicCh << 8)] = val;
      break;
    case 0x03:
      if (panelInputs->cbInst[colMap]) {
        panelInputs->cbInst[colMap]->SetValue(val);
      }
      hid->settings[0x2103 + (logicCh << 8)] = val;
      break;
    case 0x04:
      if (vGauge < 0)
        vGauge = 0;
      else if (vGauge > panelInputs->LEVEL_RANGE)
        vGauge = panelInputs->LEVEL_RANGE;

      panelInputs->inLevelVal[logicCh] = level01DB;
      if (logicCh < 2) {
        if (panelInputs->slGainI[logicCh]) {
          panelInputs->slGainI[logicCh]->SetMeterLevels(level01DB, -960);
        }
        if (level01DB > panelInputs->PeaksI[logicCh]) {
          panelInputs->PeaksI[logicCh] = level01DB;
          if (panelInputs->lbPeaksI[logicCh]) {
            panelInputs->lbPeaksI[logicCh]->SetLabel(
                std::format("{:+.1f}", level01DB * 0.1));
          }
        }
      } else {
        if (inputLinked) {
          if (panelInputs->slGainICombined) {
            panelInputs->slGainICombined->SetMeterLevels(
                panelInputs->inLevelVal[2], panelInputs->inLevelVal[3]);
          }
        } else {
          if (panelInputs->slGainI[2]) {
            panelInputs->slGainI[2]->SetMeterLevels(panelInputs->inLevelVal[2],
                                                    -960);
          }
          if (panelInputs->slGainI[3]) {
            panelInputs->slGainI[3]->SetMeterLevels(panelInputs->inLevelVal[3],
                                                    -960);
          }
        }
        if (level01DB > panelInputs->PeaksI[2]) {
          panelInputs->PeaksI[2] = level01DB;
          if (panelInputs->lbPeaksI[2]) {
            panelInputs->lbPeaksI[2]->SetLabel(
                std::format("{:+.1f}", level01DB * 0.1));
          }
        }
      }

      panelMixers->mixLevelVal[logicCh] = level01DB;
      if (logicCh < 2) {
        bool mixLinked0 = panelMixers->btnLink[0]
                              ? panelMixers->btnLink[0]->GetValue()
                              : true;
        if (mixLinked0) {
          if (panelMixers->slVol[0]) {
            panelMixers->slVol[0]->SetMeterLevels(panelMixers->mixLevelVal[0],
                                                  panelMixers->mixLevelVal[1]);
          }
        } else {
          if (panelMixers->slVolL[0])
            panelMixers->slVolL[0]->SetMeterLevels(panelMixers->mixLevelVal[0],
                                                   -960);
          if (panelMixers->slVolR[0])
            panelMixers->slVolR[0]->SetMeterLevels(panelMixers->mixLevelVal[1],
                                                   -960);
        }
      } else {
        bool mixLinked1 = panelMixers->btnLink[1]
                              ? panelMixers->btnLink[1]->GetValue()
                              : true;
        if (mixLinked1) {
          if (panelMixers->slVol[1]) {
            panelMixers->slVol[1]->SetMeterLevels(panelMixers->mixLevelVal[2],
                                                  panelMixers->mixLevelVal[3]);
          }
        } else {
          if (panelMixers->slVolL[1])
            panelMixers->slVolL[1]->SetMeterLevels(panelMixers->mixLevelVal[2],
                                                   -960);
          if (panelMixers->slVolR[1])
            panelMixers->slVolR[1]->SetMeterLevels(panelMixers->mixLevelVal[3],
                                                   -960);
        }
      }
      break;
    case 0x05:
      if (hid->pid == 0x8754) {
        auto [muted, phase, gainDB] = gain2dB(val);
        if (colMap < 2) {
          if (panelInputs->slGainI[colMap])
            panelInputs->slGainI[colMap]->SetValue(gainDB);
        } else {
          if (inputLinked) {
            if (panelInputs->slGainICombined)
              panelInputs->slGainICombined->SetValue(gainDB);
          } else {
            if (panelInputs->slGainI[logicCh])
              panelInputs->slGainI[logicCh]->SetValue(gainDB);
          }
        }
        if (panelInputs->cbMute[colMap])
          panelInputs->cbMute[colMap]->SetValue(muted);
        if (panelInputs->cbPhase[colMap])
          panelInputs->cbPhase[colMap]->SetValue(phase);
        if (panelInputs->lbGainVal[colMap]) {
          panelInputs->lbGainVal[colMap]->SetLabel(
              std::format("{:+} dB", gainDB));
        }
      }
      break;
    case 0x06:
      if (panelInputs->cbMute[colMap])
        panelInputs->cbMute[colMap]->SetValue(val ? true : false);
      hid->settings[0x2106 + (logicCh << 8)] = val;
      break;
    case 0x07:
      if (panelInputs->cbSolo[colMap])
        panelInputs->cbSolo[colMap]->SetValue(val ? true : false);
      hid->settings[0x2107 + (logicCh << 8)] = val;
      break;
    case 0x08:
      if (panelInputs->cbPhase[colMap])
        panelInputs->cbPhase[colMap]->SetValue(val ? true : false);
      hid->settings[0x2108 + (logicCh << 8)] = val;
      break;
    case 0x09:
      if (colMap < 2) {
        if (panelInputs->slGainI[colMap])
          panelInputs->slGainI[colMap]->SetValue(val);
      } else {
        if (inputLinked) {
          if (panelInputs->slGainICombined)
            panelInputs->slGainICombined->SetValue(val);
        } else {
          if (panelInputs->slGainI[logicCh])
            panelInputs->slGainI[logicCh]->SetValue(val);
        }
      }
      if (panelInputs->lbGainVal[colMap]) {
        panelInputs->lbGainVal[colMap]->SetLabel(std::format("{:+} dB", val));
      }
      hid->settings[0x2109 + (logicCh << 8)] = val;
      break;
    }
  } break;
  case 0x30: {
    int32_t vGauge = level01DB - panelOutputs->LEVEL_MIN;
    int32_t logicCh = ch - 0x31;
    if (logicCh < 0 || logicCh >= 10)
      return;

    int phoneIdx = -1;
    if (logicCh == 4 || logicCh == 8) {
      phoneIdx = 0;
    } else if (logicCh == 5 || logicCh == 9) {
      phoneIdx = 1;
    }

    if (phoneIdx != -1) {
      if (subCh == 1) {
        if (val >= 1 && val <= 14) {
          if (panelOutputs->cbSelect[phoneIdx]) {
            panelOutputs->cbSelect[phoneIdx]->SetSelection(val - 1);
          }
          hid->settings[ch16] = val;
        }
      } else if (subCh == 2) {
        panelOutputs->m_phoneGain[phoneIdx] = val ? true : false;
        if (panelOutputs->choicePhoneOut->GetSelection() == phoneIdx) {
          if (panelOutputs->btnPhoneGain)
            panelOutputs->btnPhoneGain->SetValue(val ? true : false);
        }
        hid->settings[ch16] = val;
      } else if (subCh == 3) {
        int phoneMix = (val - 50) * 2;
        panelOutputs->m_phoneMix[phoneIdx] = phoneMix;
        if (panelOutputs->choicePhoneOut->GetSelection() == phoneIdx) {
          if (panelOutputs->mixKnob)
            panelOutputs->mixKnob->SetValue(phoneMix);
          panelOutputs->updatePhoneMixLabel(phoneMix);
        }
        hid->settings[ch16] = val;
      }
    } else if (logicCh == 0x06) {
      if (subCh == 1 || subCh == 2) {
        if (panelOutputs->btnPhoneIcon[0])
          panelOutputs->btnPhoneIcon[0]->SetValue(val);
      } else {
        if (panelOutputs->btnTRS[0])
          panelOutputs->btnTRS[0]->SetValue(val);
      }
    } else {
      if (logicCh < 6 && subCh == 1) {
        if (vGauge < 0)
          vGauge = 0;
        else if (vGauge > panelOutputs->LEVEL_RANGE)
          vGauge = panelOutputs->LEVEL_RANGE;

        panelOutputs->outLevelVal[logicCh] = level01DB;
        int strip = logicCh / 2;
        bool outLinked = panelOutputs->btnLink[strip]
                             ? panelOutputs->btnLink[strip]->GetValue()
                             : true;
        if (outLinked) {
          if (panelOutputs->slOutput[strip]) {
            panelOutputs->slOutput[strip]->SetMeterLevels(
                panelOutputs->outLevelVal[strip * 2],
                panelOutputs->outLevelVal[strip * 2 + 1]);
          }
        } else {
          if (panelOutputs->slOutputL[strip])
            panelOutputs->slOutputL[strip]->SetMeterLevels(
                panelOutputs->outLevelVal[strip * 2], -960);
          if (panelOutputs->slOutputR[strip])
            panelOutputs->slOutputR[strip]->SetMeterLevels(
                panelOutputs->outLevelVal[strip * 2 + 1], -960);
        }
      }
    }
  } break;
  case 0x40: {
    int32_t vGauge = level01DB - panelMixers->LEVEL_MIN;
    int32_t logicBus = ch - 0x41;
    if (vGauge < 0)
      vGauge = 0;
    else if (vGauge > panelMixers->LEVEL_RANGE)
      vGauge = panelMixers->LEVEL_RANGE;

    int stripIdx = 2 + (logicBus / 2);
    if (stripIdx < 6) {
      panelMixers->mixLevelVal[logicBus + 4] = level01DB;
      bool mixLinked = panelMixers->btnLink[stripIdx]
                           ? panelMixers->btnLink[stripIdx]->GetValue()
                           : true;
      if (mixLinked) {
        if (panelMixers->slVol[stripIdx]) {
          panelMixers->slVol[stripIdx]->SetMeterLevels(
              panelMixers->mixLevelVal[stripIdx * 2],
              panelMixers->mixLevelVal[stripIdx * 2 + 1]);
        }
      } else {
        if (panelMixers->slVolL[stripIdx])
          panelMixers->slVolL[stripIdx]->SetMeterLevels(
              panelMixers->mixLevelVal[stripIdx * 2], -960);
        if (panelMixers->slVolR[stripIdx])
          panelMixers->slVolR[stripIdx]->SetMeterLevels(
              panelMixers->mixLevelVal[stripIdx * 2 + 1], -960);
      }
    }
  } break;
  case 0x50: {
    int32_t vGauge = level01DB - panelOutputs->LEVEL_MIN;
    int32_t logicCh = ch - 0x51;
    if (logicCh < 6) {
      if (subCh == 1) {
        if (vGauge < 0)
          vGauge = 0;
        else if (vGauge > panelOutputs->LEVEL_RANGE)
          vGauge = panelOutputs->LEVEL_RANGE;

        panelLoopbacks->loopLevelVal[logicCh] = level01DB;
        int strip = logicCh / 2;
        bool loopLinked = panelLoopbacks->btnLink[strip]
                              ? panelLoopbacks->btnLink[strip]->GetValue()
                              : true;
        if (loopLinked) {
          if (panelLoopbacks->slOutput[strip]) {
            panelLoopbacks->slOutput[strip]->SetMeterLevels(
                panelLoopbacks->loopLevelVal[strip * 2],
                panelLoopbacks->loopLevelVal[strip * 2 + 1]);
          }
        } else {
          if (panelLoopbacks->slOutputL[strip])
            panelLoopbacks->slOutputL[strip]->SetMeterLevels(
                panelLoopbacks->loopLevelVal[strip * 2], -960);
          if (panelLoopbacks->slOutputR[strip])
            panelLoopbacks->slOutputR[strip]->SetMeterLevels(
                panelLoopbacks->loopLevelVal[strip * 2 + 1], -960);
        }
      } else if (subCh == 3) {
        auto [muted, dummy, gainDB] = gain2dB(val);
        int strip = logicCh / 2;
        bool loopLinked = panelLoopbacks->btnLink[strip]
                              ? panelLoopbacks->btnLink[strip]->GetValue()
                              : true;
        if (loopLinked) {
          if (panelLoopbacks->slOutput[strip])
            panelLoopbacks->slOutput[strip]->SetValue(gainDB);
        } else {
          if (logicCh % 2 == 0) {
            if (panelLoopbacks->slOutputL[strip])
              panelLoopbacks->slOutputL[strip]->SetValue(gainDB);
          } else {
            if (panelLoopbacks->slOutputR[strip])
              panelLoopbacks->slOutputR[strip]->SetValue(gainDB);
          }
        }
      }
    } else {
      int loopIdx = logicCh - 6;
      if (loopIdx >= 0 && loopIdx < panelLoopbacks->N_LOOPBACKS) {
        if (panelLoopbacks->cbSelect[loopIdx]) {
          panelLoopbacks->cbSelect[loopIdx]->SetSelection(val - 1);
        }
        hid->settings[0x5701 + (loopIdx << 8)] = val;
      }
    }
  } break;
  case 0x60:
    hid->settings[ch16] = val;
    break;
  }
}

// ----------------------------------------------------------------------------
// Thread reader polling
// ----------------------------------------------------------------------------

void TPMixer::HidReader(hid_device *handle) {
  uint8_t bufread[256];
  uint16_t ch = 0;
  uint64_t val;
  int res = 0;
  if (NULL != handle) {
    while ((NULL != handle) && (!toStopHidReader)) {
      res = hid_read(handle, bufread, 16);
      if (res < 0) {
        break;
      }
      if (res > 0 && 0x22 == bufread[0]) {
        ch = read16BE(&bufread[5]);
        val = read32BE(&bufread[7]);
        CallAfter(&TPMixer::scbUpdateLevels, ch, val);
      }
    }
  } else {
    while (!toStopHidReader) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      int activeCols = (hid->pid == 0x8754) ? 3 : 2;
      for (int i = 0; i < activeCols; ++i) {
        int valLeft =
            panelInputs->LEVEL_MIN + rand() % (panelInputs->LEVEL_RANGE);
        CallAfter(&TPMixer::scbUpdateLevels, (uint16_t)(0x2104 + (i << 8)),
                  valLeft);
      }
      for (int i = 0; i < 6; ++i) {
        int valPlay =
            panelMixers->LEVEL_MIN + rand() % (panelMixers->LEVEL_RANGE);
        CallAfter(&TPMixer::scbUpdateLevels, (uint16_t)(0x4101 + (i << 8)),
                  valPlay);
      }
    }
  }
}

void ScaleUIElements(wxWindow *win, double scale) {
  if (!win)
    return;

  double fontScale = 1.0 + (scale - 1.0) * 0.4;
  if (fontScale < 0.8)
    fontScale = 0.8;
  if (fontScale > 1.25)
    fontScale = 1.25;

  if (auto btn = dynamic_cast<CustomButton *>(win)) {
    btn->Rescale(scale);
  } else if (auto knob = dynamic_cast<KnobControl *>(win)) {
    knob->Rescale(scale);
  } else if (auto meter = dynamic_cast<LevelMeter *>(win)) {
    meter->Rescale(scale);
  } else if (auto strip = dynamic_cast<FaderStrip *>(win)) {
    strip->Rescale(scale);
  } else if (auto txt = dynamic_cast<wxStaticText *>(win)) {
    wxString lbl = txt->GetLabel();
    double baseSize = 8.5;
    if (lbl.Contains("TOPPING")) {
      baseSize = 11.0;
    } else if (lbl.Contains("Professional")) {
      baseSize = 8.5;
    } else if (txt->GetFont().GetWeight() == wxFONTWEIGHT_BOLD) {
      baseSize = 9.5;
    } else if (txt->GetFont().GetFractionalPointSize() < 8.0) {
      baseSize = 7.0;
    }
    wxFont f = txt->GetFont();
    f.SetFractionalPointSize(baseSize * fontScale);
    txt->SetFont(f);
    txt->InvalidateBestSize();
    txt->SetMinSize(txt->GetBestSize());
  } else if (auto choice = dynamic_cast<wxChoice *>(win)) {
    wxFont f = choice->GetFont();
    f.SetFractionalPointSize(8.5 * fontScale);
    choice->SetFont(f);
  } else if (auto cb = dynamic_cast<wxComboBox *>(win)) {
    wxFont f = cb->GetFont();
    f.SetFractionalPointSize(8.5 * fontScale);
    cb->SetFont(f);
  }

  if (win->GetName() == "colPanel" || win->GetName() == "stripPanel" ||
      win->GetName() == "monMixPanel") {
    win->SetMinSize(wxSize(120 * scale, -1));
    if (win->GetName() == "monMixPanel") {
      win->SetMaxSize(wxSize(120 * scale, -1));
    }
  } else if (win->GetName() == "headerPanel") {
    win->SetMinSize(wxSize(-1, 32 * scale));
    win->SetMaxSize(wxSize(-1, 32 * scale));
  }

  wxWindowList &children = win->GetChildren();
  for (wxWindowList::iterator it = children.begin(); it != children.end();
       ++it) {
    wxWindow *child = *it;
    if (dynamic_cast<wxTopLevelWindow *>(child)) {
      continue;
    }
    ScaleUIElements(child, scale);
  }
}

void TPMixer::RescaleUI(double scale) {
  int leftWidth = 400;
  int rightWidth = 760;

  panelInputs->SetMinSize(wxSize(leftWidth * scale, -1));
  panelInputs->SetMaxSize(wxSize(leftWidth * scale, -1));

  panelLoopbacks->SetMinSize(wxSize(leftWidth * scale, -1));
  panelLoopbacks->SetMaxSize(wxSize(leftWidth * scale, -1));

  panelMixers->SetMinSize(wxSize(rightWidth * scale, -1));
  panelMixers->SetMaxSize(wxSize(rightWidth * scale, -1));

  int numOutStrips = (hid->pid == 0x8752)
                         ? 1
                         : ((hid->pid == 0x8755 || hid->pid == 0x8756) ? 2 : 3);
  int outputPanelWidth = numOutStrips * 120 + 120;
  panelOutputs->SetMinSize(wxSize(outputPanelWidth * scale, -1));
  panelOutputs->SetMaxSize(wxSize(outputPanelWidth * scale, -1));

  // NOTE: Do NOT call SetMinSize here with scaled values.
  // The window minimum size is fixed at design size so the user can always
  // shrink the window back after maximizing.

  ScaleUIElements(this, scale);

  Layout();
  Refresh();
}

void TPMixer::OnSize(wxSizeEvent &event) {
  wxSize size = GetClientSize();
  int leftWidth = 400;
  int rightWidth = 760;
  int designWidth = leftWidth + rightWidth + 24;
  int designHeight = 580;

  double scaleX = (double)size.x / designWidth;
  double scaleY = (double)size.y / designHeight;
  double newScale = std::min(scaleX, scaleY);
  if (newScale < 0.5)
    newScale = 0.5;
  if (newScale > 3.0)
    newScale = 3.0;

  if (std::abs(newScale - g_uiScale) > 0.02) {
    g_uiScale = newScale;
    RescaleUI(g_uiScale);
  }
  event.Skip();
}

void TPMixer::saveSettings() {
  createDir(dir1);
  createDir(dir2);
  FILE *f = fopen(fileCfg.c_str(), "w");
  if (NULL != f) {
    hid->settings[0x9999] = (int32_t)(g_uiScale * 100.0);
    for (const auto &[key, value] : hid->settings) {
      fprintf(f, "%04x %08x\n", key, value);
    }
    fclose(f);
  }
}

bool TPMixer::loadSettings() {
  hid->initializeSettingsWithDefaults();
  FILE *f = fopen(fileCfg.c_str(), "r");
  uint16_t key = 0;
  int32_t value = 0;
  ssize_t n;
  size_t len = 0;
  char *line = NULL;
  g_uiScale = 1.0;
  if (NULL != f) {
    while ((n = getline(&line, &len, f)) != -1) {
      sscanf(line, "%" SCNx16 " %" SCNx32, &key, &value);
      if (key == 0x9999) {
        g_uiScale = (double)value / 100.0;
      } else {
        hid->settings[key] = value;
        CallAfter(&TPMixer::scbUpdateLevels, key, value);
      }
    }
    if (NULL != line)
      free(line);
    fclose(f);
    CallAfter(&TPMixer::refreshMixerUi, -1);
    CallAfter(&TPMixer::refreshLoopbackUi);
    CallAfter(&TPMixer::refreshOutputUi);
    if (panelInputs->btnLink) {
      bool linked =
          hid->settings.contains(0x9000) ? (hid->settings[0x9000] != 0) : true;
      panelInputs->btnLink->SetValue(linked);
      if (panelInputs->slGainICombined)
        panelInputs->slGainICombined->Show(linked);
      if (panelInputs->slGainI[2])
        panelInputs->slGainI[2]->Show(!linked);
      if (panelInputs->slGainI[3])
        panelInputs->slGainI[3]->Show(!linked);
      if (panelInputs->combBtnSizer)
        panelInputs->combBtnSizer->Show(linked);
      if (panelInputs->splitBtnSizer)
        panelInputs->splitBtnSizer->Show(!linked);
      panelInputs->Layout();
    }
    // Always re-apply scale after loading so panel sizes initialize correctly
    CallAfter(&TPMixer::RescaleUI, g_uiScale);
    return true;
  }
  return false;
}

void TPMixer::pushGuiStateToDevice() {
  if (NULL == hid->getHandle())
    return;

  // 1. Push Inputs: MON, 48V, INST, and Gain/Mute/Solo/Phase
  for (int i = 0; i < hid->numInputs; ++i) {
    if (i < 2) {
      if (panelInputs->cbMon[i]) {
        hid->setInputMon(i, panelInputs->cbMon[i]->GetValue());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (panelInputs->cb48V[i]) {
        hid->setInput48V(i, panelInputs->cb48V[i]->GetValue());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (panelInputs->cbInst[i]) {
        hid->setInputInst(i, panelInputs->cbInst[i]->GetValue());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    sendInput(i, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 2. Push Mixer volumes (Mix A, B, C, D) for all 12 channels
  for (int bus = 0; bus < 4; ++bus) {
    for (int src = 0; src < 12; ++src) {
      uint16_t keyL = ((0x61 + bus * 2) << 8) | (src + 1);
      uint16_t keyR = ((0x62 + bus * 2) << 8) | (src + 1);
      if (hid->settings.contains(keyL) && hid->settings.contains(keyR)) {
        int32_t gainL = hid->settings[keyL];
        int32_t gainR = hid->settings[keyR];
        hid->setMixVol(bus, src, gainL, gainR);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  }

  // 3. Push Output Routing Selection and Volume
  int numOuts = (hid->pid == 0x8752)
                    ? 1
                    : ((hid->pid == 0x8755 || hid->pid == 0x8756) ? 2 : 3);
  for (int i = 0; i < numOuts; ++i) {
    if (panelOutputs->cbSelect[i]) {
      int val = panelOutputs->cbSelect[i]->GetSelection();
      hid->setOutputSel(i, val + 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    setOutputVol(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 4. Push Phone/TRS/AUX enables (0x3701..0x3704)
  if (hid->settings.contains(0x3701)) {
    hid->setOutputMon(0, hid->settings[0x3701] != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (hid->settings.contains(0x3702)) {
    hid->setOutputMon(1, hid->settings[0x3702] != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (hid->settings.contains(0x3703)) {
    hid->setOutputLine(0, hid->settings[0x3703] != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (hid->settings.contains(0x3704) && hid->pid != 0x8755 &&
      hid->pid != 0x8752) {
    hid->setOutputLine(1, hid->settings[0x3704] != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 5. Push Phone Gain and Phone Mix
  uint8_t phoneBase = hid->phoneRegOffset();
  for (int i = 0; i < 2; ++i) {
    uint16_t keyGain = ((phoneBase + i) << 8) | 2;
    uint16_t keyMix = ((phoneBase + i) << 8) | 3;
    if (hid->settings.contains(keyGain)) {
      hid->setPhoneGainBoost(i, hid->settings[keyGain]);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (hid->settings.contains(keyMix)) {
      hid->setPhoneMix(i, hid->settings[keyMix]);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // 6. Push Loopback selection and levels
  for (int i = 0; i < panelLoopbacks->N_LOOPBACKS; ++i) {
    if (panelLoopbacks->cbSelect[i]) {
      int val = panelLoopbacks->cbSelect[i]->GetSelection();
      hid->setLoopSel(i, val + 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    setLoopVol(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 7. Push Device Standby, OTG Mode, and LED Brightness Settings
  if (hid->pid == 0x8754) {
    if (hid->settings.contains(0x3901)) {
      hid->setDeviceSetting(0x39, 0x01, hid->settings[0x3901]);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (hid->settings.contains(0x3a01)) {
      hid->setDeviceSetting(0x3a, 0x01, hid->settings[0x3a01]);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  } else {
    if (hid->settings.contains(0x1101)) {
      hid->setDeviceSetting(0x11, 0x01, hid->settings[0x1101]);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (hid->settings.contains(0x1103)) {
      hid->setDeviceSetting(0x11, 0x03, hid->settings[0x1103]);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  if (hid->settings.contains(0x1104)) {
    hid->setDeviceSetting(0x11, 0x04, hid->settings[0x1104]);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void TPMixer::refreshInputsUi() {
  if (NULL == hid->getHandle())
    return;

  int numCols = (hid->pid == 0x8754)
                    ? 3
                    : ((hid->pid == 0x8752 || hid->pid == 0x8756) ? 2 : 1);

  bool inputLinked =
      (panelInputs->btnLink) ? panelInputs->btnLink->GetValue() : true;

  for (int col = 0; col < numCols; ++col) {
    int logicCh = col;
    int colMap = (logicCh >= 2) ? 2 : logicCh;

    uint16_t keyMon = 0x2101 + (logicCh << 8);
    if (hid->settings.contains(keyMon)) {
      int32_t val = hid->settings[keyMon];
      if (panelInputs->cbMon[colMap]) {
        panelInputs->cbMon[colMap]->SetValue(val ? true : false);
      }
    }

    uint16_t key48V = 0x2102 + (logicCh << 8);
    if (hid->settings.contains(key48V)) {
      int32_t val = hid->settings[key48V];
      if (panelInputs->cb48V[colMap]) {
        panelInputs->cb48V[colMap]->SetValue(val ? true : false);
      }
    }

    uint16_t keyInst = 0x2103 + (logicCh << 8);
    if (hid->settings.contains(keyInst)) {
      int32_t val = hid->settings[keyInst];
      if (panelInputs->cbInst[colMap]) {
        panelInputs->cbInst[colMap]->SetValue(val ? true : false);
      }
    }

    uint16_t keyMute = 0x2106 + (logicCh << 8);
    if (hid->settings.contains(keyMute)) {
      int32_t val = hid->settings[keyMute];
      if (panelInputs->cbMute[colMap]) {
        panelInputs->cbMute[colMap]->SetValue(val ? true : false);
      }
    }

    uint16_t keySolo = 0x2107 + (logicCh << 8);
    if (hid->settings.contains(keySolo)) {
      int32_t val = hid->settings[keySolo];
      if (panelInputs->cbSolo[colMap]) {
        panelInputs->cbSolo[colMap]->SetValue(val ? true : false);
      }
    }

    uint16_t keyPhase = 0x2108 + (logicCh << 8);
    if (hid->settings.contains(keyPhase)) {
      int32_t val = hid->settings[keyPhase];
      if (panelInputs->cbPhase[colMap]) {
        panelInputs->cbPhase[colMap]->SetValue(val ? true : false);
      }
    }

    uint16_t keyGain = 0x2109 + (logicCh << 8);
    if (!hid->settings.contains(keyGain)) {
      keyGain = 0x2105 + (logicCh << 8);
    }
    if (hid->settings.contains(keyGain)) {
      int32_t val = hid->settings[keyGain];
      if (colMap < 2) {
        if (panelInputs->slGainI[colMap]) {
          panelInputs->slGainI[colMap]->SetValue(val);
        }
      } else {
        if (inputLinked) {
          if (panelInputs->slGainICombined) {
            panelInputs->slGainICombined->SetValue(val);
          }
        } else {
          if (panelInputs->slGainI[logicCh]) {
            panelInputs->slGainI[logicCh]->SetValue(val);
          }
        }
      }
      if (panelInputs->lbGainVal[colMap]) {
        panelInputs->lbGainVal[colMap]->SetLabel(std::format("{:+} dB", val));
      }
    }
  }
  if (panelInputs)
    panelInputs->Layout();
}

void TPMixer::refreshMixerUi(int16_t bus) {
  if (bus < 0)
    bus = 0;
  for (int16_t stripIdx = 0; stripIdx < panelMixers->N_MIX_SRCS / 2;
       stripIdx++) {
    int l = stripIdx * 2;
    int r = l + 1;

    // Left source keys
    uint16_t keyL_L = ((0x61 + bus * 2) << 8) | (l + 1);
    uint16_t keyL_R = ((0x62 + bus * 2) << 8) | (l + 1);

    // Right source keys
    uint16_t keyR_L = ((0x61 + bus * 2) << 8) | (r + 1);
    uint16_t keyR_R = ((0x62 + bus * 2) << 8) | (r + 1);

    if (hid->settings.contains(keyL_L) && hid->settings.contains(keyL_R) &&
        hid->settings.contains(keyR_L) && hid->settings.contains(keyR_R)) {

      auto [mutedL, phaseL, panL, gainDBL] = lrGain2PandB(
          hid->settings[keyL_L], hid->settings[keyL_R], panelMixers->minGain);
      auto [mutedR, phaseR, panR, gainDBR] = lrGain2PandB(
          hid->settings[keyR_L], hid->settings[keyR_R], panelMixers->minGain);

      if (panelMixers->slPan[l])
        panelMixers->slPan[l]->SetValue(panL);
      if (panelMixers->slPan[r])
        panelMixers->slPan[r]->SetValue(panR);

      bool linked = hid->settings.contains(0x9100 + stripIdx)
                        ? (hid->settings[0x9100 + stripIdx] != 0)
                        : true;
      if (panelMixers->btnLink[stripIdx])
        panelMixers->btnLink[stripIdx]->SetValue(linked);

      if (linked) {
        if (panelMixers->slVol[stripIdx])
          panelMixers->slVol[stripIdx]->SetValue(gainDBL);
        if (panelMixers->ckSolo[stripIdx])
          panelMixers->ckSolo[stripIdx]->SetValue(false);
        if (panelMixers->ckMute[stripIdx])
          panelMixers->ckMute[stripIdx]->SetValue(mutedL);
        if (panelMixers->ckPhaseCombined[stripIdx])
          panelMixers->ckPhaseCombined[stripIdx]->SetValue(phaseL);
      } else {
        if (panelMixers->slVolL[stripIdx])
          panelMixers->slVolL[stripIdx]->SetValue(gainDBL);
        if (panelMixers->slVolR[stripIdx])
          panelMixers->slVolR[stripIdx]->SetValue(gainDBR);

        if (panelMixers->ckSoloL[stripIdx])
          panelMixers->ckSoloL[stripIdx]->SetValue(false);
        if (panelMixers->ckSoloR[stripIdx])
          panelMixers->ckSoloR[stripIdx]->SetValue(false);
        if (panelMixers->ckMuteL[stripIdx])
          panelMixers->ckMuteL[stripIdx]->SetValue(mutedL);
        if (panelMixers->ckMuteR[stripIdx])
          panelMixers->ckMuteR[stripIdx]->SetValue(mutedR);

        if (panelMixers->ckPhase[l])
          panelMixers->ckPhase[l]->SetValue(phaseL);
        if (panelMixers->ckPhase[r])
          panelMixers->ckPhase[r]->SetValue(phaseR);
      }

      if (panelMixers->slVol[stripIdx])
        panelMixers->slVol[stripIdx]->Show(linked);
      if (panelMixers->slVolL[stripIdx])
        panelMixers->slVolL[stripIdx]->Show(!linked);
      if (panelMixers->slVolR[stripIdx])
        panelMixers->slVolR[stripIdx]->Show(!linked);

      if (panelMixers->combBtnSizer[stripIdx])
        panelMixers->combBtnSizer[stripIdx]->Show(linked);
      if (panelMixers->splitBtnSizer[stripIdx])
        panelMixers->splitBtnSizer[stripIdx]->Show(!linked);

      if (panelMixers->lbVolVal[stripIdx]) {
        if (linked) {
          panelMixers->lbVolVal[stripIdx]->SetLabel(
              mutedL ? "-inf  -inf"
                     : std::format("{:.0f}  {:.0f}", (double)gainDBL,
                                   (double)gainDBL));
        } else {
          wxString lblL =
              mutedL ? "-inf" : std::format("{:.0f}", (double)gainDBL);
          wxString lblR =
              mutedR ? "-inf" : std::format("{:.0f}", (double)gainDBR);
          panelMixers->lbVolVal[stripIdx]->SetLabel(lblL + "  " + lblR);
        }
      }
    }
  }
  panelMixers->Layout();
}

void TPMixer::refreshLoopbackUi() {
  for (int16_t i = 0; i < panelLoopbacks->N_LOOPBACKS; i++) {
    if (!panelLoopbacks->btnLink[i])
      continue;
    int32_t valL = hid->settings[0x5103 + ((i * 2) << 8)];
    int32_t valR_real = hid->settings[0x5103 + ((i * 2 + 1) << 8)];

    auto [mutedL, dummyL, gainDBL] = gain2dB(valL);
    auto [mutedR, dummyR, gainDBR] = gain2dB(valR_real);
    bool linked = hid->settings.contains(0x9200 + i)
                      ? (hid->settings[0x9200 + i] != 0)
                      : true;

    if (panelLoopbacks->btnLink[i])
      panelLoopbacks->btnLink[i]->SetValue(linked);

    if (linked) {
      if (panelLoopbacks->slOutput[i])
        panelLoopbacks->slOutput[i]->SetValue(gainDBL);
      if (panelLoopbacks->ckMute[i])
        panelLoopbacks->ckMute[i]->SetValue(mutedL);
    } else {
      if (panelLoopbacks->slOutputL[i])
        panelLoopbacks->slOutputL[i]->SetValue(gainDBL);
      if (panelLoopbacks->slOutputR[i])
        panelLoopbacks->slOutputR[i]->SetValue(gainDBR);
      if (panelLoopbacks->ckMuteL[i])
        panelLoopbacks->ckMuteL[i]->SetValue(mutedL);
      if (panelLoopbacks->ckMuteR[i])
        panelLoopbacks->ckMuteR[i]->SetValue(mutedR);
    }

    if (panelLoopbacks->slOutput[i])
      panelLoopbacks->slOutput[i]->Show(linked);
    if (panelLoopbacks->slOutputL[i])
      panelLoopbacks->slOutputL[i]->Show(!linked);
    if (panelLoopbacks->slOutputR[i])
      panelLoopbacks->slOutputR[i]->Show(!linked);

    if (panelLoopbacks->combBtnSizer[i])
      panelLoopbacks->combBtnSizer[i]->Show(linked);
    if (panelLoopbacks->splitBtnSizer[i])
      panelLoopbacks->splitBtnSizer[i]->Show(!linked);

    if (panelLoopbacks->lbLoopVolVal[i]) {
      if (mutedL && mutedR) {
        panelLoopbacks->lbLoopVolVal[i]->SetLabel("-inf  -inf");
      } else {
        wxString lblL =
            mutedL ? "-inf" : std::format("{:.0f}", (double)gainDBL);
        wxString lblR =
            mutedR ? "-inf" : std::format("{:.0f}", (double)gainDBR);
        panelLoopbacks->lbLoopVolVal[i]->SetLabel(lblL + "  " + lblR);
      }
      int32_t selVal = hid->settings[0x5701 + (i << 8)];
      if (panelLoopbacks->cbSelect[i] && selVal >= 1 && selVal <= 14) {
        panelLoopbacks->cbSelect[i]->SetSelection(selVal - 1);
      }
    }
  }
  panelLoopbacks->Layout();
}

void TPMixer::refreshOutputUi() {
  int numOuts = (hid->pid == 0x8752)
                    ? 1
                    : ((hid->pid == 0x8755 || hid->pid == 0x8756) ? 2 : 3);
  for (int16_t i = 0; i < numOuts; i++) {
    int32_t valL = hid->settings[0x3103 + ((i * 2) << 8)];
    int32_t valR = hid->settings[0x3103 + ((i * 2 + 1) << 8)];

    auto [mutedL, dummyL, gainDBL] = gain2dB(valL);
    auto [mutedR, dummyR, gainDBR] = gain2dB(valR);
    bool linked = hid->settings.contains(0x9300 + i)
                      ? (hid->settings[0x9300 + i] != 0)
                      : true;

    if (panelOutputs->btnLink[i])
      panelOutputs->btnLink[i]->SetValue(linked);

    if (linked) {
      if (panelOutputs->slOutput[i])
        panelOutputs->slOutput[i]->SetValue(gainDBL);
      if (i > 0 && panelOutputs->ckMute[i])
        panelOutputs->ckMute[i]->SetValue(mutedL);
    } else {
      if (panelOutputs->slOutputL[i])
        panelOutputs->slOutputL[i]->SetValue(gainDBL);
      if (panelOutputs->slOutputR[i])
        panelOutputs->slOutputR[i]->SetValue(gainDBR);
      if (i > 0) {
        if (panelOutputs->ckMuteL[i])
          panelOutputs->ckMuteL[i]->SetValue(mutedL);
        if (panelOutputs->ckMuteR[i])
          panelOutputs->ckMuteR[i]->SetValue(mutedR);
      }
    }

    if (panelOutputs->slOutput[i])
      panelOutputs->slOutput[i]->Show(linked);
    if (panelOutputs->slOutputL[i])
      panelOutputs->slOutputL[i]->Show(!linked);
    if (panelOutputs->slOutputR[i])
      panelOutputs->slOutputR[i]->Show(!linked);

    if (i > 0) {
      if (panelOutputs->combBtnSizer[i])
        panelOutputs->combBtnSizer[i]->Show(linked);
      if (panelOutputs->splitBtnSizer[i])
        panelOutputs->splitBtnSizer[i]->Show(!linked);
    }

    if (panelOutputs->lbOutVolVal[i]) {
      if (mutedL && mutedR) {
        panelOutputs->lbOutVolVal[i]->SetLabel("-inf  -inf");
      } else {
        wxString lblL =
            mutedL ? "-inf" : std::format("{:.0f}", (double)gainDBL);
        wxString lblR =
            mutedR ? "-inf" : std::format("{:.0f}", (double)gainDBR);
        panelOutputs->lbOutVolVal[i]->SetLabel(lblL + "  " + lblR);
      }
      int32_t selVal = hid->settings[0x3501 + (i << 8)];
      if (panelOutputs->cbSelect[i] && selVal >= 1 && selVal <= 14) {
        panelOutputs->cbSelect[i]->SetSelection(selVal - 1);
      }
    }
  }
  uint8_t phoneBase = hid->phoneRegOffset();
  for (int phoneIdx = 0; phoneIdx < 2; ++phoneIdx) {
    uint16_t keyGain = ((phoneBase + phoneIdx) << 8) | 2;
    uint16_t keyMix = ((phoneBase + phoneIdx) << 8) | 3;
    if (hid->settings.contains(keyGain)) {
      panelOutputs->m_phoneGain[phoneIdx] = (hid->settings[keyGain] != 0);
    }
    if (hid->settings.contains(keyMix)) {
      int32_t val = hid->settings[keyMix];
      panelOutputs->m_phoneMix[phoneIdx] = (val - 50) * 2;
    }
  }
  int curPhone = panelOutputs->choicePhoneOut->GetSelection();
  if (curPhone != wxNOT_FOUND) {
    if (panelOutputs->mixKnob) {
      panelOutputs->mixKnob->SetValue(panelOutputs->m_phoneMix[curPhone]);
    }
    if (panelOutputs->btnPhoneGain) {
      panelOutputs->btnPhoneGain->SetValue(panelOutputs->m_phoneGain[curPhone]);
    }
    panelOutputs->updatePhoneMixLabel(panelOutputs->m_phoneMix[curPhone]);
  }

  if (panelOutputs->btnPhoneIcon[0]) {
    bool phoneVal =
        hid->settings.contains(0x3701) ? (hid->settings[0x3701] != 0) : true;
    panelOutputs->btnPhoneIcon[0]->SetValue(phoneVal);
  }
  if (panelOutputs->btnTRS[0]) {
    bool trsVal =
        hid->settings.contains(0x3703) ? (hid->settings[0x3703] != 0) : true;
    panelOutputs->btnTRS[0]->SetValue(trsVal);
  }
  if (panelOutputs->btnAUX[0]) {
    bool auxVal =
        hid->settings.contains(0x3704) ? (hid->settings[0x3704] != 0) : true;
    panelOutputs->btnAUX[0]->SetValue(auxVal);
  }

  panelOutputs->Layout();
}

void TPMixer::OnInputGain(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;

  int colMap = (ch >= 2) ? 2 : ch;
  bool inputLinked =
      (panelInputs->btnLink) ? panelInputs->btnLink->GetValue() : true;

  if (panelInputs->lbGainVal[colMap]) {
    int32_t gainDB = 0;
    if (colMap < 2) {
      if (panelInputs->slGainI[colMap])
        gainDB = panelInputs->slGainI[colMap]->GetValue();
      panelInputs->lbGainVal[colMap]->SetLabel(std::format("{:+} dB", gainDB));
    } else {
      if (inputLinked) {
        if (panelInputs->slGainICombined)
          gainDB = panelInputs->slGainICombined->GetValue();
        panelInputs->lbGainVal[colMap]->SetLabel(
            std::format("{:+} dB", gainDB));
      } else {
        int32_t gainDBL =
            panelInputs->slGainI[2] ? panelInputs->slGainI[2]->GetValue() : 0;
        int32_t gainDBR =
            panelInputs->slGainI[3] ? panelInputs->slGainI[3]->GetValue() : 0;
        panelInputs->lbGainVal[colMap]->SetLabel(
            std::format("{:+} / {:+}", gainDBL, gainDBR));
      }
    }
  }

  switch (id & (~0xf)) {
  case ID_INPUT_GAIN:
  case ID_INPUT_SOLO:
  case ID_INPUT_MUTE:
  case ID_INPUT_PHASE:
    sendInput(ch, true, id);
    break;
  case ID_INPUT_48V:
    if (panelInputs->cb48V[ch])
      hid->setInput48V(ch, panelInputs->cb48V[ch]->GetValue());
    break;
  case ID_INPUT_MON:
    if (panelInputs->cbMon[ch])
      hid->setInputMon(ch, panelInputs->cbMon[ch]->GetValue());
    break;
  case ID_INPUT_INST:
    if (panelInputs->cbInst[ch])
      hid->setInputInst(ch, panelInputs->cbInst[ch]->GetValue());
    break;
  }
}

void TPMixer::OnMixBusSel(wxCommandEvent &event) {
  int32_t val = event.GetInt();
  refreshMixerUi(val);
}

void TPMixer::OnMixVolume(wxCommandEvent &event) {
  // Not used in direct binding but kept for signature safety
}

void TPMixer::OnMixVolumeChanged(int stripIdx) {
  int32_t bus = 0;
  for (int t = 0; t < panelMixers->N_MIXERS; ++t) {
    if (panelMixers->tabButtons[t] && panelMixers->tabButtons[t]->GetValue()) {
      bus = t;
      break;
    }
  }

  bool linked = panelMixers->btnLink[stripIdx]
                    ? panelMixers->btnLink[stripIdx]->GetValue()
                    : true;

  int32_t panL = panelMixers->slPan[stripIdx * 2]
                     ? panelMixers->slPan[stripIdx * 2]->GetValue()
                     : -100;
  int32_t panR = panelMixers->slPan[stripIdx * 2 + 1]
                     ? panelMixers->slPan[stripIdx * 2 + 1]->GetValue()
                     : 100;

  bool anySolo = false;
  for (int i = 0; i < panelMixers->N_MIX_SRCS / 2; ++i) {
    if (panelMixers->btnLink[i] && panelMixers->btnLink[i]->GetValue()) {
      if (panelMixers->ckSolo[i] && panelMixers->ckSolo[i]->GetValue())
        anySolo = true;
    } else {
      if (panelMixers->ckSoloL[i] && panelMixers->ckSoloL[i]->GetValue())
        anySolo = true;
      if (panelMixers->ckSoloR[i] && panelMixers->ckSoloR[i]->GetValue())
        anySolo = true;
    }
  }

  int32_t gainDBL = 0, gainDBR = 0;
  bool muteL = false, muteR = false;
  bool soloL = false, soloR = false;
  bool phaseL = false, phaseR = false;

  if (linked) {
    int32_t gainDB = panelMixers->slVol[stripIdx]
                         ? panelMixers->slVol[stripIdx]->GetValue()
                         : 0;
    bool mute = panelMixers->ckMute[stripIdx]
                    ? panelMixers->ckMute[stripIdx]->GetValue()
                    : false;
    bool solo = panelMixers->ckSolo[stripIdx]
                    ? panelMixers->ckSolo[stripIdx]->GetValue()
                    : false;
    bool phase = panelMixers->ckPhaseCombined[stripIdx]
                     ? panelMixers->ckPhaseCombined[stripIdx]->GetValue()
                     : false;

    gainDBL = gainDBR = gainDB;
    muteL = muteR = mute;
    soloL = soloR = solo;
    phaseL = phaseR = phase;

    if (panelMixers->lbVolVal[stripIdx]) {
      if (mute || gainDB <= -60) {
        panelMixers->lbVolVal[stripIdx]->SetLabel("-inf  -inf");
      } else {
        panelMixers->lbVolVal[stripIdx]->SetLabel(
            std::format("{:.0f}  {:.0f}", (double)gainDB, (double)gainDB));
      }
    }
  } else {
    gainDBL = panelMixers->slVolL[stripIdx]
                  ? panelMixers->slVolL[stripIdx]->GetValue()
                  : 0;
    gainDBR = panelMixers->slVolR[stripIdx]
                  ? panelMixers->slVolR[stripIdx]->GetValue()
                  : 0;
    muteL = panelMixers->ckMuteL[stripIdx]
                ? panelMixers->ckMuteL[stripIdx]->GetValue()
                : false;
    muteR = panelMixers->ckMuteR[stripIdx]
                ? panelMixers->ckMuteR[stripIdx]->GetValue()
                : false;
    soloL = panelMixers->ckSoloL[stripIdx]
                ? panelMixers->ckSoloL[stripIdx]->GetValue()
                : false;
    soloR = panelMixers->ckSoloR[stripIdx]
                ? panelMixers->ckSoloR[stripIdx]->GetValue()
                : false;
    phaseL = panelMixers->ckPhase[stripIdx * 2]
                 ? panelMixers->ckPhase[stripIdx * 2]->GetValue()
                 : false;
    phaseR = panelMixers->ckPhase[stripIdx * 2 + 1]
                 ? panelMixers->ckPhase[stripIdx * 2 + 1]->GetValue()
                 : false;

    if (panelMixers->lbVolVal[stripIdx]) {
      wxString lblL = (muteL || gainDBL <= -60)
                          ? "-inf"
                          : std::format("{:.0f}", (double)gainDBL);
      wxString lblR = (muteR || gainDBR <= -60)
                          ? "-inf"
                          : std::format("{:.0f}", (double)gainDBR);
      panelMixers->lbVolVal[stripIdx]->SetLabel(lblL + "  " + lblR);
    }
  }

  auto [gL_L, gL_R] =
      gain->getStereoGain(gainDBL, muteL, soloL, anySolo, phaseL, panL);
  auto [gR_L, gR_R] =
      gain->getStereoGain(gainDBR, muteR, soloR, anySolo, phaseR, panR);
  hid->setMixVol(bus, stripIdx * 2, gL_L, gL_R);
  hid->settings[((0x61 + bus * 2) << 8) | (stripIdx * 2 + 1)] = gL_L;
  hid->settings[((0x62 + bus * 2) << 8) | (stripIdx * 2 + 1)] = gL_R;

  hid->setMixVol(bus, stripIdx * 2 + 1, gR_L, gR_R);
  hid->settings[((0x61 + bus * 2) << 8) | (stripIdx * 2 + 2)] = gR_L;
  hid->settings[((0x62 + bus * 2) << 8) | (stripIdx * 2 + 2)] = gR_R;
}

void TPMixer::OnOutputVolume(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  bool linked =
      panelOutputs->btnLink[ch] ? panelOutputs->btnLink[ch]->GetValue() : true;

  if (panelOutputs->lbOutVolVal[ch]) {
    if (linked) {
      int32_t val = panelOutputs->slOutput[ch]
                        ? panelOutputs->slOutput[ch]->GetValue()
                        : 0;
      if (val <= -60) {
        panelOutputs->lbOutVolVal[ch]->SetLabel("-inf  -inf");
      } else {
        panelOutputs->lbOutVolVal[ch]->SetLabel(
            std::format("{:.0f}  {:.0f}", (double)val, (double)val));
      }
    } else {
      int32_t valL = panelOutputs->slOutputL[ch]
                         ? panelOutputs->slOutputL[ch]->GetValue()
                         : 0;
      int32_t valR = panelOutputs->slOutputR[ch]
                         ? panelOutputs->slOutputR[ch]->GetValue()
                         : 0;
      wxString lblL =
          (valL <= -60) ? "-inf" : std::format("{:.0f}", (double)valL);
      wxString lblR =
          (valR <= -60) ? "-inf" : std::format("{:.0f}", (double)valR);
      panelOutputs->lbOutVolVal[ch]->SetLabel(lblL + "  " + lblR);
    }
  }

  setOutputVol(ch);
}

void TPMixer::OnOutputToggle(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();

  switch (id & (~0xf)) {
  case ID_OUTPUT_SEL:
    hid->setOutputSel(ch, val + 1);
    break;
  case ID_OUTPUT_MON:
    if (ch == 0) {
      hid->setOutputMon(ch, val);
    } else {
      setOutputVol(ch);
    }
    break;
  case ID_OUTPUT_LINE:
    hid->setOutputLine(ch, val);
    break;
  }
}

void TPMixer::OnLoopVolume(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  bool linked = panelLoopbacks->btnLink[ch]
                    ? panelLoopbacks->btnLink[ch]->GetValue()
                    : true;
  bool mute = panelLoopbacks->ckMute[ch]
                  ? panelLoopbacks->ckMute[ch]->GetValue()
                  : false;

  if (panelLoopbacks->lbLoopVolVal[ch]) {
    if (linked) {
      int32_t val = panelLoopbacks->slOutput[ch]
                        ? panelLoopbacks->slOutput[ch]->GetValue()
                        : 0;
      if (mute || val <= -60) {
        panelLoopbacks->lbLoopVolVal[ch]->SetLabel("-inf  -inf");
      } else {
        panelLoopbacks->lbLoopVolVal[ch]->SetLabel(
            std::format("{:.0f}  {:.0f}", (double)val, (double)val));
      }
    } else {
      int32_t valL = panelLoopbacks->slOutputL[ch]
                         ? panelLoopbacks->slOutputL[ch]->GetValue()
                         : 0;
      int32_t valR = panelLoopbacks->slOutputR[ch]
                         ? panelLoopbacks->slOutputR[ch]->GetValue()
                         : 0;
      bool muteL = panelLoopbacks->ckMuteL[ch]
                       ? panelLoopbacks->ckMuteL[ch]->GetValue()
                       : false;
      bool muteR = panelLoopbacks->ckMuteR[ch]
                       ? panelLoopbacks->ckMuteR[ch]->GetValue()
                       : false;
      wxString lblL =
          (muteL || valL <= -60) ? "-inf" : std::format("{:.0f}", (double)valL);
      wxString lblR =
          (muteR || valR <= -60) ? "-inf" : std::format("{:.0f}", (double)valR);
      panelLoopbacks->lbLoopVolVal[ch]->SetLabel(lblL + "  " + lblR);
    }
  }

  setLoopVol(ch);
}

void TPMixer::OnLoopToggle(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t ch = id & 0x0f;
  int32_t val = event.GetInt();

  if ((id & (~0xf)) == ID_LOOP_SEL) {
    hid->setLoopSel(ch, val + 1);
  } else {
    setLoopVol(ch);
  }
}

void TPMixer::OnPhoneMix(wxCommandEvent &event) {
  int32_t val = event.GetInt();
  int idx = panelOutputs->choicePhoneOut->GetSelection();
  if (idx == wxNOT_FOUND)
    return;
  panelOutputs->m_phoneMix[idx] = val;
  panelOutputs->updatePhoneMixLabel(val);
  hid->setPhoneMix(idx, val);
}

void TPMixer::OnPhoneGain(wxCommandEvent &event) {
  int32_t val = event.GetInt();
  int idx = panelOutputs->choicePhoneOut->GetSelection();
  if (idx == wxNOT_FOUND)
    return;
  panelOutputs->m_phoneGain[idx] = val ? true : false;
  hid->setPhoneGainBoost(idx, val);
}

void TPMixer::OnClose(wxCloseEvent &event) {
  toStopHidReader = true;
  if (thReader && thReader->joinable()) {
    thReader->join();
  }
  delete thReader;
  thReader = nullptr;

  bool autoSave =
      hid->settings.contains(0x9001) ? (hid->settings[0x9001] != 0) : true;
  if (autoSave) {
    saveSettings();
  }
  delete hid;
  hid = nullptr;
  delete gain;
  gain = nullptr;

  event.Skip();
}

void TPMixer::OnLoad(wxCommandEvent &event) {
  if (loadSettings()) {
    refreshInputsUi();
    refreshMixerUi(-1);
    refreshLoopbackUi();
    refreshOutputUi();
    if (NULL != hid->getHandle()) {
      pushGuiStateToDevice();
    }
  }
}

void TPMixer::OnSave(wxCommandEvent &event) { saveSettings(); }

void TPMixer::OnDeviceSave(wxCommandEvent &event) { hid->saveDeviceDefault(); }

void TPMixer::OnResetDefaults(wxCommandEvent &event) {
  refreshInputsUi();
  refreshMixerUi(-1);
  refreshLoopbackUi();
  refreshOutputUi();
  if (panelInputs)
    panelInputs->Layout();
  if (panelOutputs)
    panelOutputs->Layout();
  if (panelLoopbacks)
    panelLoopbacks->Layout();
  Layout();

  if (NULL != hid->getHandle()) {
    pushGuiStateToDevice();
  }
  saveSettings();
}

void TPMixer::OnInputPeak(wxCommandEvent &event) {
  uint32_t id = event.GetId();
  uint32_t ch = id - ID_INPUT_PEAK;
  if (ch < panelInputs->N_INPUTS) {
    panelInputs->PeaksI[ch] = panelInputs->LEVEL_MIN;
    if (panelInputs->lbPeaksI[ch]) {
      panelInputs->lbPeaksI[ch]->SetLabel("-120.0");
    }
  }
}

void TPMixer::OnLinkToggle(wxCommandEvent &event) {
  int32_t id = event.GetId();
  int32_t cls = id & (~0xf);
  int32_t ch = id & 0x0f;

  if (cls == ID_INPUT_LINK) {
    bool linked = panelInputs->btnLink->GetValue();
    hid->settings[0x9000] = linked ? 1 : 0;
    if (linked) {
      int32_t lVal =
          panelInputs->slGainI[2] ? panelInputs->slGainI[2]->GetValue() : 0;
      if (panelInputs->slGainICombined)
        panelInputs->slGainICombined->SetValue(lVal);
      if (panelInputs->cbMute[2] && panelInputs->cbMuteL)
        panelInputs->cbMute[2]->SetValue(panelInputs->cbMuteL->GetValue());
      if (panelInputs->cbSolo[2] && panelInputs->cbSoloL)
        panelInputs->cbSolo[2]->SetValue(panelInputs->cbSoloL->GetValue());
      if (panelInputs->cbPhase[2] && panelInputs->cbPhaseL)
        panelInputs->cbPhase[2]->SetValue(panelInputs->cbPhaseL->GetValue());
    } else {
      int32_t combVal = panelInputs->slGainICombined
                            ? panelInputs->slGainICombined->GetValue()
                            : 0;
      bool combMute =
          panelInputs->cbMute[2] ? panelInputs->cbMute[2]->GetValue() : false;
      bool combSolo =
          panelInputs->cbSolo[2] ? panelInputs->cbSolo[2]->GetValue() : false;
      bool combPhase =
          panelInputs->cbPhase[2] ? panelInputs->cbPhase[2]->GetValue() : false;

      if (panelInputs->slGainI[2])
        panelInputs->slGainI[2]->SetValue(combVal);
      if (panelInputs->slGainI[3])
        panelInputs->slGainI[3]->SetValue(combVal);
      if (panelInputs->cbMuteL)
        panelInputs->cbMuteL->SetValue(combMute);
      if (panelInputs->cbMuteR)
        panelInputs->cbMuteR->SetValue(combMute);
      if (panelInputs->cbSoloL)
        panelInputs->cbSoloL->SetValue(combSolo);
      if (panelInputs->cbSoloR)
        panelInputs->cbSoloR->SetValue(combSolo);
      if (panelInputs->cbPhaseL)
        panelInputs->cbPhaseL->SetValue(combPhase);
      if (panelInputs->cbPhaseR)
        panelInputs->cbPhaseR->SetValue(combPhase);
    }
    if (panelInputs->slGainICombined)
      panelInputs->slGainICombined->Show(linked);
    if (panelInputs->slGainI[2])
      panelInputs->slGainI[2]->Show(!linked);
    if (panelInputs->slGainI[3])
      panelInputs->slGainI[3]->Show(!linked);

    if (panelInputs->combBtnSizer)
      panelInputs->combBtnSizer->Show(linked);
    if (panelInputs->splitBtnSizer)
      panelInputs->splitBtnSizer->Show(!linked);

    panelInputs->Layout();
    sendInput(2, true);
  } else if (cls == ID_MIX_LINK) {
    bool linked = panelMixers->btnLink[ch]->GetValue();
    hid->settings[0x9100 + ch] = linked ? 1 : 0;
    if (linked) {
      int32_t lVal =
          panelMixers->slVolL[ch] ? panelMixers->slVolL[ch]->GetValue() : 0;
      if (panelMixers->slVol[ch])
        panelMixers->slVol[ch]->SetValue(lVal);
      if (panelMixers->ckMute[ch] && panelMixers->ckMuteL[ch])
        panelMixers->ckMute[ch]->SetValue(panelMixers->ckMuteL[ch]->GetValue());
      if (panelMixers->ckSolo[ch] && panelMixers->ckSoloL[ch])
        panelMixers->ckSolo[ch]->SetValue(panelMixers->ckSoloL[ch]->GetValue());
      if (panelMixers->ckPhaseCombined[ch] && panelMixers->ckPhase[ch * 2])
        panelMixers->ckPhaseCombined[ch]->SetValue(
            panelMixers->ckPhase[ch * 2]->GetValue());
    } else {
      int32_t combVal =
          panelMixers->slVol[ch] ? panelMixers->slVol[ch]->GetValue() : 0;
      bool combMute =
          panelMixers->ckMute[ch] ? panelMixers->ckMute[ch]->GetValue() : false;
      bool combSolo =
          panelMixers->ckSolo[ch] ? panelMixers->ckSolo[ch]->GetValue() : false;
      bool combPhase = panelMixers->ckPhaseCombined[ch]
                           ? panelMixers->ckPhaseCombined[ch]->GetValue()
                           : false;

      if (panelMixers->slVolL[ch])
        panelMixers->slVolL[ch]->SetValue(combVal);
      if (panelMixers->slVolR[ch])
        panelMixers->slVolR[ch]->SetValue(combVal);
      if (panelMixers->ckMuteL[ch])
        panelMixers->ckMuteL[ch]->SetValue(combMute);
      if (panelMixers->ckMuteR[ch])
        panelMixers->ckMuteR[ch]->SetValue(combMute);
      if (panelMixers->ckSoloL[ch])
        panelMixers->ckSoloL[ch]->SetValue(combSolo);
      if (panelMixers->ckSoloR[ch])
        panelMixers->ckSoloR[ch]->SetValue(combSolo);
      if (panelMixers->ckPhase[ch * 2])
        panelMixers->ckPhase[ch * 2]->SetValue(combPhase);
      if (panelMixers->ckPhase[ch * 2 + 1])
        panelMixers->ckPhase[ch * 2 + 1]->SetValue(combPhase);
    }
    if (panelMixers->slVol[ch])
      panelMixers->slVol[ch]->Show(linked);
    if (panelMixers->slVolL[ch])
      panelMixers->slVolL[ch]->Show(!linked);
    if (panelMixers->slVolR[ch])
      panelMixers->slVolR[ch]->Show(!linked);

    if (panelMixers->combBtnSizer[ch])
      panelMixers->combBtnSizer[ch]->Show(linked);
    if (panelMixers->splitBtnSizer[ch])
      panelMixers->splitBtnSizer[ch]->Show(!linked);

    panelMixers->Layout();
    this->OnMixVolumeChanged(ch);
  } else if (cls == ID_LOOP_LINK) {
    bool linked = panelLoopbacks->btnLink[ch]->GetValue();
    hid->settings[0x9200 + ch] = linked ? 1 : 0;
    if (linked) {
      int32_t lVal = panelLoopbacks->slOutputL[ch]
                         ? panelLoopbacks->slOutputL[ch]->GetValue()
                         : 0;
      if (panelLoopbacks->slOutput[ch])
        panelLoopbacks->slOutput[ch]->SetValue(lVal);
      if (panelLoopbacks->ckMute[ch] && panelLoopbacks->ckMuteL[ch])
        panelLoopbacks->ckMute[ch]->SetValue(
            panelLoopbacks->ckMuteL[ch]->GetValue());
    } else {
      int32_t combVal = panelLoopbacks->slOutput[ch]
                            ? panelLoopbacks->slOutput[ch]->GetValue()
                            : 0;
      bool combMute = panelLoopbacks->ckMute[ch]
                          ? panelLoopbacks->ckMute[ch]->GetValue()
                          : false;
      if (panelLoopbacks->slOutputL[ch])
        panelLoopbacks->slOutputL[ch]->SetValue(combVal);
      if (panelLoopbacks->slOutputR[ch])
        panelLoopbacks->slOutputR[ch]->SetValue(combVal);
      if (panelLoopbacks->ckMuteL[ch])
        panelLoopbacks->ckMuteL[ch]->SetValue(combMute);
      if (panelLoopbacks->ckMuteR[ch])
        panelLoopbacks->ckMuteR[ch]->SetValue(combMute);
    }
    if (panelLoopbacks->slOutput[ch])
      panelLoopbacks->slOutput[ch]->Show(linked);
    if (panelLoopbacks->slOutputL[ch])
      panelLoopbacks->slOutputL[ch]->Show(!linked);
    if (panelLoopbacks->slOutputR[ch])
      panelLoopbacks->slOutputR[ch]->Show(!linked);

    if (panelLoopbacks->combBtnSizer[ch])
      panelLoopbacks->combBtnSizer[ch]->Show(linked);
    if (panelLoopbacks->splitBtnSizer[ch])
      panelLoopbacks->splitBtnSizer[ch]->Show(!linked);

    panelLoopbacks->Layout();
    setLoopVol(ch);
  } else if (cls == ID_OUTPUT_LINK) {
    bool linked = panelOutputs->btnLink[ch]->GetValue();
    hid->settings[0x9300 + ch] = linked ? 1 : 0;
    if (linked) {
      int32_t lVal = panelOutputs->slOutputL[ch]
                         ? panelOutputs->slOutputL[ch]->GetValue()
                         : 0;
      if (panelOutputs->slOutput[ch])
        panelOutputs->slOutput[ch]->SetValue(lVal);
      if (ch > 0 && panelOutputs->ckMute[ch] && panelOutputs->ckMuteL[ch])
        panelOutputs->ckMute[ch]->SetValue(
            panelOutputs->ckMuteL[ch]->GetValue());
    } else {
      int32_t combVal = panelOutputs->slOutput[ch]
                            ? panelOutputs->slOutput[ch]->GetValue()
                            : 0;
      bool combMute = (ch > 0 && panelOutputs->ckMute[ch])
                          ? panelOutputs->ckMute[ch]->GetValue()
                          : false;
      if (panelOutputs->slOutputL[ch])
        panelOutputs->slOutputL[ch]->SetValue(combVal);
      if (panelOutputs->slOutputR[ch])
        panelOutputs->slOutputR[ch]->SetValue(combVal);
      if (ch > 0) {
        if (panelOutputs->ckMuteL[ch])
          panelOutputs->ckMuteL[ch]->SetValue(combMute);
        if (panelOutputs->ckMuteR[ch])
          panelOutputs->ckMuteR[ch]->SetValue(combMute);
      }
    }
    if (panelOutputs->slOutput[ch])
      panelOutputs->slOutput[ch]->Show(linked);
    if (panelOutputs->slOutputL[ch])
      panelOutputs->slOutputL[ch]->Show(!linked);
    if (panelOutputs->slOutputR[ch])
      panelOutputs->slOutputR[ch]->Show(!linked);

    if (ch > 0) {
      if (panelOutputs->combBtnSizer[ch])
        panelOutputs->combBtnSizer[ch]->Show(linked);
      if (panelOutputs->splitBtnSizer[ch])
        panelOutputs->splitBtnSizer[ch]->Show(!linked);
    }

    panelOutputs->Layout();
    setOutputVol(ch);
  }
}

class MyApp : public wxApp {
public:
  bool OnInit() override {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    wxInitAllImageHandlers();
    TPMixer *frame = new TPMixer();
    frame->Show(true);
    return true;
  }
  int OnExit() override {
    hid_exit();
    return wxApp::OnExit();
  }
};

wxIMPLEMENT_APP(MyApp);
