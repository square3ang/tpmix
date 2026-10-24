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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <thread>
#include <tuple>

#include <QtWidgets>

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

// Gain calculation class
class Gain {
protected:
  constexpr static int32_t POS_RANGE = 36;
  constexpr static int32_t NEG_RANGE = 90;
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
  uint8_t getHidInputChannel(int16_t ch) const {
    if (pid == 0x8756) {
      if (ch == 0) return 0x21; // Analog IN 1
      if (ch == 1) return 0x23; // Analog IN 2
      if (ch == 2) return 0x22; // Mobile In
    }
    return 0x21 + ch;
  }
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
      uint16_t base = (getHidInputChannel(i) << 8);
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
      settings[0x3704] = 1; // AUX default: ON (E4x4)
    if (force || !settings.contains(0x3705))
      settings[0x3705] = 1; // AUX default: ON (E2x2 OTG)

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
    buf[5] = getHidInputChannel(ch);
    buf[6] = 0x05;
    write32BE(&buf[7], gain);
    enqueue(exec);
  };
  void setInput48V(int16_t ch, bool pOn, bool exec = true) {
    buf[5] = getHidInputChannel(ch);
    buf[6] = 0x02;
    int32_t on32 = pOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };
  void setInputMon(int16_t ch, bool MonOn, bool exec = true) {
    buf[5] = getHidInputChannel(ch);
    buf[6] = 0x01;
    int32_t on32 = MonOn ? 1 : 0;
    write32BE(&buf[7], on32);
    enqueue(exec);
  };
  void setInputInst(int16_t ch, bool InstOn, bool exec = true) {
    buf[5] = getHidInputChannel(ch);
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
  ICON_HEADPHONE,
  ICON_JACK
};

static QString qs(const std::string &s) { return QString::fromStdString(s); }

// Colors sampled from the official Control Center screenshots
static const QColor COL_WINDOW(0, 0, 0), COL_HEADER(24, 24, 24),
    COL_SECTION(31, 31, 31), COL_STRIP(22, 22, 22), COL_TITLEBOX(52, 52, 52),
    COL_TITLE(185, 185, 185), COL_BTN(58, 58, 58), COL_BTN_TXT(175, 175, 175),
    COL_METER_BG(46, 46, 46), COL_SCALE(150, 150, 150),
    COL_TRACK(160, 160, 160), COL_HANDLE(216, 216, 218),
    COL_GREEN(89, 155, 34), COL_RED(192, 57, 43), COL_YELLOW(212, 172, 13),
    COL_BLUE(60, 92, 170), COL_ORANGE(211, 120, 0), COL_VAL(220, 220, 220);

static QVBoxLayout *vbox(QWidget *w = nullptr, int margin = 0, int spacing = 0) {
  auto *l = new QVBoxLayout(w);
  l->setContentsMargins(margin, margin, margin, margin);
  l->setSpacing(spacing);
  return l;
}

static QHBoxLayout *hbox(QWidget *w = nullptr, int margin = 0, int spacing = 0) {
  auto *l = new QHBoxLayout(w);
  l->setContentsMargins(margin, margin, margin, margin);
  l->setSpacing(spacing);
  return l;
}

static void setBg(QWidget *w, const QColor &bg) {
  w->setAutoFillBackground(true);
  QPalette p = w->palette();
  p.setColor(QPalette::Window, bg);
  w->setPalette(p);
}

static QWidget *mkPanel(QWidget *parent, const QColor &bg, const char *name) {
  auto *w = new QWidget(parent);
  w->setObjectName(name);
  setBg(w, bg);
  return w;
}

// basePt is re-applied (times font scale) by ScaleUIElements
static QLabel *mkLabel(QWidget *parent, const QString &text, double pt,
                       bool bold, const QColor &col,
                       Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter) {
  auto *l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSizeF(pt);
  f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
  l->setFont(f);
  l->setProperty("basePt", pt);
  QPalette p = l->palette();
  p.setColor(QPalette::WindowText, col);
  l->setPalette(p);
  l->setAlignment(align);
  return l;
}

// Rounded grey header box on top of every channel strip
static QLabel *mkTitleBox(QWidget *parent, const QString &text) {
  QLabel *l = mkLabel(parent, text, 9.5, true, COL_TITLE, Qt::AlignCenter);
  l->setObjectName("titleBox");
  return l;
}

// "Input |" section heading
static QHBoxLayout *titleRow(QWidget *parent, const QString &title) {
  auto *row = hbox(nullptr, 0, 12);
  row->setContentsMargins(10, 6, 0, 4);
  row->addWidget(mkLabel(parent, title, 11, true, QColor(200, 200, 200)));
  auto *bar = new QWidget(parent);
  bar->setObjectName("titleBar");
  setBg(bar, QColor(161, 161, 163));
  bar->setFixedSize(3, 22);
  row->addWidget(bar, 0, Qt::AlignVCenter);
  return row;
}

static double fontScaleFor(double scale) {
  return std::clamp(1.0 + (scale - 1.0) * 0.4, 0.8, 1.25);
}

static QString dbText(int v, int minVal) {
  if (v <= minVal)
    return QString::fromUtf8("-∞");
  return v == 0 ? "-0" : QString::number(v);
}

enum ButtonStyle {
  BTN_BOX,  // grey box, filled with the active color when on
  BTN_TEXT, // bare text, white when on, dim when off (Link)
  BTN_TAB,  // colored text, outlined when selected (Mix A..D)
  BTN_ICON, // bare icon (header)
};

class CustomButton : public QWidget {
public:
  QSize m_designSize;
  std::function<void(bool)> onClick;

  CustomButton(QWidget *parent, const QString &label, bool isToggle = false,
               QSize size = QSize(45, 14),
               const QColor &activeCol = COL_BLUE,
               IconType iconType = ICON_NONE, ButtonStyle style = BTN_BOX)
      : QWidget(parent), m_designSize(size), m_label(label),
        m_isToggle(isToggle), m_activeCol(activeCol), m_iconType(iconType),
        m_style(style) {
    if (m_designSize.width() <= 0 || m_designSize.height() <= 0)
      m_designSize = QSize(45, 14);
    Rescale(g_uiScale);
  }

  void SetValue(bool value) {
    if (m_isChecked != value) {
      m_isChecked = value;
      update();
    }
  }
  bool GetValue() const { return m_isChecked; }

  void SetLabel(const QString &label) {
    m_label = label;
    update();
  }

  void Rescale(double scale) {
    int w = m_designSize.width() * scale, h = m_designSize.height() * scale;
    setMinimumSize(w, h);
    // narrow split buttons stretch to their column
    setMaximumSize(m_designSize.width() <= 18 ? QWIDGETSIZE_MAX : w, h);
  }

protected:
  QSize sizeHint() const override { return minimumSize(); }

  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(g_uiScale, g_uiScale);
    double dx = width() / g_uiScale, dy = height() / g_uiScale;
    bool on = m_isToggle && m_isChecked;

    QColor bgCol = Qt::transparent;
    QColor fgCol = COL_BTN_TXT;
    if (m_style == BTN_BOX) {
      bgCol = on ? m_activeCol : COL_BTN;
      if (!on && m_isHovered)
        bgCol = COL_BTN.lighter(130);
      if (m_isPressed && !m_isToggle)
        bgCol = COL_BTN.darker(130);
      fgCol = on ? QColor(255, 255, 255) : COL_BTN_TXT;
    } else if (m_style == BTN_TEXT) {
      fgCol = !m_isToggle ? QColor(200, 200, 200)
              : on        ? QColor(240, 240, 240)
                          : QColor(80, 80, 80);
      if (m_isHovered)
        fgCol = fgCol.lighter(130);
    } else if (m_style == BTN_TAB) {
      fgCol = m_activeCol;
      if (on) {
        p.setPen(QPen(QColor(126, 126, 126), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1, 1, dx - 2, dy - 2), 3, 3);
      } else if (m_isHovered) {
        fgCol = fgCol.lighter(130);
      }
    } else {
      fgCol = m_isHovered ? QColor(255, 255, 255) : QColor(225, 225, 225);
      if (m_isPressed)
        fgCol = QColor(150, 150, 150);
    }
    if (!isEnabled()) {
      fgCol = QColor(90, 90, 90);
      if (m_style == BTN_BOX)
        bgCol = QColor(40, 40, 40);
    }
    if (bgCol.alpha()) {
      p.setPen(Qt::NoPen);
      p.setBrush(bgCol);
      p.drawRoundedRect(QRectF(0, 0, dx, dy), 2.5, 2.5);
    }

    double cx = dx / 2.0, cy = dy / 2.0;
    if (m_iconType == ICON_NONE) {
      double fontScale = fontScaleFor(g_uiScale);
      QFont f = font();
      double pt = m_style == BTN_TAB ? 10.5 : (dy < 14.0 ? 7 : 8);
      f.setPointSizeF(std::max(5.0, pt * fontScale));
      f.setWeight(m_style == BTN_TEXT && !on && m_isToggle ? QFont::Normal
                                                           : QFont::DemiBold);
      p.setFont(f);
      p.setPen(fgCol);
      p.drawText(QRectF(0, 0, dx, dy), Qt::AlignCenter, m_label);
      return;
    }

    p.setPen(QPen(fgCol, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    double k = std::min(dx, dy) / 20.0; // icons drawn for a 20x20 box
    if (m_iconType == ICON_GEAR) {
      p.setBrush(fgCol);
      QPainterPath gear;
      for (int i = 0; i < 8; ++i) {
        double a = i * M_PI / 4.0;
        QPointF c(cx + 6.2 * k * cos(a), cy + 6.2 * k * sin(a));
        gear.addEllipse(c, 2.0 * k, 2.0 * k);
      }
      gear.addEllipse(QPointF(cx, cy), 6.3 * k, 6.3 * k);
      gear.setFillRule(Qt::WindingFill);
      QPainterPath hole;
      hole.addEllipse(QPointF(cx, cy), 2.6 * k, 2.6 * k);
      p.setPen(Qt::NoPen);
      p.drawPath(gear.simplified().subtracted(hole));
    } else if (m_iconType == ICON_SAVE) {
      double w = 14 * k, h = 14 * k, x = cx - w / 2.0, y = cy - h / 2.0;
      QPainterPath path;
      path.moveTo(x, y);
      path.lineTo(x + w - 3 * k, y);
      path.lineTo(x + w, y + 3 * k);
      path.lineTo(x + w, y + h);
      path.lineTo(x, y + h);
      path.closeSubpath();
      QPainterPath body = path;
      QPainterPath cut;
      cut.addRect(QRectF(x + 3 * k, y + 1.5 * k, 6 * k, 3.5 * k));
      cut.addRect(QRectF(x + 3 * k, y + 8 * k, w - 6 * k, 4.5 * k));
      p.setPen(Qt::NoPen);
      p.setBrush(fgCol);
      p.drawPath(body.subtracted(cut));
    } else if (m_iconType == ICON_DOWNLOAD) {
      p.setPen(QPen(fgCol, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.drawLine(QPointF(cx, cy - 7 * k), QPointF(cx, cy + 3 * k));
      p.drawLine(QPointF(cx - 4 * k, cy - 1 * k), QPointF(cx, cy + 3 * k));
      p.drawLine(QPointF(cx + 4 * k, cy - 1 * k), QPointF(cx, cy + 3 * k));
      QPainterPath tray;
      tray.moveTo(cx - 7 * k, cy + 2 * k);
      tray.lineTo(cx - 7 * k, cy + 7 * k);
      tray.lineTo(cx + 7 * k, cy + 7 * k);
      tray.lineTo(cx + 7 * k, cy + 2 * k);
      p.drawPath(tray);
    } else if (m_iconType == ICON_HEADPHONE) {
      double r = 5.0;
      QRectF arcRect(cx - r, cy + 1.0 - r, 2 * r, 2 * r);
      QPainterPath path;
      path.arcMoveTo(arcRect, 180);
      path.arcTo(arcRect, 180, -180);
      p.drawPath(path);
      p.setPen(Qt::NoPen);
      p.setBrush(fgCol);
      p.drawRoundedRect(QRectF(cx - r - 1.2, cy, 3.0, 4.5), 1, 1);
      p.drawRoundedRect(QRectF(cx + r - 1.8, cy, 3.0, 4.5), 1, 1);
    } else if (m_iconType == ICON_JACK) {
      // 1/4" plug lying diagonally
      p.translate(cx, cy);
      p.rotate(-45);
      p.setPen(Qt::NoPen);
      p.setBrush(fgCol);
      p.drawRoundedRect(QRectF(-2.0, -1.6, 5.5, 3.2), 0.8, 0.8);
      p.drawRect(QRectF(3.5, -0.8, 3.5, 1.6));
      p.setPen(QPen(fgCol, 1.4, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(-2.0, 0), QPointF(-7.0, 0));
    }
  }

  void mousePressEvent(QMouseEvent *e) override {
    if (!isEnabled() || e->button() != Qt::LeftButton)
      return;
    m_isPressed = true;
    if (m_isToggle) {
      m_isChecked = !m_isChecked;
      if (onClick)
        onClick(m_isChecked);
    }
    update();
  }

  void mouseReleaseEvent(QMouseEvent *e) override {
    if (e->button() != Qt::LeftButton)
      return;
    bool wasPressed = m_isPressed;
    m_isPressed = false;
    update();
    if (!m_isToggle && wasPressed && rect().contains(e->pos()) && onClick)
      onClick(m_isChecked);
  }

  void enterEvent(QEnterEvent *) override {
    m_isHovered = true;
    update();
  }

  void leaveEvent(QEvent *) override {
    m_isHovered = false;
    update();
  }

private:
  QString m_label;
  bool m_isToggle;
  bool m_isChecked = false;
  bool m_isPressed = false;
  bool m_isHovered = false;
  QColor m_activeCol;
  IconType m_iconType;
  ButtonStyle m_style;
};

class KnobControl : public QWidget {
public:
  QSize m_designSize;
  std::function<void(int)> onChange;

  KnobControl(QWidget *parent, const QString &title, int minVal, int maxVal,
              int defaultVal, QSize size = QSize(40, 50), bool showValue = false)
      : QWidget(parent), m_designSize(size), m_title(title), m_val(defaultVal),
        m_min(minVal), m_max(maxVal), m_default(defaultVal),
        m_showValue(showValue) {
    if (m_designSize.width() <= 0 || m_designSize.height() <= 0)
      m_designSize = QSize(40, 50);
    Rescale(g_uiScale);
  }

  void SetValue(int val) {
    val = std::clamp(val, m_min, m_max);
    if (m_val != val) {
      m_val = val;
      update();
    }
  }
  int GetValue() const { return m_val; }

  void Rescale(double scale) {
    setFixedSize(m_designSize.width() * scale, m_designSize.height() * scale);
  }

protected:
  double angleOf(int v) const {
    double pct = (double)(v - m_min) / (m_max - m_min);
    return -135.0 + pct * 270.0; // degrees clockwise from 12 o'clock
  }

  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(g_uiScale, g_uiScale);
    double dx = width() / g_uiScale, dy = height() / g_uiScale;

    double textH = m_showValue ? 14 : 0;
    double r = std::min(dx, dy - textH) / 2.0 - 3.5;
    QPointF c(dx / 2.0, (dy - textH) / 2.0);
    QColor ring = isEnabled() ? QColor(153, 153, 153) : QColor(80, 80, 80);
    QColor white = isEnabled() ? QColor(250, 250, 250) : QColor(110, 110, 110);

    p.setBrush(QColor(42, 42, 42));
    p.setPen(QPen(ring, std::max(1.2, r * 0.07)));
    p.drawEllipse(c, r, r);

    // white arc from the zero/centre position to the current value
    double a0 = angleOf(std::clamp(0, m_min, m_max)), a1 = angleOf(m_val);
    if (std::abs(a1 - a0) > 0.5) {
      double ra = r + 2.0;
      QRectF arcRect(c.x() - ra, c.y() - ra, 2 * ra, 2 * ra);
      p.setPen(QPen(white, std::max(1.4, r * 0.09), Qt::SolidLine, Qt::FlatCap));
      p.setBrush(Qt::NoBrush);
      // Qt: 0deg = 3 o'clock, positive = counter-clockwise
      p.drawArc(arcRect, (int)((90 - a0) * 16), (int)(-(a1 - a0) * 16));
    }

    // pointer triangle near the rim
    double a = angleOf(m_val) * M_PI / 180.0;
    QPointF dir(sin(a), -cos(a)), perp(cos(a), sin(a));
    double tipR = r * 0.78, baseR = r * 0.50, half = r * 0.17;
    QPolygonF tri;
    tri << c + dir * tipR << c + dir * baseR + perp * half
        << c + dir * baseR - perp * half;
    p.setPen(Qt::NoPen);
    p.setBrush(white);
    p.drawPolygon(tri);

    if (m_showValue) {
      QFont f = font();
      f.setPointSizeF(8.5);
      f.setWeight(QFont::DemiBold);
      p.setFont(f);
      p.setPen(isEnabled() ? QColor(200, 200, 200) : QColor(80, 80, 80));
      QString valStr = m_val == 0 ? "+0" : qs(std::format("{:+}", m_val));
      p.drawText(QRectF(0, dy - textH, dx, textH), Qt::AlignCenter, valStr);
    }
  }

  void mousePressEvent(QMouseEvent *e) override {
    if (!isEnabled() || e->button() != Qt::LeftButton)
      return;
    m_isDragging = true;
    m_dragStartY = e->position().y();
    m_dragStartVal = m_val;
  }

  void mouseReleaseEvent(QMouseEvent *) override { m_isDragging = false; }

  void mouseMoveEvent(QMouseEvent *e) override {
    if (!m_isDragging)
      return;
    int dy = m_dragStartY - e->position().y();
    int delta = dy * ((m_max - m_min) / 120.0);
    if (delta == 0 && dy != 0)
      delta = (dy > 0) ? 1 : -1;
    setUserValue(m_dragStartVal + delta);
  }

  void mouseDoubleClickEvent(QMouseEvent *) override {
    if (isEnabled())
      setUserValue(m_default);
  }

  void wheelEvent(QWheelEvent *e) override {
    if (isEnabled() && e->angleDelta().y())
      setUserValue(m_val + (e->angleDelta().y() > 0 ? 1 : -1));
  }

public:
  void setUserValue(int v) {
    v = std::clamp(v, m_min, m_max);
    if (v == m_val)
      return;
    m_val = v;
    update();
    if (onChange)
      onChange(m_val);
  }

private:
  QString m_title;
  int m_val, m_min, m_max, m_default;
  bool m_showValue;
  bool m_isDragging = false;
  double m_dragStartY = 0;
  int m_dragStartVal = 0;
};

// Combo drawn like the original: bold text and a small triangle, optionally
// on a grey box ("boxCombo")
class FlatCombo : public QComboBox {
public:
  FlatCombo(QWidget *parent, const char *name) : QComboBox(parent) {
    setObjectName(name);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = rect();
    bool box = objectName() == "boxCombo";
    if (box) {
      p.setPen(Qt::NoPen);
      p.setBrush(COL_TITLEBOX);
      p.drawRoundedRect(r, 4, 4);
    }
    QColor fg = box ? QColor(185, 185, 185) : QColor(225, 225, 225);
    if (underMouse())
      fg = fg.lighter(115);
    QFont f = font();
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(fg);
    double tri = r.height() * 0.22;
    p.drawText(r.adjusted(4, 0, -(tri * 2 + 8), 0), Qt::AlignCenter, currentText());
    double cx = r.right() - tri - 6, cy = r.center().y();
    QPolygonF arrow;
    arrow << QPointF(cx - tri, cy - tri * 0.5) << QPointF(cx + tri, cy - tri * 0.5)
          << QPointF(cx, cy + tri * 0.7);
    p.setPen(Qt::NoPen);
    p.setBrush(fg);
    p.drawPolygon(arrow);
  }
};

// Fader with level meter(s), or a bare meter driven by an attached gain knob
// (input channels). Mono faders put the meter on one side (SetMeterOnRight).
class FaderStrip : public QWidget {
public:
  QSize m_designSize;
  std::function<void(int)> onChange;

  FaderStrip(QWidget *parent, int minVal, int maxVal, bool stereo = false,
             QSize size = QSize(45, 120))
      : QWidget(parent), m_designSize(size), m_min(minVal), m_max(maxVal),
        m_stereo(stereo) {
    if (m_designSize.width() <= 0 || m_designSize.height() <= 0)
      m_designSize = QSize(45, 120);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    Rescale(g_uiScale);
  }

  // Turns this strip into a meter; the knob edits the value instead.
  void AttachKnob(KnobControl *knob) {
    m_knob = knob;
    setFocusPolicy(Qt::NoFocus);
    knob->SetValue(m_val);
    knob->onChange = [this](int v) { setUserValue(v); };
  }

  void setVisible(bool visible) override {
    QWidget::setVisible(visible);
    if (m_knob)
      m_knob->setVisible(visible);
  }

  void SetValue(int val) {
    val = std::clamp(val, m_min, m_max);
    if (m_knob)
      m_knob->SetValue(val);
    if (m_val != val) {
      m_val = val;
      update();
    }
  }
  int GetValue() const { return m_val; }

  void SetMeterLevels(int left, int right) {
    m_leftVal = std::clamp(left, -960, 10);
    m_rightVal = std::clamp(right, -960, 10);
    m_leftPeak = std::max(m_leftPeak, m_leftVal);
    m_rightPeak = std::max(m_rightPeak, m_rightVal);
    update();
  }

  void SetMeterOnRight(bool onRight) {
    m_meterOnRight = onRight;
    update();
  }

  void Rescale(double scale) {
    setMinimumSize(m_designSize.width() * scale, m_designSize.height() * scale);
  }

protected:
  QSize sizeHint() const override { return minimumSize(); }

  void keyPressEvent(QKeyEvent *e) override {
    int k = e->key();
    if (k == Qt::Key_Up || k == Qt::Key_Right)
      setUserValue(m_val + 1);
    else if (k == Qt::Key_Down || k == Qt::Key_Left)
      setUserValue(m_val - 1);
    else
      QWidget::keyPressEvent(e);
  }

  void wheelEvent(QWheelEvent *e) override {
    if (!m_knob && e->angleDelta().y())
      setUserValue(m_val + (e->angleDelta().y() > 0 ? 1 : -1));
  }

  void focusInEvent(QFocusEvent *) override { update(); }
  void focusOutEvent(QFocusEvent *) override { update(); }

  // value readout row (faders only; inputs show the peak button instead)
  double top() const { return m_knob ? 6.0 : 18.0; }
  static constexpr double BOTTOM = 6.0;

  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(g_uiScale, g_uiScale);
    double dx = width() / g_uiScale, dy = height() / g_uiScale;
    double ySlot = top(), hSlot = dy - top() - BOTTOM;
    bool meterOnly = m_knob != nullptr;

    QFont small = font();
    small.setPointSizeF(6.5);
    small.setWeight(QFont::DemiBold);
    QFont top_ = font();
    top_.setPointSizeF(8);
    top_.setWeight(QFont::DemiBold);
    QFontMetricsF fm(small);

    // Positions: meter(s), scale labels and the fader track
    const double mw = meterOnly ? 9.0 : 6.0;
    double cx = dx / 2.0;
    double trackX = cx, meterL = -1, meterR = -1;
    enum { LABEL_LEFT, LABEL_RIGHT, LABEL_BOTH } labelSide;
    if (m_stereo) {
      meterL = meterOnly ? 4 : cx - 32;
      meterR = meterOnly ? dx - 4 - mw : cx + 32 - mw;
      labelSide = LABEL_BOTH;
    } else if (meterOnly) {
      meterL = 6;
      labelSide = LABEL_RIGHT;
    } else if (m_meterOnRight) {
      trackX = 9;
      meterR = dx - 5 - mw;
      labelSide = LABEL_RIGHT;
    } else {
      trackX = dx - 9;
      meterL = 5;
      labelSide = LABEL_LEFT;
    }

    // 1. Meters
    auto levelY = [&](int v) {
      double pct = std::clamp((double)(v + 960) / 970.0, 0.0, 1.0);
      return ySlot + hSlot - hSlot * pct;
    };
    auto drawMeter = [&](double x, int val, int peak) {
      p.setPen(Qt::NoPen);
      p.fillRect(QRectF(x, ySlot, mw, hSlot), COL_METER_BG);
      double y = levelY(val);
      if (ySlot + hSlot - y > 0) {
        p.fillRect(QRectF(x, y, mw, ySlot + hSlot - y), COL_GREEN);
        if (val > -120)
          p.fillRect(QRectF(x, y, mw, levelY(-120) - y), COL_YELLOW);
        if (val > -30)
          p.fillRect(QRectF(x, y, mw, levelY(-30) - y), COL_RED);
      }
      if (peak > -960) {
        double yp = levelY(peak);
        p.fillRect(QRectF(x, yp - 0.5, mw, 1.2), QColor(230, 230, 230));
      }
    };
    if (meterL >= 0)
      drawMeter(meterL, m_leftVal, m_leftPeak);
    if (meterR >= 0)
      drawMeter(meterR, m_stereo ? m_rightVal : m_leftVal,
                m_stereo ? m_rightPeak : m_leftPeak);

    // 2. Scale: meters show the -60..0 dBFS scale, faders their gain range
    std::vector<int> ticks;
    int lo = meterOnly ? -60 : m_min, hi = meterOnly ? 0 : m_max;
    for (int db : {0, -6, -12, -24, -36, -60})
      if (db >= lo && db <= hi)
        ticks.push_back(db);
    auto scaleY = [&](int db) {
      double pct = std::clamp((double)(db - lo) / (hi - lo), 0.0, 1.0);
      return ySlot + hSlot - hSlot * pct;
    };
    p.setFont(small);
    p.setPen(COL_SCALE);
    double lblLeftEdge, lblRightEdge; // where labels end / start
    if (meterOnly) {
      lblLeftEdge = (meterL >= 0 ? meterL + mw : 0) + 3;
      lblRightEdge = (meterR >= 0 ? meterR : dx) - 3;
    } else {
      lblLeftEdge = trackX + 8;
      lblRightEdge = trackX - 8;
    }
    for (int db : ticks) {
      double y = scaleY(db);
      QString lbl = meterOnly ? QString::number(db)
                              : (db <= m_min ? QString::fromUtf8("-∞")
                                             : dbText(db, m_min));
      double tw = fm.horizontalAdvance(lbl), th = fm.height();
      if (labelSide == LABEL_RIGHT || labelSide == LABEL_BOTH) {
        p.drawLine(QPointF(lblLeftEdge, y), QPointF(lblLeftEdge + 3, y));
        p.drawText(QRectF(lblLeftEdge + 5, y - th / 2, tw + 2, th),
                   Qt::AlignLeft | Qt::AlignVCenter, lbl);
      }
      if (labelSide == LABEL_LEFT || labelSide == LABEL_BOTH) {
        p.drawLine(QPointF(lblRightEdge - 3, y), QPointF(lblRightEdge, y));
        p.drawText(QRectF(lblRightEdge - 5 - tw - 2, y - th / 2, tw + 2, th),
                   Qt::AlignRight | Qt::AlignVCenter, lbl);
      }
    }

    // 3. Fader value readout
    if (meterOnly)
      return;
    p.setFont(top_);
    p.setPen(QColor(205, 205, 205));
    p.drawText(QRectF(trackX - 14, 0, 28, top() - 2), Qt::AlignCenter,
               dbText(m_val, m_min));

    // 4. Fader: filled track below the cap, then the cap
    double yVal = ValToY(m_val, hSlot, ySlot);
    p.fillRect(QRectF(trackX - 1.5, ySlot, 3, hSlot), QColor(36, 36, 36));
    p.setPen(Qt::NoPen);
    p.setBrush(COL_TRACK);
    p.drawRoundedRect(QRectF(trackX - 3, yVal, 6, ySlot + hSlot - yVal), 2, 2);
    double hh = 13.0, hw = 15.0;
    p.setBrush(COL_HANDLE);
    p.drawRect(QRectF(trackX - hw / 2, yVal - hh / 2, hw, hh));
    p.setPen(QPen(QColor(150, 150, 150), 1));
    p.drawLine(QPointF(trackX - hw / 2 + 2, yVal), QPointF(trackX + hw / 2 - 2, yVal));

    if (hasFocus()) {
      p.setPen(QPen(QColor(80, 120, 200), 1));
      p.setBrush(Qt::NoBrush);
      p.drawRect(QRectF(trackX - hw / 2 - 1.5, yVal - hh / 2 - 1.5, hw + 3, hh + 3));
    }
  }

  void mousePressEvent(QMouseEvent *e) override {
    if (m_knob || e->button() != Qt::LeftButton)
      return;
    setFocus();
    m_isDragging = true;
    double hSlot = height() / g_uiScale - top() - BOTTOM;
    m_dragStartY = e->position().y() / g_uiScale;
    // grab the cap where it is; clicking elsewhere jumps to that point
    double capY = ValToY(m_val, hSlot, top());
    if (std::abs(m_dragStartY - capY) > 8)
      setUserValue(YToVal(m_dragStartY, hSlot, top()));
    m_dragStartVal = m_val;
  }

  void mouseReleaseEvent(QMouseEvent *) override { m_isDragging = false; }

  void mouseMoveEvent(QMouseEvent *e) override {
    if (!m_isDragging)
      return;
    double hSlot = height() / g_uiScale - top() - BOTTOM;
    double dy = e->position().y() / g_uiScale - m_dragStartY;
    setUserValue(YToVal(ValToY(m_dragStartVal, hSlot, top()) + dy, hSlot, top()));
  }

  void mouseDoubleClickEvent(QMouseEvent *) override {
    if (!m_knob)
      setUserValue(0);
  }

private:
  int m_val = 0, m_min, m_max;
  bool m_stereo;
  int m_leftVal = -960, m_rightVal = -960, m_leftPeak = -960, m_rightPeak = -960;
  bool m_isDragging = false;
  int m_dragStartVal = 0;
  double m_dragStartY = 0;
  bool m_meterOnRight = false;
  KnobControl *m_knob = nullptr;

  double ValToY(int val, double hSlot, double ySlot) {
    double pct = std::clamp((double)(val - m_min) / (m_max - m_min), 0.0, 1.0);
    return ySlot + hSlot - hSlot * pct;
  }

  int YToVal(double y, double hSlot, double ySlot) {
    double pct = std::clamp((ySlot + hSlot - y) / hSlot, 0.0, 1.0);
    return m_min + std::round(pct * (m_max - m_min));
  }

  void setUserValue(int v) {
    v = std::clamp(v, m_min, m_max);
    if (m_knob)
      m_knob->SetValue(v);
    if (v == m_val)
      return;
    m_val = v;
    update();
    if (onChange)
      onChange(m_val);
  }
};

// ----------------------------------------------------------------------------
// Dialogs
// ----------------------------------------------------------------------------

class SettingsDialog : public QDialog {
public:
  SettingsDialog(QWidget *parent, uint16_t pid, ToppingHID *hid,
                 std::function<void()> onReset)
      : QDialog(parent), m_hid(hid) {
    setWindowTitle("Settings");
    auto *mainSizer = vbox(this, 8, 0);
    auto *cols = hbox(nullptr, 0, 16);

    // Left Column: System settings
    auto *leftCol = vbox(nullptr, 5, 8);
    leftCol->addWidget(mkLabel(this, "System settings", 9.5, true, QColor(220, 220, 220)));
    leftCol->addWidget(mkLabel(this, "Workspace storage:", 8.5, false, QColor(220, 220, 220)));
    auto *dirRow = hbox(nullptr, 0, 5);
    dirRow->addWidget(mkLabel(this, "~/.config/topping...", 8.5, false, QColor(140, 140, 140)), 1);
    auto *btnChangeDir = new QPushButton("Change", this);
    btnChangeDir->setFixedSize(55, 22);
    dirRow->addWidget(btnChangeDir);
    leftCol->addLayout(dirRow);
    cbSaveWorkspace = new QCheckBox("Auto save workspace", this);
    cbSaveWorkspace->setChecked(m_hid->settings.contains(0x9001)
                                    ? (m_hid->settings[0x9001] != 0)
                                    : true);
    leftCol->addWidget(cbSaveWorkspace);
    leftCol->addStretch(1);
    cols->addLayout(leftCol, 1);

    auto *divLine = new QFrame(this);
    divLine->setFrameShape(QFrame::VLine);
    divLine->setFrameShadow(QFrame::Sunken);
    cols->addWidget(divLine);

    // Right Column: Device settings
    auto *rightCol = vbox(nullptr, 5, 8);
    rightCol->addWidget(mkLabel(this, "Device settings", 9.5, true, QColor(220, 220, 220)));
    auto *brightRow = hbox(nullptr, 0, 5);
    brightRow->addWidget(mkLabel(this, "Brightness:", 8.5, false, QColor(220, 220, 220)));
    choiceBrightness = new QComboBox(this);
    choiceBrightness->addItems({"Low", "Normal", "High"});
    int brightVal = m_hid->settings.contains(0x1104) ? m_hid->settings[0x1104] : 1;
    choiceBrightness->setCurrentIndex(std::clamp(brightVal, 0, 2));
    brightRow->addWidget(choiceBrightness, 1);
    rightCol->addLayout(brightRow);

    cbAutoStandby = new QCheckBox("Automatic standby", this);
    uint16_t standbyReg = (pid == 0x8754) ? 0x3901 : 0x1101;
    cbAutoStandby->setChecked(
        m_hid->settings.contains(standbyReg) ? m_hid->settings[standbyReg] : 1);
    rightCol->addWidget(cbAutoStandby);

    bool isOTG = (pid == 0x8755 || pid == 0x8756);
    if (isOTG || pid == 0x8754) {
      uint16_t otgReg = (pid == 0x8754) ? 0x3a01 : 0x1103;
      cbOTGMode = new QCheckBox("Mobile applications", this);
      cbOTGMode->setChecked(m_hid->settings.contains(otgReg) ? m_hid->settings[otgReg] : 0);
      rightCol->addWidget(cbOTGMode);
    }
    rightCol->addStretch(1);
    cols->addLayout(rightCol, 1);
    mainSizer->addLayout(cols);

    auto *okRow = hbox(nullptr, 8, 8);
    okRow->addStretch(1);
    auto *btnReset = new QPushButton("Reset Defaults", this);
    auto *btnOK = new QPushButton("OK", this);
    okRow->addWidget(btnReset);
    okRow->addWidget(btnOK);
    mainSizer->addLayout(okRow);

    connect(btnOK, &QPushButton::clicked, this, &QDialog::accept);
    connect(choiceBrightness, &QComboBox::activated, this, [this] { OnDeviceChange(); });
    connect(cbAutoStandby, &QCheckBox::clicked, this, [this] { OnDeviceChange(); });
    if (cbOTGMode)
      connect(cbOTGMode, &QCheckBox::clicked, this, [this] { OnDeviceChange(); });
    connect(cbSaveWorkspace, &QCheckBox::clicked, this, [this](bool on) {
      m_hid->settings[0x9001] = on ? 1 : 0;
    });
    connect(btnReset, &QPushButton::clicked, this, [this, onReset] {
      m_hid->initializeSettingsWithDefaults(true);
      cbSaveWorkspace->setChecked(true);
      m_hid->settings[0x9001] = 1;

      choiceBrightness->setCurrentIndex(1);
      m_hid->setDeviceSetting(0x11, 0x04, 1);
      m_hid->settings[0x1104] = 1;

      int standbyReg = (m_hid->pid == 0x8754) ? 0x3901 : 0x1101;
      m_hid->setDeviceSetting(standbyReg >> 8, standbyReg & 0xff, 1);
      m_hid->settings[standbyReg] = 1;
      cbAutoStandby->setChecked(true);

      if (cbOTGMode) {
        int otgReg = (m_hid->pid == 0x8754) ? 0x3a01 : 0x1103;
        m_hid->setDeviceSetting(otgReg >> 8, otgReg & 0xff, 0);
        m_hid->settings[otgReg] = 0;
        cbOTGMode->setChecked(false);
      }

      onReset();
      QMessageBox::information(this, "Reset Success",
                               "Settings reset to defaults and applied to device!");
    });
  }

private:
  ToppingHID *m_hid;
  QCheckBox *cbSaveWorkspace;
  QCheckBox *cbAutoStandby;
  QCheckBox *cbOTGMode = nullptr;
  QComboBox *choiceBrightness;

  void OnDeviceChange() {
    if (NULL == m_hid->getHandle())
      return;

    int sel = choiceBrightness->currentIndex();
    if (sel >= 0) {
      m_hid->setDeviceSetting(0x11, 0x04, sel);
      m_hid->settings[0x1104] = sel;
    }

    int val32 = cbAutoStandby->isChecked() ? 1 : 0;
    if (m_hid->pid == 0x8754) {
      m_hid->setDeviceSetting(0x39, 0x01, val32);
      m_hid->settings[0x3901] = val32;
    } else {
      m_hid->setDeviceSetting(0x11, 0x01, val32);
      m_hid->settings[0x1101] = val32;
    }

    if (cbOTGMode) {
      int otgVal32 = cbOTGMode->isChecked() ? 1 : 0;
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

class DownloadDialog : public QDialog {
public:
  QRadioButton *rbOption1;
  QRadioButton *rbOption3;

  DownloadDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Download");
    resize(380, 150);
    auto *mainSizer = vbox(this, 10, 6);
    mainSizer->addSpacing(10);
    rbOption1 = new QRadioButton("Save current workspace and download to device", this);
    rbOption3 = new QRadioButton("Download to device only", this);
    rbOption1->setChecked(true);
    mainSizer->addWidget(rbOption1);
    mainSizer->addWidget(rbOption3);
    auto *btnRow = hbox(nullptr, 0, 10);
    btnRow->addStretch(1);
    auto *btnCancel = new QPushButton("Cancel", this);
    auto *btnOK = new QPushButton("OK", this);
    btnRow->addWidget(btnCancel);
    btnRow->addWidget(btnOK);
    mainSizer->addLayout(btnRow);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(btnOK, &QPushButton::clicked, this, &QDialog::accept);
  }
};

// ----------------------------------------------------------------------------
// Panels
// ----------------------------------------------------------------------------

static const QStringList OutputSels = {
    "IN1",         "IN2",         "IN3",         "IN4",   "IN1+2",
    "IN3+4",       "Playback1+2", "Playback3+4", "Playback5+6",
    "Playback7+8", "MIX A",       "MIX B",       "MIX C", "MIX D",
};

// Channel strip: dark rounded column with a grey title box on top
static QWidget *mkStrip(QWidget *parent, QLabel *&title, const QString &name,
                        QVBoxLayout *&layout) {
  auto *strip = new QWidget(parent);
  strip->setObjectName("stripPanel");
  strip->setAttribute(Qt::WA_StyledBackground);
  layout = vbox(strip, 0, 3);
  layout->setContentsMargins(0, 0, 0, 6);
  title = mkTitleBox(strip, name);
  layout->addWidget(title);
  return strip;
}

// value readouts now live inside the faders; kept so callers can still set them
static QLabel *mkHiddenLabel(QWidget *parent) {
  auto *l = new QLabel(parent);
  l->hide();
  return l;
}

class PanelInputs : public QWidget {
public:
  constexpr static int32_t N_INPUTS = 4;
  constexpr static int32_t LEVEL_MIN = -960;
  constexpr static int32_t LEVEL_MAX = 10;
  constexpr static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  constexpr static int32_t minGain = -60;
  constexpr static int32_t maxGain = 30;

  QLabel *lbTitleI[N_INPUTS] = {};
  CustomButton *lbPeaksI[N_INPUTS] = {};
  QLabel *lbGainVal[N_INPUTS] = {};

  FaderStrip *slGainI[N_INPUTS] = {};
  FaderStrip *slGainICombined = nullptr;
  CustomButton *btnLink = nullptr;

  CustomButton *cb48V[N_INPUTS] = {};
  CustomButton *cbMon[N_INPUTS] = {};
  CustomButton *cbInst[N_INPUTS] = {};
  CustomButton *cbSolo[N_INPUTS] = {};
  CustomButton *cbMute[N_INPUTS] = {};
  CustomButton *cbPhase[N_INPUTS] = {};

  // Split buttons for channel 3+4
  CustomButton *cbSoloL = nullptr, *cbSoloR = nullptr;
  CustomButton *cbMuteL = nullptr, *cbMuteR = nullptr;
  CustomButton *cbPhaseL = nullptr, *cbPhaseR = nullptr;

  QWidget *combBtns = nullptr;
  QWidget *splitBtns = nullptr;

  int32_t PeaksI[N_INPUTS];
  int32_t inLevelVal[N_INPUTS];

  PanelInputs(QWidget *parent, uint16_t pid = 0x8754) : QWidget(parent) {
    setBg(this, COL_SECTION);
    std::fill(std::begin(PeaksI), std::end(PeaksI), LEVEL_MIN);
    std::fill(std::begin(inLevelVal), std::end(inLevelVal), LEVEL_MIN);

    auto *mainSizer = vbox(this);
    auto *title = titleRow(this, "Input");
    title->addStretch(1);
    mainSizer->addLayout(title);

    auto *colsSizer = hbox(nullptr, 8, 8);
    colsSizer->setContentsMargins(8, 0, 8, 8);
    int numCols = (pid == 0x8754) ? 3 : ((pid == 0x8752 || pid == 0x8756) ? 2 : 1);
    bool isE1x2 = pid == 0x8755;

    for (int col = 0; col < numCols; ++col) {
      QString colName = col == 0 ? (isE1x2 ? "Analog IN" : "IN 1")
                                 : (col == 1 ? "IN 2" : "IN 3+4");
      QVBoxLayout *cs;
      QWidget *colPanel = mkStrip(this, lbTitleI[col], colName, cs);
      colPanel->setObjectName("colPanel");
      lbGainVal[col] = mkHiddenLabel(colPanel);

      // Peak readout (click to reset)
      lbPeaksI[col] = new CustomButton(colPanel, "-60.0", false, QSize(45, 16),
                                       COL_BLUE, ICON_NONE, BTN_TEXT);
      cs->addSpacing(4);
      cs->addWidget(lbPeaksI[col], 0, Qt::AlignHCenter);

      auto *midSizer = hbox(nullptr, 0, 4);
      midSizer->setContentsMargins(6, 0, 6, 0);
      QWidget *btnColW = new QWidget(colPanel);
      auto *btnCol = vbox(btnColW, 0, 3);
      auto addC = [](QVBoxLayout *l, QWidget *w) { l->addWidget(w, 0, Qt::AlignHCenter); };
      auto *knobRow = hbox(nullptr, 0, 4);

      if (col < 2) {
        slGainI[col] = new FaderStrip(colPanel, minGain, maxGain, false, QSize(44, 150));
        midSizer->addWidget(slGainI[col], 1);

        cbMon[col] = new CustomButton(btnColW, "MON", true, QSize(36, 17), COL_GREEN);
        cb48V[col] = new CustomButton(btnColW, "48V", true, QSize(36, 17), COL_RED);
        cbInst[col] = new CustomButton(btnColW, "INST", true, QSize(36, 17), COL_GREEN);
        cbSolo[col] = new CustomButton(btnColW, "SOLO", true, QSize(36, 17), COL_YELLOW);
        cbMute[col] = new CustomButton(btnColW, "MUTE", true, QSize(36, 17), COL_RED);
        cbPhase[col] = new CustomButton(btnColW, QString::fromUtf8("ø"), true, QSize(36, 17), COL_GREEN);
        for (auto *b : {cbMon[col], cb48V[col], cbInst[col]})
          addC(btnCol, b);
        btnCol->addSpacing(12);
        for (auto *b : {cbSolo[col], cbMute[col], cbPhase[col]})
          addC(btnCol, b);

        auto *knob = new KnobControl(colPanel, "Gain", minGain, maxGain, 0, QSize(64, 78), true);
        slGainI[col]->AttachKnob(knob);
        knobRow->addWidget(knob);
      } else {
        // Stereo channel IN 3+4
        slGainI[2] = new FaderStrip(colPanel, minGain, maxGain, false, QSize(26, 150));
        slGainI[3] = new FaderStrip(colPanel, minGain, maxGain, false, QSize(26, 150));
        slGainICombined = new FaderStrip(colPanel, minGain, maxGain, true, QSize(44, 150));
        midSizer->addWidget(slGainI[2], 1);
        midSizer->addWidget(slGainI[3], 1);
        midSizer->addWidget(slGainICombined, 1);

        btnLink = new CustomButton(btnColW, "Link", true, QSize(36, 16), COL_BLUE, ICON_NONE, BTN_TEXT);
        btnLink->SetValue(true);
        addC(btnCol, btnLink);
        btnCol->addSpacing(8);

        combBtns = new QWidget(btnColW);
        auto *comb = vbox(combBtns, 0, 3);
        cbSolo[col] = new CustomButton(combBtns, "SOLO", true, QSize(36, 17), COL_YELLOW);
        cbMute[col] = new CustomButton(combBtns, "MUTE", true, QSize(36, 17), COL_RED);
        cbPhase[col] = new CustomButton(combBtns, QString::fromUtf8("ø"), true, QSize(36, 17), COL_GREEN);
        for (auto *b : {cbSolo[col], cbMute[col], cbPhase[col]})
          addC(comb, b);
        btnCol->addWidget(combBtns);

        splitBtns = new QWidget(btnColW);
        auto *split = hbox(splitBtns, 0, 2);
        auto *lc = vbox(nullptr, 0, 3), *rc = vbox(nullptr, 0, 3);
        cbSoloL = new CustomButton(splitBtns, "S", true, QSize(17, 17), COL_YELLOW);
        cbMuteL = new CustomButton(splitBtns, "M", true, QSize(17, 17), COL_RED);
        cbPhaseL = new CustomButton(splitBtns, QString::fromUtf8("ø"), true, QSize(17, 17), COL_GREEN);
        cbSoloR = new CustomButton(splitBtns, "S", true, QSize(17, 17), COL_YELLOW);
        cbMuteR = new CustomButton(splitBtns, "M", true, QSize(17, 17), COL_RED);
        cbPhaseR = new CustomButton(splitBtns, QString::fromUtf8("ø"), true, QSize(17, 17), COL_GREEN);
        for (auto *b : {cbSoloL, cbMuteL, cbPhaseL})
          lc->addWidget(b);
        for (auto *b : {cbSoloR, cbMuteR, cbPhaseR})
          rc->addWidget(b);
        split->addLayout(lc, 1);
        split->addLayout(rc, 1);
        btnCol->addWidget(splitBtns);

        auto *knobC = new KnobControl(colPanel, "Gain", minGain, maxGain, 0, QSize(64, 78), true);
        auto *knobL = new KnobControl(colPanel, "Gain", minGain, maxGain, 0, QSize(46, 60), true);
        auto *knobR = new KnobControl(colPanel, "Gain", minGain, maxGain, 0, QSize(46, 60), true);
        slGainICombined->AttachKnob(knobC);
        slGainI[2]->AttachKnob(knobL);
        slGainI[3]->AttachKnob(knobR);
        knobRow->addWidget(knobL);
        knobRow->addWidget(knobR);
        knobRow->addWidget(knobC);
        slGainI[2]->setVisible(false);
        slGainI[3]->setVisible(false);
        splitBtns->hide();
      }
      midSizer->addWidget(btnColW, 0, Qt::AlignVCenter);
      cs->addLayout(midSizer, 1);
      cs->addSpacing(6);
      auto *knobWrap = hbox();
      knobWrap->addStretch(1);
      knobWrap->addLayout(knobRow);
      knobWrap->addStretch(1);
      cs->addLayout(knobWrap);

      colPanel->setMinimumWidth(120);
      colsSizer->addWidget(colPanel);
    }
    colsSizer->addStretch(1);
    mainSizer->addLayout(colsSizer, 1);
  }
};

class PanelMixers : public QWidget {
public:
  constexpr static int32_t N_MIXERS = 4;
  constexpr static int32_t N_MIX_SRCS = 12;
  constexpr static int32_t N_STRIPS = N_MIX_SRCS / 2;
  constexpr static int32_t LEVEL_MIN = -960;
  constexpr static int32_t LEVEL_MAX = 10;
  constexpr static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  constexpr static int32_t minGain = -60;
  constexpr static int32_t maxGain = 6;

  CustomButton *tabButtons[N_MIXERS] = {};

  QLabel *lbTitle[N_STRIPS] = {};
  QLabel *lbVolVal[N_STRIPS] = {};
  KnobControl *slPan[N_MIX_SRCS] = {};

  FaderStrip *slVol[N_STRIPS] = {};
  FaderStrip *slVolL[N_STRIPS] = {};
  FaderStrip *slVolR[N_STRIPS] = {};
  CustomButton *btnLink[N_STRIPS] = {};

  // Combined Buttons
  CustomButton *ckMute[N_STRIPS] = {};
  CustomButton *ckSolo[N_STRIPS] = {};
  CustomButton *ckPhaseCombined[N_STRIPS] = {};

  // Split Buttons
  CustomButton *ckMuteL[N_STRIPS] = {};
  CustomButton *ckMuteR[N_STRIPS] = {};
  CustomButton *ckSoloL[N_STRIPS] = {};
  CustomButton *ckSoloR[N_STRIPS] = {};
  CustomButton *ckPhase[N_MIX_SRCS] = {};

  QWidget *combBtns[N_STRIPS] = {};
  QWidget *splitBtns[N_STRIPS] = {};

  int32_t mixLevelVal[N_MIX_SRCS];

  PanelMixers(QWidget *parent) : QWidget(parent) {
    setBg(this, COL_SECTION);
    std::fill(std::begin(mixLevelVal), std::end(mixLevelVal), LEVEL_MIN);

    auto *mainSizer = vbox(this);
    auto *title = titleRow(this, "Mixer");
    title->addSpacing(14);
    const QColor tabColors[4] = {QColor(140, 200, 120), QColor(40, 120, 220),
                                 QColor(200, 190, 60), QColor(220, 150, 30)};
    const char *mixNames[4] = {"Mix A", "Mix B", "Mix C", "Mix D"};
    for (int i = 0; i < N_MIXERS; ++i) {
      tabButtons[i] = new CustomButton(this, mixNames[i], true, QSize(64, 22),
                                       tabColors[i], ICON_NONE, BTN_TAB);
      title->addWidget(tabButtons[i], 0, Qt::AlignVCenter);
      title->addSpacing(10);
    }
    tabButtons[0]->SetValue(true);
    title->addStretch(1);
    mainSizer->addLayout(title);

    const char *Titles[N_STRIPS] = {"IN1+2",        "Disconnected", "Playback 1/2",
                                    "Playback 3/4", "Playback 5/6", "Playback 7/8"};
    auto *stripsSizer = hbox(nullptr, 0, 8);
    stripsSizer->setContentsMargins(8, 0, 8, 8);
    for (int i = 0; i < N_STRIPS; ++i) {
      QVBoxLayout *ss;
      QWidget *sp = mkStrip(this, lbTitle[i], Titles[i], ss);

      int l = i * 2, r = l + 1;
      auto *panRow = hbox(nullptr, 0, 0);
      slPan[l] = new KnobControl(sp, "Pan L", -100, 100, -100, QSize(34, 34));
      slPan[r] = new KnobControl(sp, "Pan R", -100, 100, 100, QSize(34, 34));
      panRow->addWidget(slPan[l], 1, Qt::AlignHCenter);
      panRow->addWidget(slPan[r], 1, Qt::AlignHCenter);
      ss->addSpacing(4);
      ss->addLayout(panRow);

      btnLink[i] = new CustomButton(sp, "Link", true, QSize(40, 16), COL_BLUE, ICON_NONE, BTN_TEXT);
      btnLink[i]->SetValue(i != 0);
      ss->addWidget(btnLink[i], 0, Qt::AlignHCenter);

      lbVolVal[i] = mkHiddenLabel(sp);

      auto *faderSizer = hbox();
      faderSizer->setContentsMargins(2, 0, 2, 2);
      slVol[i] = new FaderStrip(sp, minGain, maxGain, true, QSize(80, 150));
      slVolL[i] = new FaderStrip(sp, minGain, maxGain, false, QSize(40, 150));
      slVolL[i]->SetMeterOnRight(true);
      slVolR[i] = new FaderStrip(sp, minGain, maxGain, false, QSize(40, 150));
      faderSizer->addWidget(slVolL[i], 1);
      faderSizer->addWidget(slVolR[i], 1);
      faderSizer->addWidget(slVol[i], 1);
      if (i == 0) {
        slVol[i]->hide();
      } else {
        slVolL[i]->hide();
        slVolR[i]->hide();
      }
      ss->addLayout(faderSizer, 1);

      combBtns[i] = new QWidget(sp);
      auto *comb = vbox(combBtns[i], 0, 3);
      ckPhaseCombined[i] = new CustomButton(combBtns[i], QString::fromUtf8("ø"), true, QSize(46, 18), COL_GREEN);
      ckSolo[i] = new CustomButton(combBtns[i], "SOLO", true, QSize(46, 18), COL_YELLOW);
      ckMute[i] = new CustomButton(combBtns[i], "MUTE", true, QSize(46, 18), COL_RED);
      for (auto *b : {ckPhaseCombined[i], ckSolo[i], ckMute[i]})
        comb->addWidget(b, 0, Qt::AlignHCenter);
      combBtns[i]->setVisible(i != 0);
      ss->addWidget(combBtns[i]);

      splitBtns[i] = new QWidget(sp);
      auto *split = hbox(splitBtns[i], 0, 0);
      auto *lc = vbox(nullptr, 0, 3), *rc = vbox(nullptr, 0, 3);
      ckPhase[l] = new CustomButton(splitBtns[i], QString::fromUtf8("ø"), true, QSize(40, 18), COL_GREEN);
      ckSoloL[i] = new CustomButton(splitBtns[i], "SOLO", true, QSize(40, 18), COL_YELLOW);
      ckMuteL[i] = new CustomButton(splitBtns[i], "MUTE", true, QSize(40, 18), COL_RED);
      ckPhase[r] = new CustomButton(splitBtns[i], QString::fromUtf8("ø"), true, QSize(40, 18), COL_GREEN);
      ckSoloR[i] = new CustomButton(splitBtns[i], "SOLO", true, QSize(40, 18), COL_YELLOW);
      ckMuteR[i] = new CustomButton(splitBtns[i], "MUTE", true, QSize(40, 18), COL_RED);
      for (auto *b : {ckPhase[l], ckSoloL[i], ckMuteL[i]})
        lc->addWidget(b, 0, Qt::AlignHCenter);
      for (auto *b : {ckPhase[r], ckSoloR[i], ckMuteR[i]})
        rc->addWidget(b, 0, Qt::AlignHCenter);
      split->addLayout(lc, 1);
      split->addLayout(rc, 1);
      splitBtns[i]->setVisible(i == 0);
      ss->addWidget(splitBtns[i]);

      stripsSizer->addWidget(sp, 1);
    }
    mainSizer->addLayout(stripsSizer, 1);
  }
};

// Shared layout of Loopback and Output strips: title, source combo, link,
// linked/split faders.
struct BusStrip {
  QLabel *title;
  QComboBox *select;
  CustomButton *link;
  QLabel *volVal;
  FaderStrip *fader, *faderL, *faderR;
  QVBoxLayout *layout;
  QWidget *panel;
};

static BusStrip mkBusStrip(QWidget *parent, const QString &name, int sel,
                           int minGain, int maxGain, int faderH) {
  BusStrip s;
  s.panel = mkStrip(parent, s.title, name, s.layout);
  s.select = new FlatCombo(s.panel, "flatCombo");
  s.select->addItems(OutputSels);
  s.select->setCurrentIndex(sel);
  s.layout->addWidget(s.select);
  s.link = new CustomButton(s.panel, "Link", true, QSize(40, 16), COL_BLUE, ICON_NONE, BTN_TEXT);
  s.link->SetValue(true);
  s.layout->addWidget(s.link, 0, Qt::AlignHCenter);
  s.volVal = mkHiddenLabel(s.panel);
  auto *faderSizer = hbox();
  faderSizer->setContentsMargins(4, 0, 4, 4);
  s.fader = new FaderStrip(s.panel, minGain, maxGain, true, QSize(80, faderH));
  s.faderL = new FaderStrip(s.panel, minGain, maxGain, false, QSize(40, faderH));
  s.faderL->SetMeterOnRight(true);
  s.faderR = new FaderStrip(s.panel, minGain, maxGain, false, QSize(40, faderH));
  faderSizer->addWidget(s.faderL, 1);
  faderSizer->addWidget(s.faderR, 1);
  faderSizer->addWidget(s.fader, 1);
  s.faderL->hide();
  s.faderR->hide();
  s.layout->addLayout(faderSizer, 1);
  s.panel->setMinimumWidth(120);
  return s;
}

// combined MUTE + hidden split L/R MUTE pair
static void mkMuteButtons(QWidget *parent, QVBoxLayout *layout,
                          CustomButton *&mute, CustomButton *&muteL,
                          CustomButton *&muteR, QWidget *&comb,
                          QWidget *&split) {
  comb = new QWidget(parent);
  auto *c = vbox(comb);
  mute = new CustomButton(comb, "MUTE", true, QSize(46, 20), COL_RED);
  c->addWidget(mute, 0, Qt::AlignHCenter);
  layout->addWidget(comb);
  split = new QWidget(parent);
  auto *s = hbox(split, 0, 0);
  muteL = new CustomButton(split, "MUTE", true, QSize(40, 20), COL_RED);
  muteR = new CustomButton(split, "MUTE", true, QSize(40, 20), COL_RED);
  s->addWidget(muteL, 1, Qt::AlignHCenter);
  s->addWidget(muteR, 1, Qt::AlignHCenter);
  layout->addWidget(split);
  split->hide();
}

class PanelLoopbacks : public QWidget {
public:
  constexpr static int32_t N_LOOPBACKS = 3;
  constexpr static int32_t LEVEL_MIN = -960;
  constexpr static int32_t LEVEL_MAX = 10;
  constexpr static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  constexpr static int32_t minGain = -60;
  constexpr static int32_t maxGain = 20;

  QLabel *lbTitle[N_LOOPBACKS] = {};
  QComboBox *cbSelect[N_LOOPBACKS] = {};
  QLabel *lbLoopVolVal[N_LOOPBACKS] = {};

  FaderStrip *slOutput[N_LOOPBACKS] = {};
  FaderStrip *slOutputL[N_LOOPBACKS] = {};
  FaderStrip *slOutputR[N_LOOPBACKS] = {};
  CustomButton *btnLink[N_LOOPBACKS] = {};

  CustomButton *ckMute[N_LOOPBACKS] = {};
  CustomButton *ckMuteL[N_LOOPBACKS] = {};
  CustomButton *ckMuteR[N_LOOPBACKS] = {};

  QWidget *combBtns[N_LOOPBACKS] = {};
  QWidget *splitBtns[N_LOOPBACKS] = {};

  int32_t loopLevelVal[N_LOOPBACKS * 2];

  PanelLoopbacks(QWidget *parent) : QWidget(parent) {
    setBg(this, COL_SECTION);
    std::fill(std::begin(loopLevelVal), std::end(loopLevelVal), LEVEL_MIN);

    auto *mainSizer = vbox(this);
    auto *title = titleRow(this, "Loopback");
    title->addStretch(1);
    mainSizer->addLayout(title);

    auto *stripsSizer = hbox(nullptr, 0, 8);
    stripsSizer->setContentsMargins(8, 0, 8, 8);
    for (int i = 0; i < N_LOOPBACKS; ++i) {
      BusStrip s = mkBusStrip(this, qs(std::format("Loopback {}+{}", i * 2 + 1, i * 2 + 2)),
                              10 + i, minGain, maxGain, 150);
      lbTitle[i] = s.title;
      cbSelect[i] = s.select;
      btnLink[i] = s.link;
      lbLoopVolVal[i] = s.volVal;
      slOutput[i] = s.fader;
      slOutputL[i] = s.faderL;
      slOutputR[i] = s.faderR;
      mkMuteButtons(s.panel, s.layout, ckMute[i], ckMuteL[i], ckMuteR[i],
                    combBtns[i], splitBtns[i]);
      stripsSizer->addWidget(s.panel, 1);
    }
    mainSizer->addLayout(stripsSizer, 1);
  }
};

class PanelOutputs : public QWidget {
public:
  constexpr static int32_t N_OUTPUTS = 3;
  constexpr static int32_t LEVEL_MIN = -960;
  constexpr static int32_t LEVEL_MAX = 10;
  constexpr static int32_t LEVEL_RANGE = LEVEL_MAX - LEVEL_MIN;
  constexpr static int32_t minGain = -60;
  constexpr static int32_t maxGain = 20;

  QLabel *lbTitle[N_OUTPUTS] = {};
  QComboBox *cbSelect[N_OUTPUTS] = {};
  QLabel *lbOutVolVal[N_OUTPUTS] = {};

  FaderStrip *slOutput[N_OUTPUTS] = {};
  FaderStrip *slOutputL[N_OUTPUTS] = {};
  FaderStrip *slOutputR[N_OUTPUTS] = {};
  CustomButton *btnLink[N_OUTPUTS] = {};

  CustomButton *ckMute[N_OUTPUTS] = {};
  CustomButton *ckMuteL[N_OUTPUTS] = {};
  CustomButton *ckMuteR[N_OUTPUTS] = {};

  QWidget *combBtns[N_OUTPUTS] = {};
  QWidget *splitBtns[N_OUTPUTS] = {};

  CustomButton *btnTRS[2] = {};
  CustomButton *btnAUX[2] = {};
  CustomButton *btnPhoneIcon[2] = {};

  KnobControl *mixKnob;
  CustomButton *btnPhoneGain;
  QComboBox *choicePhoneOut;
  QLabel *lblMixVal;

  int32_t outLevelVal[N_OUTPUTS * 2];
  int m_phoneMix[2] = {0, 0};
  bool m_phoneGain[2] = {false, false};

  void updatePhoneMixLabel(int val) {
    int inputPct = 50 - val / 2;
    int playPct = 50 + val / 2;
    lblMixVal->setText(qs(std::format("Level: {}/{}", inputPct, playPct)));
  }

  PanelOutputs(QWidget *parent, uint16_t pid = 0x8754) : QWidget(parent) {
    setBg(this, COL_SECTION);
    std::fill(std::begin(outLevelVal), std::end(outLevelVal), LEVEL_MIN);

    auto *mainSizer = vbox(this);
    auto *title = titleRow(this, "Output");
    title->addStretch(1);
    mainSizer->addLayout(title);

    auto *stripsSizer = hbox(nullptr, 0, 8);
    stripsSizer->setContentsMargins(8, 0, 8, 8);
    bool isOTG = (pid == 0x8755 || pid == 0x8756);
    int numStrips = (pid == 0x8752) ? 1 : (isOTG ? 2 : 3);

    for (int i = 0; i < numStrips; ++i) {
      QString name = i == 0 ? "Output 1+2"
                     : i == 1 ? (isOTG ? "Mobile OUT" : "Disconnected")
                              : "S/PDIF OUT";
      BusStrip s = mkBusStrip(this, name, 6, minGain, maxGain, 150);
      lbTitle[i] = s.title;
      cbSelect[i] = s.select;
      btnLink[i] = s.link;
      lbOutVolVal[i] = s.volVal;
      slOutput[i] = s.fader;
      slOutputL[i] = s.faderL;
      slOutputR[i] = s.faderR;

      if (i == 0) {
        auto *btnRow = hbox(nullptr, 0, 6);
        btnRow->addStretch(1);
        btnPhoneIcon[0] = new CustomButton(s.panel, "", true, QSize(30, 20), COL_BLUE, ICON_HEADPHONE);
        btnPhoneIcon[0]->SetValue(true);
        btnTRS[0] = new CustomButton(s.panel, "", true, QSize(30, 20), COL_BLUE, ICON_JACK);
        btnTRS[0]->SetValue(true);
        btnRow->addWidget(btnPhoneIcon[0]);
        btnRow->addWidget(btnTRS[0]);
        if (pid == 0x8754 || pid == 0x8756) {
          btnAUX[0] = new CustomButton(s.panel, "AUX", true, QSize(30, 20), COL_BLUE);
          btnAUX[0]->SetValue(true);
          btnRow->addWidget(btnAUX[0]);
        }
        btnRow->addStretch(1);
        s.layout->addLayout(btnRow);
      } else {
        mkMuteButtons(s.panel, s.layout, ckMute[i], ckMuteL[i], ckMuteR[i],
                      combBtns[i], splitBtns[i]);
      }
      stripsSizer->addWidget(s.panel);
    }
    stripsSizer->addStretch(1);

    QLabel *monTitle;
    QVBoxLayout *mm;
    QWidget *monMixPanel = mkStrip(this, monTitle, "Monitor Mix", mm);
    monMixPanel->setObjectName("monMixPanel");
    mm->addSpacing(10);
    mixKnob = new KnobControl(monMixPanel, "Mix", -100, 100, 0, QSize(70, 70));
    mm->addWidget(mixKnob, 0, Qt::AlignHCenter);
    auto *lblRow = hbox(nullptr, 0, 10);
    lblRow->addStretch(1);
    lblRow->addWidget(mkLabel(monMixPanel, "Input", 8.5, true, COL_TITLE));
    lblRow->addWidget(mkLabel(monMixPanel, "Playback", 8.5, true, COL_TITLE));
    lblRow->addStretch(1);
    mm->addLayout(lblRow);
    mm->addSpacing(8);
    choicePhoneOut = new FlatCombo(monMixPanel, "boxCombo");
    choicePhoneOut->addItems({"Phone 1", "Phone 2"});
    mm->addWidget(choicePhoneOut);
    mm->addStretch(1);
    lblMixVal = mkLabel(monMixPanel, "Level: 0dBu", 8.5, true, COL_TITLE, Qt::AlignCenter);
    mm->addWidget(lblMixVal);
    mm->addSpacing(10);
    btnPhoneGain = new CustomButton(monMixPanel, "Gain", true, QSize(56, 22), COL_BLUE);
    mm->addWidget(btnPhoneGain, 0, Qt::AlignHCenter);
    mm->addSpacing(10);
    monMixPanel->setFixedWidth(120);
    mixKnob->setEnabled(pid == 0x8754);
    stripsSizer->addWidget(monMixPanel);

    mainSizer->addLayout(stripsSizer, 1);

    connect(choicePhoneOut, &QComboBox::activated, this, [this](int idx) {
      if (idx < 0)
        return;
      mixKnob->SetValue(m_phoneMix[idx]);
      updatePhoneMixLabel(m_phoneMix[idx]);
      btnPhoneGain->SetValue(m_phoneGain[idx]);
    });
  }
};

void ScaleUIElements(QWidget *win, double scale) {
  if (!win)
    return;
  double fontScale = fontScaleFor(scale);

  if (auto *btn = dynamic_cast<CustomButton *>(win)) {
    btn->Rescale(scale);
  } else if (auto *knob = dynamic_cast<KnobControl *>(win)) {
    knob->Rescale(scale);
  } else if (auto *strip = dynamic_cast<FaderStrip *>(win)) {
    strip->Rescale(scale);
  } else if (auto *txt = qobject_cast<QLabel *>(win)) {
    double pt = txt->property("basePt").toDouble();
    if (pt > 0) {
      QFont f = txt->font();
      f.setPointSizeF(pt * fontScale);
      txt->setFont(f);
    }
  } else if (auto *cb = qobject_cast<QComboBox *>(win)) {
    QFont f = cb->font();
    f.setPointSizeF(8.5 * fontScale);
    cb->setFont(f);
  }

  QString name = win->objectName();
  if (name == "colPanel" || name == "stripPanel") {
    win->setMinimumWidth(120 * scale);
  } else if (name == "monMixPanel") {
    win->setFixedWidth(120 * scale);
  } else if (name == "headerPanel") {
    win->setFixedHeight(44 * scale);
  }

  for (QObject *o : win->children()) {
    auto *child = qobject_cast<QWidget *>(o);
    if (child && !child->isWindow())
      ScaleUIElements(child, scale);
  }
}

// ----------------------------------------------------------------------------
// Main window
// ----------------------------------------------------------------------------

class TPMixer : public QWidget {
public:
  std::atomic<bool> toStopHidReader = false;

  ToppingHID *hid;
  Gain *gain;

  PanelInputs *panelInputs;
  PanelMixers *panelMixers;
  PanelLoopbacks *panelLoopbacks;
  PanelOutputs *panelOutputs;
  QLabel *valWork;

  TPMixer();
  ~TPMixer() override {
    stopHidReader();
    delete hid;
    delete gain;
  }

protected:
  const std::string pathSep = "/";
  const std::string dirConfig = ".config";
  const std::string dirApp = "toppingmixer";
  const std::string ConfigFile = "toppingmixer.settings";
  std::string dir1;
  std::string dir2;
  std::string fileCfg;

  std::thread *thReader = nullptr;
  bool closed = false;

  static const int DESIGN_W = 400 + 760 + 24;
  static const int DESIGN_H = 760;

  void callAfter(std::function<void()> f) {
    QMetaObject::invokeMethod(this, std::move(f), Qt::QueuedConnection);
  }

  void scbUpdateLevels(uint16_t ch16, int32_t val);
  void pushGuiStateToDevice();
  void HidReader(hid_device *handle);
  void startHidReader() {
    thReader = new std::thread(&TPMixer::HidReader, this, hid->getHandle());
  };
  void stopHidReader() {
    toStopHidReader = true;
    if (thReader && thReader->joinable())
      thReader->join();
    delete thReader;
    thReader = nullptr;
  }

  int numOutputs() const {
    return (hid->pid == 0x8752)
               ? 1
               : ((hid->pid == 0x8755 || hid->pid == 0x8756) ? 2 : 3);
  }

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

  void sendInput(int32_t ch, bool hw = true) {
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

      uint16_t keyBase = hid->getHidInputChannel(i) << 8;

      hid->settings[keyBase | 0x01] = mon ? 1 : 0;
      if (panelInputs->cbInst[colMap]) {
        hid->settings[keyBase | 0x03] =
            panelInputs->cbInst[colMap]->GetValue() ? 1 : 0;
      }
      hid->settings[keyBase | 0x05] = gainVal;

      hid->settings[keyBase | 0x06] = mute ? 1 : 0;
      hid->settings[keyBase | 0x07] = solo ? 1 : 0;
      hid->settings[keyBase | 0x08] = phase ? 1 : 0;
      hid->settings[keyBase | 0x09] = loopGainDB;
    }
  }

  void saveSettings();
  bool loadSettings();
  void refreshInputsUi();
  void refreshMixerUi(int16_t bus = -1);
  void refreshLoopbackUi();
  void refreshOutputUi();

  void OnLoad();
  void OnSave();
  void OnResetDefaults();

  void OnSettings() {
    SettingsDialog dlg(this, hid->pid, hid, [this] { OnResetDefaults(); });
    ScaleUIElements(&dlg, g_uiScale);
    dlg.exec();
  }

  void OnInputGain(int32_t id);
  void OnMixBusSel(int32_t bus) { refreshMixerUi(bus); }
  void OnOutputVolume(int32_t id);
  void OnOutputToggle(int32_t id, int32_t val);
  void OnLoopVolume(int32_t id);
  void OnLoopToggle(int32_t id, int32_t val);
  void OnPhoneMix(int32_t val);
  void OnPhoneGain(int32_t val);
  void OnInputPeak(int32_t id);
  void OnLinkToggle(int32_t id);
  void OnMixVolumeChanged(int stripIdx);
  void RescaleUI(double scale);

  void closeEvent(QCloseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
};

// ----------------------------------------------------------------------------
// TPMixer constructor
// ----------------------------------------------------------------------------

TPMixer::TPMixer() : QWidget(nullptr) {
  setWindowTitle("TOPPING Professional Control Center");
  setBg(this, COL_WINDOW);

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
    setWindowTitle(qs(deviceName + " Control Center"));
  }

  auto *mainSizer = vbox(this);
  // children shrink with the window (RescaleUI), so the layout must not
  // force the window's minimum size up
  mainSizer->setSizeConstraint(QLayout::SetNoConstraint);

  // 1. TOP HEADER PANEL
  QWidget *headerPanel = mkPanel(this, COL_HEADER, "headerPanel");
  headerPanel->setFixedHeight(44);
  auto *headerSizer = hbox(headerPanel, 0, 0);
  headerSizer->setContentsMargins(16, 0, 18, 0);

  QLabel *logo = mkLabel(headerPanel, "TOPPING", 16, false, QColor(235, 235, 235));
  QFont lf = logo->font();
  lf.setLetterSpacing(QFont::PercentageSpacing, 112);
  lf.setWeight(QFont::Light);
  logo->setFont(lf);
  headerSizer->addWidget(logo);
  headerSizer->addSpacing(8);
  headerSizer->addWidget(mkLabel(headerPanel, "Professional", 13, false, QColor(225, 225, 225)));
  headerSizer->addSpacing(14);
  auto *btnSettings = new CustomButton(headerPanel, "", false, QSize(24, 24), COL_BLUE, ICON_GEAR, BTN_ICON);
  headerSizer->addWidget(btnSettings, 0, Qt::AlignVCenter);
  headerSizer->addStretch(1);
  headerSizer->addWidget(mkLabel(headerPanel, "Workspace", 11, true, QColor(170, 170, 170)));
  headerSizer->addSpacing(8);
  auto *bar = new QWidget(headerPanel);
  setBg(bar, QColor(200, 200, 200));
  bar->setFixedSize(2, 22);
  headerSizer->addWidget(bar, 0, Qt::AlignVCenter);
  headerSizer->addSpacing(10);
  valWork = mkLabel(headerPanel, "Default", 11, true, QColor(235, 235, 235));
  headerSizer->addWidget(valWork);
  headerSizer->addSpacing(24);
  auto *btnSave = new CustomButton(headerPanel, "", false, QSize(24, 24), COL_BLUE, ICON_SAVE, BTN_ICON);
  auto *btnLoad = new CustomButton(headerPanel, "", false, QSize(24, 24), COL_BLUE, ICON_DOWNLOAD, BTN_ICON);
  headerSizer->addWidget(btnSave, 0, Qt::AlignVCenter);
  headerSizer->addSpacing(8);
  headerSizer->addWidget(btnLoad, 0, Qt::AlignVCenter);
  mainSizer->addWidget(headerPanel);

  // 2. MAIN GRID WITH BALANCED COLUMNS
  panelInputs = new PanelInputs(this, hid->pid);
  panelMixers = new PanelMixers(this);
  panelLoopbacks = new PanelLoopbacks(this);
  panelOutputs = new PanelOutputs(this, hid->pid);

  auto *col1 = vbox(nullptr, 0, 8);
  col1->addWidget(panelInputs, 1);
  col1->addWidget(panelLoopbacks, 1);
  auto *col2 = vbox(nullptr, 0, 8);
  col2->addWidget(panelMixers, 1);
  col2->addWidget(panelOutputs, 1, Qt::AlignLeft);

  auto *contentSizer = hbox(nullptr, 8, 8);
  contentSizer->addLayout(col1);
  contentSizer->addLayout(col2);
  contentSizer->addStretch(1);
  mainSizer->addLayout(contentSizer, 1);

  btnSettings->onClick = [this](bool) { OnSettings(); };
  btnSave->onClick = [this](bool) { OnSave(); };
  btnLoad->onClick = [this](bool) { OnLoad(); };

  auto bindIn = [this](CustomButton *b, int id) {
    if (b)
      b->onClick = [this, id](bool) { OnInputGain(id); };
  };
  auto bindInF = [this](FaderStrip *f, int id) {
    if (f)
      f->onChange = [this, id](int) { OnInputGain(id); };
  };
  int numCols = (hid->pid == 0x8754)
                    ? 3
                    : ((hid->pid == 0x8752 || hid->pid == 0x8756) ? 2 : 1);
  for (int col = 0; col < numCols; ++col) {
    if (col < 2) {
      bindIn(panelInputs->cbMon[col], ID_INPUT_MON + col);
      bindIn(panelInputs->cb48V[col], ID_INPUT_48V + col);
      bindIn(panelInputs->cbInst[col], ID_INPUT_INST + col);
      bindInF(panelInputs->slGainI[col], ID_INPUT_GAIN + col);
      bindIn(panelInputs->cbSolo[col], ID_INPUT_SOLO + col);
      bindIn(panelInputs->cbMute[col], ID_INPUT_MUTE + col);
      bindIn(panelInputs->cbPhase[col], ID_INPUT_PHASE + col);
    } else {
      bindInF(panelInputs->slGainI[2], ID_INPUT_GAIN + 2);
      bindInF(panelInputs->slGainI[3], ID_INPUT_GAIN + 3);
      bindInF(panelInputs->slGainICombined, ID_INPUT_GAIN + 2);
      if (panelInputs->btnLink)
        panelInputs->btnLink->onClick = [this](bool) { OnLinkToggle(ID_INPUT_LINK); };
      // Input split controls
      bindIn(panelInputs->cbSoloL, ID_INPUT_SOLO + 2);
      bindIn(panelInputs->cbSoloR, ID_INPUT_SOLO + 3);
      bindIn(panelInputs->cbMuteL, ID_INPUT_MUTE + 2);
      bindIn(panelInputs->cbMuteR, ID_INPUT_MUTE + 3);
      bindIn(panelInputs->cbPhaseL, ID_INPUT_PHASE + 2);
      bindIn(panelInputs->cbPhaseR, ID_INPUT_PHASE + 3);
      // Combined
      bindIn(panelInputs->cbSolo[2], ID_INPUT_SOLO + 2);
      bindIn(panelInputs->cbMute[2], ID_INPUT_MUTE + 2);
      bindIn(panelInputs->cbPhase[2], ID_INPUT_PHASE + 2);
    }
    if (panelInputs->lbPeaksI[col])
      panelInputs->lbPeaksI[col]->onClick = [this, col](bool) {
        OnInputPeak(ID_INPUT_PEAK + col);
      };
  }

  for (int i = 0; i < panelMixers->N_MIXERS; ++i) {
    panelMixers->tabButtons[i]->onClick = [this, i](bool) {
      for (int t = 0; t < panelMixers->N_MIXERS; ++t)
        panelMixers->tabButtons[t]->SetValue(t == i);
      OnMixBusSel(i);
    };
  }

  for (int i = 0; i < panelMixers->N_STRIPS; ++i) {
    auto changed = [this, i](auto) { OnMixVolumeChanged(i); };
    for (auto *k : {panelMixers->slPan[i * 2], panelMixers->slPan[i * 2 + 1]})
      k->onChange = changed;
    for (auto *f : {panelMixers->slVol[i], panelMixers->slVolL[i], panelMixers->slVolR[i]})
      f->onChange = changed;
    for (auto *b : {panelMixers->ckSolo[i], panelMixers->ckMute[i],
                    panelMixers->ckPhaseCombined[i], panelMixers->ckSoloL[i],
                    panelMixers->ckSoloR[i], panelMixers->ckMuteL[i],
                    panelMixers->ckMuteR[i], panelMixers->ckPhase[i * 2],
                    panelMixers->ckPhase[i * 2 + 1]})
      b->onClick = changed;
    panelMixers->btnLink[i]->onClick = [this, i](bool) { OnLinkToggle(ID_MIX_LINK + i); };
  }

  for (int i = 0; i < panelLoopbacks->N_LOOPBACKS; ++i) {
    connect(panelLoopbacks->cbSelect[i], &QComboBox::activated, this,
            [this, i](int idx) { OnLoopToggle(ID_LOOP_SEL + i, idx); });
    for (auto *f : {panelLoopbacks->slOutput[i], panelLoopbacks->slOutputL[i],
                    panelLoopbacks->slOutputR[i]})
      f->onChange = [this, i](int) { OnLoopVolume(ID_LOOP_VOL_B + i); };
    panelLoopbacks->btnLink[i]->onClick = [this, i](bool) { OnLinkToggle(ID_LOOP_LINK + i); };
    for (auto *b : {panelLoopbacks->ckMute[i], panelLoopbacks->ckMuteL[i],
                    panelLoopbacks->ckMuteR[i]})
      b->onClick = [this, i](bool on) { OnLoopToggle(ID_LOOP_MUTE + i, on); };
  }

  for (int i = 0; i < numOutputs(); ++i) {
    connect(panelOutputs->cbSelect[i], &QComboBox::activated, this,
            [this, i](int idx) { OnOutputToggle(ID_OUTPUT_SEL + i, idx); });
    for (auto *f : {panelOutputs->slOutput[i], panelOutputs->slOutputL[i],
                    panelOutputs->slOutputR[i]})
      f->onChange = [this, i](int) { OnOutputVolume(ID_OUTPUT_VOL_B + i); };
    panelOutputs->btnLink[i]->onClick = [this, i](bool) { OnLinkToggle(ID_OUTPUT_LINK + i); };
    if (i == 0) {
      panelOutputs->btnPhoneIcon[0]->onClick = [this](bool on) { OnOutputToggle(ID_OUTPUT_MON, on); };
      panelOutputs->btnTRS[0]->onClick = [this](bool on) { OnOutputToggle(ID_OUTPUT_LINE, on); };
      if (panelOutputs->btnAUX[0])
        panelOutputs->btnAUX[0]->onClick = [this](bool on) { OnOutputToggle(ID_OUTPUT_LINE + 1, on); };
    } else {
      for (auto *b : {panelOutputs->ckMute[i], panelOutputs->ckMuteL[i],
                      panelOutputs->ckMuteR[i]})
        b->onClick = [this, i](bool on) { OnOutputToggle(ID_OUTPUT_MON + i, on); };
    }
  }

  panelOutputs->mixKnob->onChange = [this](int v) { OnPhoneMix(v); };
  panelOutputs->btnPhoneGain->onClick = [this](bool on) { OnPhoneGain(on); };

  if (hid->pid == 0x8755 || hid->pid == 0x8756) {
    panelMixers->lbTitle[0]->setText(hid->pid == 0x8755 ? "Analog IN" : "IN 1+2");
    panelMixers->lbTitle[1]->setText("Mobile IN");
  } else if (hid->pid == 0x8752) {
    panelMixers->lbTitle[1]->setText("Unused");
  }

  loadSettings();
  refreshInputsUi();
  refreshMixerUi(-1);
  refreshLoopbackUi();
  refreshOutputUi();

  setMinimumSize(DESIGN_W, DESIGN_H);
  resize(DESIGN_W * g_uiScale, DESIGN_H * g_uiScale);
  RescaleUI(g_uiScale);

  startHidReader();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (NULL != hid->getHandle()) {
    pushGuiStateToDevice();
  }
}

// ----------------------------------------------------------------------------
// scbUpdateLevels callback routing
// ----------------------------------------------------------------------------

static int getLogicInputChannel(uint8_t hidCh, uint16_t pid) {
  if (pid == 0x8756) {
    if (hidCh == 0x21) return 0; // Analog IN 1
    if (hidCh == 0x23) return 1; // Analog IN 2
    if (hidCh == 0x22) return 2; // Mobile In
  }
  return hidCh - 0x21;
}

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
  } else if (cls == 0x30 && (ch - 0x31) < 10 && (subCh == 0x01 || subCh == 0x02)) {
    isLevelMeter = true;
  } else if (cls == 0x50 && (subCh == 0x01 || subCh == 0x02)) {
    isLevelMeter = true;
  }

  if (!isLevelMeter) {
    hid->settings[ch16] = val;
  }

  switch (cls) {
  case 0x10:
    break;
  case 0x20: {
    int32_t logicCh = getLogicInputChannel(ch, hid->pid);
    if (logicCh < 0 || logicCh >= panelInputs->N_INPUTS)
      return;
    int colMap = (logicCh >= 2) ? 2 : logicCh;
    bool inputLinked =
        (panelInputs->btnLink) ? panelInputs->btnLink->GetValue() : true;
    uint16_t keyBase = hid->getHidInputChannel(logicCh) << 8;

    switch (subCh) {
    case 0x01:
      if (panelInputs->cbMon[colMap]) {
        panelInputs->cbMon[colMap]->SetValue(val);
      }
      hid->settings[keyBase | 0x01] = val;
      break;
    case 0x02:
      if (panelInputs->cb48V[colMap]) {
        panelInputs->cb48V[colMap]->SetValue(val);
      }
      hid->settings[keyBase | 0x02] = val;
      break;
    case 0x03:
      if (panelInputs->cbInst[colMap]) {
        panelInputs->cbInst[colMap]->SetValue(val);
      }
      hid->settings[keyBase | 0x03] = val;
      break;
    case 0x04: {
      panelInputs->inLevelVal[logicCh] = level01DB;
      if (logicCh < 2) {
        if (panelInputs->slGainI[logicCh]) {
          panelInputs->slGainI[logicCh]->SetMeterLevels(level01DB, -960);
        }
        if (level01DB > panelInputs->PeaksI[logicCh]) {
          panelInputs->PeaksI[logicCh] = level01DB;
          if (panelInputs->lbPeaksI[logicCh]) {
            panelInputs->lbPeaksI[logicCh]->SetLabel(
                qs(std::format("{:+.1f}", level01DB * 0.1)));
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
                qs(std::format("{:+.1f}", level01DB * 0.1)));
          }
        }
      }

      // E1X2 OTG reports IN1, Mobile L, Mobile R on 0x21..0x23, but its
      // mixer sources are IN1 = 0 and Mobile L/R = 2/3 (strip "Mobile In").
      int mixCh = (hid->pid == 0x8755 && logicCh > 0) ? logicCh + 1 : logicCh;
      if (mixCh >= 4)
        break;
      panelMixers->mixLevelVal[mixCh] = level01DB;
      int strip = mixCh / 2;
      bool mixLinked = panelMixers->btnLink[strip]->GetValue();
      if (mixLinked) {
        panelMixers->slVol[strip]->SetMeterLevels(
            panelMixers->mixLevelVal[strip * 2],
            panelMixers->mixLevelVal[strip * 2 + 1]);
      } else {
        panelMixers->slVolL[strip]->SetMeterLevels(
            panelMixers->mixLevelVal[strip * 2], -960);
        panelMixers->slVolR[strip]->SetMeterLevels(
            panelMixers->mixLevelVal[strip * 2 + 1], -960);
      }
    } break;
    case 0x05:
      {
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
          panelInputs->lbGainVal[colMap]->setText(
              qs(std::format("{:+} dB", gainDB)));
        }
      }
      break;
    case 0x06:
      if (panelInputs->cbMute[colMap])
        panelInputs->cbMute[colMap]->SetValue(val ? true : false);
      hid->settings[keyBase | 0x06] = val;
      break;
    case 0x07:
      if (panelInputs->cbSolo[colMap])
        panelInputs->cbSolo[colMap]->SetValue(val ? true : false);
      hid->settings[keyBase | 0x07] = val;
      break;
    case 0x08:
      if (panelInputs->cbPhase[colMap])
        panelInputs->cbPhase[colMap]->SetValue(val ? true : false);
      hid->settings[keyBase | 0x08] = val;
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
        panelInputs->lbGainVal[colMap]->setText(qs(std::format("{:+} dB", val)));
      }
      hid->settings[keyBase | 0x09] = val;
      break;
    }
  } break;
  case 0x30: {
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
            panelOutputs->cbSelect[phoneIdx]->setCurrentIndex(val - 1);
          }
          hid->settings[ch16] = val;
        }
      } else if (subCh == 2) {
        panelOutputs->m_phoneGain[phoneIdx] = val ? true : false;
        if (panelOutputs->choicePhoneOut->currentIndex() == phoneIdx) {
          panelOutputs->btnPhoneGain->SetValue(val ? true : false);
        }
        hid->settings[ch16] = val;
      } else if (subCh == 3) {
        int phoneMix = (val - 50) * 2;
        panelOutputs->m_phoneMix[phoneIdx] = phoneMix;
        if (panelOutputs->choicePhoneOut->currentIndex() == phoneIdx) {
          panelOutputs->mixKnob->SetValue(phoneMix);
          panelOutputs->updatePhoneMixLabel(phoneMix);
        }
        hid->settings[ch16] = val;
      }
    } else if (logicCh == 0x06) {
      if (subCh == 1) {
        if (panelOutputs->btnPhoneIcon[0])
          panelOutputs->btnPhoneIcon[0]->SetValue(val != 0);
        hid->settings[0x3701] = val;
      } else if (subCh == 2) {
        if (panelOutputs->btnPhoneIcon[1])
          panelOutputs->btnPhoneIcon[1]->SetValue(val != 0);
        hid->settings[0x3702] = val;
      } else if (subCh == 3) {
        if (panelOutputs->btnTRS[0])
          panelOutputs->btnTRS[0]->SetValue(val != 0);
        hid->settings[0x3703] = val;
      } else if (subCh == 4) {
        if (panelOutputs->btnAUX[0] && hid->pid == 0x8754)
          panelOutputs->btnAUX[0]->SetValue(val != 0);
        hid->settings[0x3704] = val;
      } else if (subCh == 5) {
        if (panelOutputs->btnAUX[0] && hid->pid == 0x8756)
          panelOutputs->btnAUX[0]->SetValue(val != 0);
        hid->settings[0x3705] = val;
      }
    } else {
      if (logicCh < 6 && subCh == 1) {
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
    int32_t logicBus = ch - 0x41;
    int stripIdx = 2 + (logicBus / 2);
    if (logicBus >= 0 && stripIdx < 6) {
      panelMixers->mixLevelVal[logicBus + 4] = level01DB;
      bool mixLinked = panelMixers->btnLink[stripIdx]->GetValue();
      if (mixLinked) {
        panelMixers->slVol[stripIdx]->SetMeterLevels(
            panelMixers->mixLevelVal[stripIdx * 2],
            panelMixers->mixLevelVal[stripIdx * 2 + 1]);
      } else {
        panelMixers->slVolL[stripIdx]->SetMeterLevels(
            panelMixers->mixLevelVal[stripIdx * 2], -960);
        panelMixers->slVolR[stripIdx]->SetMeterLevels(
            panelMixers->mixLevelVal[stripIdx * 2 + 1], -960);
      }
    }
  } break;
  case 0x50: {
    int32_t logicCh = ch - 0x51;
    if (logicCh < 0)
      return;
    if (logicCh < 6) {
      if (subCh == 1) {
        panelLoopbacks->loopLevelVal[logicCh] = level01DB;
        int strip = logicCh / 2;
        bool loopLinked = panelLoopbacks->btnLink[strip]->GetValue();
        if (loopLinked) {
          panelLoopbacks->slOutput[strip]->SetMeterLevels(
              panelLoopbacks->loopLevelVal[strip * 2],
              panelLoopbacks->loopLevelVal[strip * 2 + 1]);
        } else {
          panelLoopbacks->slOutputL[strip]->SetMeterLevels(
              panelLoopbacks->loopLevelVal[strip * 2], -960);
          panelLoopbacks->slOutputR[strip]->SetMeterLevels(
              panelLoopbacks->loopLevelVal[strip * 2 + 1], -960);
        }
      } else if (subCh == 3) {
        auto [muted, dummy, gainDB] = gain2dB(val);
        int strip = logicCh / 2;
        bool loopLinked = panelLoopbacks->btnLink[strip]->GetValue();
        if (loopLinked) {
          panelLoopbacks->slOutput[strip]->SetValue(gainDB);
        } else if (logicCh % 2 == 0) {
          panelLoopbacks->slOutputL[strip]->SetValue(gainDB);
        } else {
          panelLoopbacks->slOutputR[strip]->SetValue(gainDB);
        }
      }
    } else {
      int loopIdx = logicCh - 6;
      if (loopIdx < panelLoopbacks->N_LOOPBACKS) {
        if (val >= 1 && val <= 14)
          panelLoopbacks->cbSelect[loopIdx]->setCurrentIndex(val - 1);
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
  if (NULL != handle) {
    while (!toStopHidReader) {
      int res = hid_read_timeout(handle, bufread, 16, 100);
      if (res < 0) {
        break;
      }
      if (res >= 11) {
        int offset = -1;
        if (bufread[0] == 0x22 && bufread[1] == 0x33 && bufread[2] == 0x20) {
          offset = 0;
        } else if (bufread[1] == 0x22 && bufread[2] == 0x33 && bufread[3] == 0x20) {
          offset = 1;
        }
        if (offset >= 0) {
          uint16_t ch = read16BE(&bufread[offset + 5]);
          int32_t val = read32BE(&bufread[offset + 7]);
          callAfter([this, ch, val] { scbUpdateLevels(ch, val); });
        }
      }
    }
  } else {
    while (!toStopHidReader) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      int activeCols = (hid->pid == 0x8754) ? 3 : 2;
      for (int i = 0; i < activeCols; ++i) {
        int valLeft =
            panelInputs->LEVEL_MIN + rand() % (panelInputs->LEVEL_RANGE);
        uint16_t ch = (hid->getHidInputChannel(i) << 8) | 0x04;
        callAfter([this, ch, valLeft] { scbUpdateLevels(ch, valLeft); });
      }
      for (int i = 0; i < 6; ++i) {
        int valPlay =
            panelMixers->LEVEL_MIN + rand() % (panelMixers->LEVEL_RANGE);
        uint16_t ch = 0x4101 + (i << 8);
        callAfter([this, ch, valPlay] { scbUpdateLevels(ch, valPlay); });
      }
    }
  }
}

void TPMixer::RescaleUI(double scale) {
  panelInputs->setFixedWidth(400 * scale);
  panelLoopbacks->setFixedWidth(400 * scale);
  panelMixers->setFixedWidth(760 * scale);
  panelOutputs->setFixedWidth(760 * scale);
  ScaleUIElements(this, scale);
  update();
}

void TPMixer::resizeEvent(QResizeEvent *event) {
  double newScale = std::min((double)width() / DESIGN_W,
                             (double)height() / DESIGN_H);
  newScale = std::clamp(newScale, 0.5, 3.0);
  if (std::abs(newScale - g_uiScale) > 0.02) {
    g_uiScale = newScale;
    RescaleUI(g_uiScale);
  }
  QWidget::resizeEvent(event);
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
  if (NULL == f)
    return false;
  while ((n = getline(&line, &len, f)) != -1) {
    if (sscanf(line, "%" SCNx16 " %" SCNx32, &key, &value) != 2)
      continue;
    if (key == 0x9999) {
      g_uiScale = std::clamp((double)value / 100.0, 0.5, 3.0);
    } else {
      hid->settings[key] = value;
      callAfter([this, key, value] { scbUpdateLevels(key, value); });
    }
  }
  if (NULL != line)
    free(line);
  fclose(f);
  callAfter([this] { refreshMixerUi(-1); });
  callAfter([this] { refreshLoopbackUi(); });
  callAfter([this] { refreshOutputUi(); });
  if (panelInputs->btnLink) {
    bool linked =
        hid->settings.contains(0x9000) ? (hid->settings[0x9000] != 0) : true;
    panelInputs->btnLink->SetValue(linked);
    panelInputs->slGainICombined->setVisible(linked);
    panelInputs->slGainI[2]->setVisible(!linked);
    panelInputs->slGainI[3]->setVisible(!linked);
    panelInputs->combBtns->setVisible(linked);
    panelInputs->splitBtns->setVisible(!linked);
  }
  return true;
}

void TPMixer::pushGuiStateToDevice() {
  if (NULL == hid->getHandle())
    return;
  auto pause = [] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); };

  // 1. Push Inputs: MON, 48V, INST, and Gain/Mute/Solo/Phase
  for (int i = 0; i < hid->numInputs; ++i) {
    if (i < 2) {
      if (panelInputs->cbMon[i]) {
        hid->setInputMon(i, panelInputs->cbMon[i]->GetValue());
        pause();
      }
      if (panelInputs->cb48V[i]) {
        hid->setInput48V(i, panelInputs->cb48V[i]->GetValue());
        pause();
      }
      if (panelInputs->cbInst[i]) {
        hid->setInputInst(i, panelInputs->cbInst[i]->GetValue());
        pause();
      }
    }
    sendInput(i, true);
    pause();
  }

  // 2. Push Mixer volumes (Mix A, B, C, D) for all 12 channels
  for (int bus = 0; bus < 4; ++bus) {
    for (int src = 0; src < 12; ++src) {
      uint16_t keyL = ((0x61 + bus * 2) << 8) | (src + 1);
      uint16_t keyR = ((0x62 + bus * 2) << 8) | (src + 1);
      if (hid->settings.contains(keyL) && hid->settings.contains(keyR)) {
        hid->setMixVol(bus, src, hid->settings[keyL], hid->settings[keyR]);
        pause();
      }
    }
  }

  // 3. Push Output Routing Selection and Volume
  for (int i = 0; i < numOutputs(); ++i) {
    if (panelOutputs->cbSelect[i]) {
      hid->setOutputSel(i, panelOutputs->cbSelect[i]->currentIndex() + 1);
      pause();
    }
    setOutputVol(i);
    pause();
  }

  // 4. Push Phone/TRS/AUX enables (0x3701..0x3705)
  if (hid->settings.contains(0x3701)) {
    hid->setOutputMon(0, hid->settings[0x3701] != 0);
    pause();
  }
  if (hid->settings.contains(0x3702) && hid->pid == 0x8754) {
    hid->setOutputMon(1, hid->settings[0x3702] != 0);
    pause();
  }
  if (hid->settings.contains(0x3703)) {
    hid->setOutputLine(0, hid->settings[0x3703] != 0);
    pause();
  }
  if (hid->settings.contains(0x3704) && hid->pid == 0x8754) {
    hid->setOutputLine(1, hid->settings[0x3704] != 0);
    pause();
  }
  if (hid->settings.contains(0x3705) && hid->pid == 0x8756) {
    hid->setDeviceSetting(0x37, 0x05, hid->settings[0x3705] != 0 ? 1 : 0);
    pause();
  }

  // 5. Push Phone Gain and Phone Mix
  uint8_t phoneBase = hid->phoneRegOffset();
  for (int i = 0; i < 2; ++i) {
    uint16_t keyGain = ((phoneBase + i) << 8) | 2;
    uint16_t keyMix = ((phoneBase + i) << 8) | 3;
    if (hid->settings.contains(keyGain)) {
      hid->setPhoneGainBoost(i, hid->settings[keyGain]);
      pause();
    }
    if (hid->settings.contains(keyMix)) {
      hid->setPhoneMix(i, hid->settings[keyMix]);
      pause();
    }
  }

  // 6. Push Loopback selection and levels
  for (int i = 0; i < panelLoopbacks->N_LOOPBACKS; ++i) {
    hid->setLoopSel(i, panelLoopbacks->cbSelect[i]->currentIndex() + 1);
    pause();
    setLoopVol(i);
    pause();
  }

  // 7. Push Device Standby, OTG Mode, and LED Brightness Settings
  if (hid->pid == 0x8754) {
    if (hid->settings.contains(0x3901)) {
      hid->setDeviceSetting(0x39, 0x01, hid->settings[0x3901]);
      pause();
    }
    if (hid->settings.contains(0x3a01)) {
      hid->setDeviceSetting(0x3a, 0x01, hid->settings[0x3a01]);
      pause();
    }
  } else {
    if (hid->settings.contains(0x1101)) {
      hid->setDeviceSetting(0x11, 0x01, hid->settings[0x1101]);
      pause();
    }
    if (hid->settings.contains(0x1103)) {
      hid->setDeviceSetting(0x11, 0x03, hid->settings[0x1103]);
      pause();
    }
  }
  if (hid->settings.contains(0x1104)) {
    hid->setDeviceSetting(0x11, 0x04, hid->settings[0x1104]);
    pause();
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
    uint16_t keyBase = hid->getHidInputChannel(logicCh) << 8;
    auto setBtn = [&](CustomButton *b, uint16_t key) {
      if (b && hid->settings.contains(key))
        b->SetValue(hid->settings[key] != 0);
    };
    setBtn(panelInputs->cbMon[colMap], keyBase | 0x01);
    setBtn(panelInputs->cb48V[colMap], keyBase | 0x02);
    setBtn(panelInputs->cbInst[colMap], keyBase | 0x03);
    setBtn(panelInputs->cbMute[colMap], keyBase | 0x06);
    setBtn(panelInputs->cbSolo[colMap], keyBase | 0x07);
    setBtn(panelInputs->cbPhase[colMap], keyBase | 0x08);

    uint16_t keyGain = keyBase | 0x09;
    if (!hid->settings.contains(keyGain)) {
      keyGain = keyBase | 0x05;
    }
    if (hid->settings.contains(keyGain)) {
      int32_t val = hid->settings[keyGain];
      if ((keyGain & 0xff) == 0x05)
        val = std::get<2>(gain2dB(val));
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
        panelInputs->lbGainVal[colMap]->setText(qs(std::format("{:+} dB", val)));
      }
    }
  }
}

static QString dbPair(bool mutedL, int32_t dBL, bool mutedR, int32_t dBR) {
  QString l = mutedL ? "-inf" : QString::number(dBL);
  QString r = mutedR ? "-inf" : QString::number(dBR);
  return l + "  " + r;
}

void TPMixer::refreshMixerUi(int16_t bus) {
  if (bus < 0)
    bus = 0;
  for (int16_t stripIdx = 0; stripIdx < panelMixers->N_STRIPS; stripIdx++) {
    int l = stripIdx * 2;
    int r = l + 1;

    // Left source keys
    uint16_t keyL_L = ((0x61 + bus * 2) << 8) | (l + 1);
    uint16_t keyL_R = ((0x62 + bus * 2) << 8) | (l + 1);

    // Right source keys
    uint16_t keyR_L = ((0x61 + bus * 2) << 8) | (r + 1);
    uint16_t keyR_R = ((0x62 + bus * 2) << 8) | (r + 1);

    if (!(hid->settings.contains(keyL_L) && hid->settings.contains(keyL_R) &&
          hid->settings.contains(keyR_L) && hid->settings.contains(keyR_R)))
      continue;

    auto [mutedL, phaseL, panL, gainDBL] = lrGain2PandB(
        hid->settings[keyL_L], hid->settings[keyL_R], panelMixers->minGain);
    auto [mutedR, phaseR, panR, gainDBR] = lrGain2PandB(
        hid->settings[keyR_L], hid->settings[keyR_R], panelMixers->minGain);

    panelMixers->slPan[l]->SetValue(panL);
    panelMixers->slPan[r]->SetValue(panR);

    bool linked = hid->settings.contains(0x9100 + stripIdx)
                      ? (hid->settings[0x9100 + stripIdx] != 0)
                      : true;
    panelMixers->btnLink[stripIdx]->SetValue(linked);

    if (linked) {
      panelMixers->slVol[stripIdx]->SetValue(gainDBL);
      panelMixers->ckSolo[stripIdx]->SetValue(false);
      panelMixers->ckMute[stripIdx]->SetValue(mutedL);
      panelMixers->ckPhaseCombined[stripIdx]->SetValue(phaseL);
    } else {
      panelMixers->slVolL[stripIdx]->SetValue(gainDBL);
      panelMixers->slVolR[stripIdx]->SetValue(gainDBR);
      panelMixers->ckSoloL[stripIdx]->SetValue(false);
      panelMixers->ckSoloR[stripIdx]->SetValue(false);
      panelMixers->ckMuteL[stripIdx]->SetValue(mutedL);
      panelMixers->ckMuteR[stripIdx]->SetValue(mutedR);
      panelMixers->ckPhase[l]->SetValue(phaseL);
      panelMixers->ckPhase[r]->SetValue(phaseR);
    }

    panelMixers->slVol[stripIdx]->setVisible(linked);
    panelMixers->slVolL[stripIdx]->setVisible(!linked);
    panelMixers->slVolR[stripIdx]->setVisible(!linked);
    panelMixers->combBtns[stripIdx]->setVisible(linked);
    panelMixers->splitBtns[stripIdx]->setVisible(!linked);

    if (linked)
      panelMixers->lbVolVal[stripIdx]->setText(
          dbPair(mutedL, gainDBL, mutedL, gainDBL));
    else
      panelMixers->lbVolVal[stripIdx]->setText(
          dbPair(mutedL, gainDBL, mutedR, gainDBR));
  }
}

void TPMixer::refreshLoopbackUi() {
  for (int16_t i = 0; i < panelLoopbacks->N_LOOPBACKS; i++) {
    int32_t valL = hid->settings[0x5103 + ((i * 2) << 8)];
    int32_t valR = hid->settings[0x5103 + ((i * 2 + 1) << 8)];

    auto [mutedL, dummyL, gainDBL] = gain2dB(valL);
    auto [mutedR, dummyR, gainDBR] = gain2dB(valR);
    bool linked = hid->settings.contains(0x9200 + i)
                      ? (hid->settings[0x9200 + i] != 0)
                      : true;

    panelLoopbacks->btnLink[i]->SetValue(linked);

    if (linked) {
      panelLoopbacks->slOutput[i]->SetValue(gainDBL);
      panelLoopbacks->ckMute[i]->SetValue(mutedL);
    } else {
      panelLoopbacks->slOutputL[i]->SetValue(gainDBL);
      panelLoopbacks->slOutputR[i]->SetValue(gainDBR);
      panelLoopbacks->ckMuteL[i]->SetValue(mutedL);
      panelLoopbacks->ckMuteR[i]->SetValue(mutedR);
    }

    panelLoopbacks->slOutput[i]->setVisible(linked);
    panelLoopbacks->slOutputL[i]->setVisible(!linked);
    panelLoopbacks->slOutputR[i]->setVisible(!linked);
    panelLoopbacks->combBtns[i]->setVisible(linked);
    panelLoopbacks->splitBtns[i]->setVisible(!linked);

    panelLoopbacks->lbLoopVolVal[i]->setText(
        dbPair(mutedL, gainDBL, mutedR, gainDBR));
    int32_t selVal = hid->settings[0x5701 + (i << 8)];
    if (selVal >= 1 && selVal <= 14) {
      panelLoopbacks->cbSelect[i]->setCurrentIndex(selVal - 1);
    }
  }
}

void TPMixer::refreshOutputUi() {
  for (int16_t i = 0; i < numOutputs(); i++) {
    int32_t valL = hid->settings[0x3103 + ((i * 2) << 8)];
    int32_t valR = hid->settings[0x3103 + ((i * 2 + 1) << 8)];

    auto [mutedL, dummyL, gainDBL] = gain2dB(valL);
    auto [mutedR, dummyR, gainDBR] = gain2dB(valR);
    bool linked = hid->settings.contains(0x9300 + i)
                      ? (hid->settings[0x9300 + i] != 0)
                      : true;

    panelOutputs->btnLink[i]->SetValue(linked);

    if (linked) {
      panelOutputs->slOutput[i]->SetValue(gainDBL);
      if (i > 0)
        panelOutputs->ckMute[i]->SetValue(mutedL);
    } else {
      panelOutputs->slOutputL[i]->SetValue(gainDBL);
      panelOutputs->slOutputR[i]->SetValue(gainDBR);
      if (i > 0) {
        panelOutputs->ckMuteL[i]->SetValue(mutedL);
        panelOutputs->ckMuteR[i]->SetValue(mutedR);
      }
    }

    panelOutputs->slOutput[i]->setVisible(linked);
    panelOutputs->slOutputL[i]->setVisible(!linked);
    panelOutputs->slOutputR[i]->setVisible(!linked);
    if (i > 0) {
      panelOutputs->combBtns[i]->setVisible(linked);
      panelOutputs->splitBtns[i]->setVisible(!linked);
    }

    panelOutputs->lbOutVolVal[i]->setText(
        dbPair(mutedL, gainDBL, mutedR, gainDBR));
    int32_t selVal = hid->settings[0x3501 + (i << 8)];
    if (selVal >= 1 && selVal <= 14) {
      panelOutputs->cbSelect[i]->setCurrentIndex(selVal - 1);
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
      panelOutputs->m_phoneMix[phoneIdx] = (hid->settings[keyMix] - 50) * 2;
    }
  }
  int curPhone = panelOutputs->choicePhoneOut->currentIndex();
  if (curPhone >= 0) {
    panelOutputs->mixKnob->SetValue(panelOutputs->m_phoneMix[curPhone]);
    panelOutputs->btnPhoneGain->SetValue(panelOutputs->m_phoneGain[curPhone]);
    panelOutputs->updatePhoneMixLabel(panelOutputs->m_phoneMix[curPhone]);
  }

  auto setFromKey = [&](CustomButton *b, uint16_t key) {
    if (b)
      b->SetValue(hid->settings.contains(key) ? (hid->settings[key] != 0) : true);
  };
  setFromKey(panelOutputs->btnPhoneIcon[0], 0x3701);
  setFromKey(panelOutputs->btnTRS[0], 0x3703);
  setFromKey(panelOutputs->btnAUX[0], hid->pid == 0x8756 ? 0x3705 : 0x3704);
}

void TPMixer::OnInputGain(int32_t id) {
  int32_t ch = id & 0x0f;

  int colMap = (ch >= 2) ? 2 : ch;
  bool inputLinked =
      (panelInputs->btnLink) ? panelInputs->btnLink->GetValue() : true;

  if (panelInputs->lbGainVal[colMap]) {
    if (colMap < 2) {
      int32_t gainDB = panelInputs->slGainI[colMap]
                           ? panelInputs->slGainI[colMap]->GetValue()
                           : 0;
      panelInputs->lbGainVal[colMap]->setText(qs(std::format("{:+} dB", gainDB)));
    } else if (inputLinked) {
      int32_t gainDB = panelInputs->slGainICombined
                           ? panelInputs->slGainICombined->GetValue()
                           : 0;
      panelInputs->lbGainVal[colMap]->setText(qs(std::format("{:+} dB", gainDB)));
    } else {
      int32_t gainDBL =
          panelInputs->slGainI[2] ? panelInputs->slGainI[2]->GetValue() : 0;
      int32_t gainDBR =
          panelInputs->slGainI[3] ? panelInputs->slGainI[3]->GetValue() : 0;
      panelInputs->lbGainVal[colMap]->setText(
          qs(std::format("{:+} / {:+}", gainDBL, gainDBR)));
    }
  }

  switch (id & (~0xf)) {
  case ID_INPUT_GAIN:
  case ID_INPUT_SOLO:
  case ID_INPUT_MUTE:
  case ID_INPUT_PHASE:
    sendInput(ch, true);
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

void TPMixer::OnMixVolumeChanged(int stripIdx) {
  int32_t bus = 0;
  for (int t = 0; t < panelMixers->N_MIXERS; ++t) {
    if (panelMixers->tabButtons[t]->GetValue()) {
      bus = t;
      break;
    }
  }

  bool linked = panelMixers->btnLink[stripIdx]->GetValue();
  int32_t panL = panelMixers->slPan[stripIdx * 2]->GetValue();
  int32_t panR = panelMixers->slPan[stripIdx * 2 + 1]->GetValue();

  bool anySolo = false;
  for (int i = 0; i < panelMixers->N_STRIPS; ++i) {
    if (panelMixers->btnLink[i]->GetValue()) {
      anySolo |= panelMixers->ckSolo[i]->GetValue();
    } else {
      anySolo |= panelMixers->ckSoloL[i]->GetValue();
      anySolo |= panelMixers->ckSoloR[i]->GetValue();
    }
  }

  int32_t gainDBL = 0, gainDBR = 0;
  bool muteL = false, muteR = false;
  bool soloL = false, soloR = false;
  bool phaseL = false, phaseR = false;

  if (linked) {
    gainDBL = gainDBR = panelMixers->slVol[stripIdx]->GetValue();
    muteL = muteR = panelMixers->ckMute[stripIdx]->GetValue();
    soloL = soloR = panelMixers->ckSolo[stripIdx]->GetValue();
    phaseL = phaseR = panelMixers->ckPhaseCombined[stripIdx]->GetValue();
  } else {
    gainDBL = panelMixers->slVolL[stripIdx]->GetValue();
    gainDBR = panelMixers->slVolR[stripIdx]->GetValue();
    muteL = panelMixers->ckMuteL[stripIdx]->GetValue();
    muteR = panelMixers->ckMuteR[stripIdx]->GetValue();
    soloL = panelMixers->ckSoloL[stripIdx]->GetValue();
    soloR = panelMixers->ckSoloR[stripIdx]->GetValue();
    phaseL = panelMixers->ckPhase[stripIdx * 2]->GetValue();
    phaseR = panelMixers->ckPhase[stripIdx * 2 + 1]->GetValue();
  }
  panelMixers->lbVolVal[stripIdx]->setText(
      dbPair(muteL || gainDBL <= -60, gainDBL, muteR || gainDBR <= -60, gainDBR));

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

void TPMixer::OnOutputVolume(int32_t id) {
  int32_t ch = id & 0x0f;
  if (panelOutputs->btnLink[ch]->GetValue()) {
    int32_t val = panelOutputs->slOutput[ch]->GetValue();
    panelOutputs->lbOutVolVal[ch]->setText(dbPair(val <= -60, val, val <= -60, val));
  } else {
    int32_t valL = panelOutputs->slOutputL[ch]->GetValue();
    int32_t valR = panelOutputs->slOutputR[ch]->GetValue();
    panelOutputs->lbOutVolVal[ch]->setText(dbPair(valL <= -60, valL, valR <= -60, valR));
  }
  setOutputVol(ch);
}

void TPMixer::OnOutputToggle(int32_t id, int32_t val) {
  int32_t ch = id & 0x0f;
  switch (id & (~0xf)) {
  case ID_OUTPUT_SEL:
    hid->setOutputSel(ch, val + 1);
    break;
  case ID_OUTPUT_MON:
    if (ch == 0) {
      hid->setOutputMon(0, val);
      hid->settings[0x3701] = val;
    } else if (ch == 1) {
      hid->setOutputMon(1, val);
      hid->settings[0x3702] = val;
    } else {
      setOutputVol(ch);
    }
    break;
  case ID_OUTPUT_LINE:
    if (ch == 0) {
      hid->setOutputLine(0, val);
      hid->settings[0x3703] = val;
    } else if (ch == 1) {
      if (hid->pid == 0x8756) {
        hid->setDeviceSetting(0x37, 0x05, val);
        hid->settings[0x3705] = val;
      } else {
        hid->setOutputLine(1, val);
        hid->settings[0x3704] = val;
      }
    }
    break;
  }
}

void TPMixer::OnLoopVolume(int32_t id) {
  int32_t ch = id & 0x0f;
  if (panelLoopbacks->btnLink[ch]->GetValue()) {
    int32_t val = panelLoopbacks->slOutput[ch]->GetValue();
    bool muted = panelLoopbacks->ckMute[ch]->GetValue() || val <= -60;
    panelLoopbacks->lbLoopVolVal[ch]->setText(dbPair(muted, val, muted, val));
  } else {
    int32_t valL = panelLoopbacks->slOutputL[ch]->GetValue();
    int32_t valR = panelLoopbacks->slOutputR[ch]->GetValue();
    panelLoopbacks->lbLoopVolVal[ch]->setText(
        dbPair(panelLoopbacks->ckMuteL[ch]->GetValue() || valL <= -60, valL,
               panelLoopbacks->ckMuteR[ch]->GetValue() || valR <= -60, valR));
  }
  setLoopVol(ch);
}

void TPMixer::OnLoopToggle(int32_t id, int32_t val) {
  int32_t ch = id & 0x0f;
  if ((id & (~0xf)) == ID_LOOP_SEL) {
    hid->setLoopSel(ch, val + 1);
  } else {
    setLoopVol(ch);
  }
}

void TPMixer::OnPhoneMix(int32_t val) {
  int idx = panelOutputs->choicePhoneOut->currentIndex();
  if (idx < 0)
    return;
  panelOutputs->m_phoneMix[idx] = val;
  panelOutputs->updatePhoneMixLabel(val);
  hid->setPhoneMix(idx, val);
}

void TPMixer::OnPhoneGain(int32_t val) {
  int idx = panelOutputs->choicePhoneOut->currentIndex();
  if (idx < 0)
    return;
  panelOutputs->m_phoneGain[idx] = val ? true : false;
  hid->setPhoneGainBoost(idx, val);
}

void TPMixer::closeEvent(QCloseEvent *event) {
  if (!closed) {
    closed = true;
    stopHidReader();
    bool autoSave =
        hid->settings.contains(0x9001) ? (hid->settings[0x9001] != 0) : true;
    if (autoSave) {
      saveSettings();
    }
  }
  QWidget::closeEvent(event);
}

void TPMixer::OnLoad() {
  DownloadDialog dlg(this);
  ScaleUIElements(&dlg, g_uiScale);
  if (dlg.exec() != QDialog::Accepted)
    return;
  if (NULL == hid->getHandle()) {
    QMessageBox::critical(this, "Download Error",
                          "Failed to download to device: Device is not connected.");
    return;
  }
  if (dlg.rbOption1->isChecked()) {
    // Option 1: Save current workspace and download to device
    saveSettings();
  }
  hid->saveDeviceDefault();
  QMessageBox::information(this, "Download Success",
                           "Settings successfully downloaded to device!");
}

void TPMixer::OnSave() {
  saveSettings();
  QMessageBox::information(this, "Save Success", "Workspace saved successfully!");
}

void TPMixer::OnResetDefaults() {
  refreshInputsUi();
  refreshMixerUi(-1);
  refreshLoopbackUi();
  refreshOutputUi();
  if (NULL != hid->getHandle()) {
    pushGuiStateToDevice();
  }
  saveSettings();
}

void TPMixer::OnInputPeak(int32_t id) {
  uint32_t ch = id - ID_INPUT_PEAK;
  if (ch < panelInputs->N_INPUTS) {
    panelInputs->PeaksI[ch] = panelInputs->LEVEL_MIN;
    if (panelInputs->lbPeaksI[ch]) {
      panelInputs->lbPeaksI[ch]->SetLabel("-120.0");
    }
  }
}

void TPMixer::OnLinkToggle(int32_t id) {
  int32_t cls = id & (~0xf);
  int32_t ch = id & 0x0f;

  if (cls == ID_INPUT_LINK) {
    PanelInputs *p = panelInputs;
    bool linked = p->btnLink->GetValue();
    hid->settings[0x9000] = linked ? 1 : 0;
    if (linked) {
      p->slGainICombined->SetValue(p->slGainI[2]->GetValue());
      p->cbMute[2]->SetValue(p->cbMuteL->GetValue());
      p->cbSolo[2]->SetValue(p->cbSoloL->GetValue());
      p->cbPhase[2]->SetValue(p->cbPhaseL->GetValue());
    } else {
      int32_t combVal = p->slGainICombined->GetValue();
      p->slGainI[2]->SetValue(combVal);
      p->slGainI[3]->SetValue(combVal);
      p->cbMuteL->SetValue(p->cbMute[2]->GetValue());
      p->cbMuteR->SetValue(p->cbMute[2]->GetValue());
      p->cbSoloL->SetValue(p->cbSolo[2]->GetValue());
      p->cbSoloR->SetValue(p->cbSolo[2]->GetValue());
      p->cbPhaseL->SetValue(p->cbPhase[2]->GetValue());
      p->cbPhaseR->SetValue(p->cbPhase[2]->GetValue());
    }
    p->slGainICombined->setVisible(linked);
    p->slGainI[2]->setVisible(!linked);
    p->slGainI[3]->setVisible(!linked);
    p->combBtns->setVisible(linked);
    p->splitBtns->setVisible(!linked);
    sendInput(2, true);
  } else if (cls == ID_MIX_LINK) {
    PanelMixers *p = panelMixers;
    bool linked = p->btnLink[ch]->GetValue();
    hid->settings[0x9100 + ch] = linked ? 1 : 0;
    if (linked) {
      p->slVol[ch]->SetValue(p->slVolL[ch]->GetValue());
      p->ckMute[ch]->SetValue(p->ckMuteL[ch]->GetValue());
      p->ckSolo[ch]->SetValue(p->ckSoloL[ch]->GetValue());
      p->ckPhaseCombined[ch]->SetValue(p->ckPhase[ch * 2]->GetValue());
    } else {
      int32_t combVal = p->slVol[ch]->GetValue();
      p->slVolL[ch]->SetValue(combVal);
      p->slVolR[ch]->SetValue(combVal);
      p->ckMuteL[ch]->SetValue(p->ckMute[ch]->GetValue());
      p->ckMuteR[ch]->SetValue(p->ckMute[ch]->GetValue());
      p->ckSoloL[ch]->SetValue(p->ckSolo[ch]->GetValue());
      p->ckSoloR[ch]->SetValue(p->ckSolo[ch]->GetValue());
      p->ckPhase[ch * 2]->SetValue(p->ckPhaseCombined[ch]->GetValue());
      p->ckPhase[ch * 2 + 1]->SetValue(p->ckPhaseCombined[ch]->GetValue());
    }
    p->slVol[ch]->setVisible(linked);
    p->slVolL[ch]->setVisible(!linked);
    p->slVolR[ch]->setVisible(!linked);
    p->combBtns[ch]->setVisible(linked);
    p->splitBtns[ch]->setVisible(!linked);
    OnMixVolumeChanged(ch);
  } else if (cls == ID_LOOP_LINK) {
    PanelLoopbacks *p = panelLoopbacks;
    bool linked = p->btnLink[ch]->GetValue();
    hid->settings[0x9200 + ch] = linked ? 1 : 0;
    if (linked) {
      p->slOutput[ch]->SetValue(p->slOutputL[ch]->GetValue());
      p->ckMute[ch]->SetValue(p->ckMuteL[ch]->GetValue());
    } else {
      p->slOutputL[ch]->SetValue(p->slOutput[ch]->GetValue());
      p->slOutputR[ch]->SetValue(p->slOutput[ch]->GetValue());
      p->ckMuteL[ch]->SetValue(p->ckMute[ch]->GetValue());
      p->ckMuteR[ch]->SetValue(p->ckMute[ch]->GetValue());
    }
    p->slOutput[ch]->setVisible(linked);
    p->slOutputL[ch]->setVisible(!linked);
    p->slOutputR[ch]->setVisible(!linked);
    p->combBtns[ch]->setVisible(linked);
    p->splitBtns[ch]->setVisible(!linked);
    setLoopVol(ch);
  } else if (cls == ID_OUTPUT_LINK) {
    PanelOutputs *p = panelOutputs;
    bool linked = p->btnLink[ch]->GetValue();
    hid->settings[0x9300 + ch] = linked ? 1 : 0;
    if (linked) {
      p->slOutput[ch]->SetValue(p->slOutputL[ch]->GetValue());
      if (ch > 0)
        p->ckMute[ch]->SetValue(p->ckMuteL[ch]->GetValue());
    } else {
      p->slOutputL[ch]->SetValue(p->slOutput[ch]->GetValue());
      p->slOutputR[ch]->SetValue(p->slOutput[ch]->GetValue());
      if (ch > 0) {
        p->ckMuteL[ch]->SetValue(p->ckMute[ch]->GetValue());
        p->ckMuteR[ch]->SetValue(p->ckMute[ch]->GetValue());
      }
    }
    p->slOutput[ch]->setVisible(linked);
    p->slOutputL[ch]->setVisible(!linked);
    p->slOutputR[ch]->setVisible(!linked);
    if (ch > 0) {
      p->combBtns[ch]->setVisible(linked);
      p->splitBtns[ch]->setVisible(!linked);
    }
    setOutputVol(ch);
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  QApplication app(argc, argv);
  app.setStyle(QStyleFactory::create("Fusion"));
  QPalette pal;
  pal.setColor(QPalette::Window, QColor(30, 30, 30));
  pal.setColor(QPalette::WindowText, QColor(220, 220, 220));
  pal.setColor(QPalette::Base, QColor(25, 25, 25));
  pal.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
  pal.setColor(QPalette::Text, QColor(220, 220, 220));
  pal.setColor(QPalette::Button, QColor(45, 45, 45));
  pal.setColor(QPalette::ButtonText, QColor(220, 220, 220));
  pal.setColor(QPalette::Highlight, COL_BLUE);
  pal.setColor(QPalette::HighlightedText, Qt::white);
  pal.setColor(QPalette::ToolTipBase, QColor(45, 45, 45));
  pal.setColor(QPalette::ToolTipText, QColor(220, 220, 220));
  app.setPalette(pal);
  app.setStyleSheet(R"(
    #stripPanel, #colPanel, #monMixPanel { background: #161616; border-radius: 4px; }
    #titleBox { background: #343434; color: #b9b9b9; border-radius: 4px; padding: 4px 2px; }
    QComboBox QAbstractItemView { background: #262626; color: #e0e0e0;
                                  selection-background-color: #3c5caa; border: 1px solid #3a3a3a; }
  )");

  int ret;
  {
    TPMixer frame;
    frame.show();
    ret = app.exec();
  }
  hid_exit();
  return ret;
}
