#include "FaceHud.h"

#include <M5Unified.h>

namespace {

const lgfx::IFont* const kJpFont = &fonts::lgfxJapanGothicP_16;
const lgfx::IFont* const kSmallFont = &fonts::lgfxJapanGothicP_12;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kBottomTop = 198;            // 下段（字幕・ボタン）の開始 Y
constexpr uint32_t kCaptionPageMs = 3200;  // 字幕が3行以上のときのページ送り間隔

// UTF-8 の先頭バイトから1文字のバイト数を求める
size_t utf8Length(uint8_t lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

// 行頭に来ると読みにくい約物（ぶら下げて前の行に残す）
bool isClosingPunctuation(const char* glyph) {
  static const char* const kMarks[] = {"、", "。", "！", "？", "」", "）",
                                       "…", "ー", ",", ".", "!", "?"};
  for (const char* mark : kMarks) {
    if (strcmp(glyph, mark) == 0) return true;
  }
  return false;
}

void drawMicIcon(M5Canvas* c, int cx, int cy, uint16_t color) {
  c->fillRoundRect(cx - 3, cy - 9, 7, 12, 3, color);
  c->drawArc(cx, cy - 2, 7, 6, 0, 180, color);
  c->drawFastVLine(cx, cy + 5, 4, color);
  c->drawFastHLine(cx - 4, cy + 9, 9, color);
}

void drawCameraIcon(M5Canvas* c, int cx, int cy, uint16_t color, uint16_t hole) {
  c->fillRect(cx - 3, cy - 7, 6, 3, color);
  c->fillRoundRect(cx - 9, cy - 5, 18, 13, 2, color);
  c->fillCircle(cx, cy + 1, 3, hole);
}

}  // namespace

FaceHud::FaceHud() {
  mux_ = portMUX_INITIALIZER_UNLOCKED;
}

void FaceHud::set(const HudData& data) {
  portENTER_CRITICAL(&mux_);
  data_ = data;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::setCaption(const String& text, uint32_t durationMs) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  strncpy(caption_, text.c_str(), sizeof(caption_) - 1);
  caption_[sizeof(caption_) - 1] = '\0';
  // 途中で切れた UTF-8 の断片を取り除く
  size_t len = strlen(caption_);
  size_t i = len;
  while (i > 0 && (static_cast<uint8_t>(caption_[i - 1]) & 0xC0) == 0x80) --i;
  if (i > 0 && utf8Length(static_cast<uint8_t>(caption_[i - 1])) > len - i + 1) {
    caption_[i - 1] = '\0';
  }
  captionSince_ = now;
  captionUntil_ = durationMs == 0 ? 0 : now + durationMs;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::clearCaption() {
  portENTER_CRITICAL(&mux_);
  caption_[0] = '\0';
  captionUntil_ = 0;
  portEXIT_CRITICAL(&mux_);
}

bool FaceHud::hasCaption() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  const bool live = caption_[0] != '\0' &&
      (captionUntil_ == 0 || static_cast<int32_t>(now - captionUntil_) < 0);
  portEXIT_CRITICAL(&mux_);
  return live;
}

void FaceHud::pushMeter(uint8_t level) {
  portENTER_CRITICAL(&mux_);
  meter_[meterHead_] = level;
  meterHead_ = (meterHead_ + 1) % kMeterBars;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::setToast(const char* text, uint32_t durationMs) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  strncpy(toast_, text ? text : "", sizeof(toast_) - 1);
  toast_[sizeof(toast_) - 1] = '\0';
  toastUntil_ = durationMs == 0 ? 0 : now + durationMs;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::draw(M5Canvas* canvas, int colorDepth, uint16_t fg,
                   uint16_t bg) {
  (void)colorDepth;
  HudData d;
  uint8_t meter[kMeterBars];
  int meterHead;
  char caption[sizeof(caption_)];
  char toast[sizeof(toast_)];
  uint32_t since = 0;
  const uint32_t now = millis();

  portENTER_CRITICAL(&mux_);
  d = data_;
  memcpy(meter, meter_, sizeof(meter));
  meterHead = meterHead_;
  const bool captionLive =
      caption_[0] != '\0' &&
      (captionUntil_ == 0 || static_cast<int32_t>(now - captionUntil_) < 0);
  if (captionLive) {
    memcpy(caption, caption_, sizeof(caption));
  } else {
    caption[0] = '\0';
  }
  since = captionSince_;
  const bool toastLive =
      toast_[0] != '\0' &&
      (toastUntil_ == 0 || static_cast<int32_t>(now - toastUntil_) < 0);
  if (toastLive) {
    memcpy(toast, toast_, sizeof(toast));
  } else {
    toast[0] = '\0';
  }
  portEXIT_CRITICAL(&mux_);

  if (!d.visible && caption[0] == '\0') return;

  canvas->setTextSize(1);
  canvas->setTextWrap(false);
  if (d.visible) drawTopBar(canvas, d, fg, bg, now);

  canvas->setFont(kJpFont);
  if (caption[0] != '\0') {
    canvas->fillRect(0, kBottomTop, kWidth, kHeight - kBottomTop, bg);
    canvas->drawFastHLine(8, kBottomTop, kWidth - 16, fg);
    if (since != cachedSince_ || strcmp(caption, cachedCaption_) != 0) {
      cachedSince_ = since;
      cachedLines_ = drawWrapped(canvas, caption, 8, 0, kWidth - 16, 18, 0, 0, fg, bg);
    }
    int page = 0;
    const int pages = (cachedLines_ + 1) / 2;
    if (pages > 1) {
      page = static_cast<int>(((now - since) / kCaptionPageMs) % pages);
    }
    drawWrapped(canvas, caption, 8, kBottomTop + 4, kWidth - 16, 18, page * 2, 2,
                fg, bg);
  } else if (d.meter) {
    drawMeter(canvas, meter, meterHead, fg);
  } else if (d.buttons) {
    drawButtons(canvas, fg, bg);
  }

  if (toast[0] != '\0' && caption[0] == '\0') {
    const int w = canvas->textWidth(toast) + 24;
    const int x = (kWidth - w) / 2;
    canvas->fillRoundRect(x, 168, w, 24, 8, fg);
    canvas->setTextColor(bg, fg);
    canvas->setTextDatum(middle_center);
    canvas->drawString(toast, kWidth / 2, 181);
  }
}

void FaceHud::drawTopBar(M5Canvas* c, const HudData& d, uint16_t fg,
                         uint16_t bg, uint32_t now) {
  constexpr int chipX = 4;
  constexpr int chipY = 3;
  constexpr int chipH = 21;

  c->setFont(kJpFont);
  if (d.chip[0] != '\0') {
    const int chipW = c->textWidth(d.chip) + 16;
    const bool blinkOff = d.chipPulse && ((now / 450) % 2 == 1);
    if (blinkOff) {
      c->fillRoundRect(chipX, chipY, chipW, chipH, 7, bg);
      c->drawRoundRect(chipX, chipY, chipW, chipH, 7, fg);
      c->setTextColor(fg, bg);
    } else {
      c->fillRoundRect(chipX, chipY, chipW, chipH, 7, fg);
      c->setTextColor(bg, fg);
    }
    c->setTextDatum(middle_center);
    c->drawString(d.chip, chipX + chipW / 2, chipY + chipH / 2 + 1);
    if (d.photo) drawCameraIcon(c, chipX + chipW + 16, chipY + chipH / 2, fg, bg);
  }

  int right = kWidth - 5;
  if (d.wifiBars >= 0) {
    for (int bar = 0; bar < 3; ++bar) {
      const int h = 5 + bar * 4;
      const int x = right - 16 + bar * 6;
      const int y = 19 - h;
      if (bar < d.wifiBars) {
        c->fillRect(x, y, 4, h, fg);
      } else {
        c->drawRect(x, y, 4, h, fg);
      }
    }
    right -= 22;
  }
  if (d.label[0] != '\0') {
    c->setFont(kSmallFont);
    c->setTextColor(fg, bg);
    c->setTextDatum(middle_right);
    c->drawString(d.label, right, chipY + chipH / 2);
  }
}

void FaceHud::drawButtons(M5Canvas* c, uint16_t fg, uint16_t bg) {
  const HudRect talk = kHudTalkButton;
  c->fillRoundRect(talk.x, talk.y, talk.w, talk.h, 10, fg);
  drawMicIcon(c, talk.x + talk.w / 2 - 30, talk.y + talk.h / 2, bg);
  c->setTextColor(bg, fg);
  c->setTextDatum(middle_center);
  c->drawString("話す", talk.x + talk.w / 2 + 12, talk.y + talk.h / 2 + 1);

  const HudRect camera = kHudCameraButton;
  c->fillRoundRect(camera.x, camera.y, camera.w, camera.h, 10, bg);
  c->drawRoundRect(camera.x, camera.y, camera.w, camera.h, 10, fg);
  drawCameraIcon(c, camera.x + 21, camera.y + camera.h / 2, fg, bg);
  c->setTextColor(fg, bg);
  c->drawString("見せて", camera.x + 66, camera.y + camera.h / 2 + 1);
}

void FaceHud::drawMeter(M5Canvas* c, const uint8_t* bars, int head, uint16_t fg) {
  constexpr int kBarWidth = 9;
  constexpr int kPitch = (kWidth - 16) / kMeterBars;
  constexpr int kCenter = 218;
  constexpr int kMaxHalf = 18;
  for (int i = 0; i < kMeterBars; ++i) {
    const uint8_t level = bars[(head + i) % kMeterBars];
    const int half = 1 + level * kMaxHalf / 100;
    c->fillRect(8 + i * kPitch + (kPitch - kBarWidth) / 2, kCenter - half, kBarWidth,
                half * 2, fg);
  }
}

int FaceHud::drawWrapped(M5Canvas* c, const char* text, int x, int y,
                         int maxWidth, int lineHeight, int skipLines,
                         int lines, uint16_t fg, uint16_t bg) {
  c->setTextColor(fg, bg);
  c->setTextDatum(top_left);
  if (lines == 0) {
    // Lay out once per caption. Rendering subsequent frames needs only two
    // complete drawString calls, with no per-glyph width measurements.
    strncpy(cachedCaption_, text, sizeof(cachedCaption_) - 1);
    cachedCaption_[sizeof(cachedCaption_) - 1] = '\0';
    int line = 0;
    int width = 0;
    lineStarts_[0] = 0;
    size_t pos = 0;
    while (cachedCaption_[pos]) {
      char glyph[5] = {};
      size_t n = 0;
      const size_t len = utf8Length(static_cast<uint8_t>(cachedCaption_[pos]));
      while (n < len && cachedCaption_[pos + n]) {
        glyph[n] = cachedCaption_[pos + n];
        ++n;
      }
      if (glyph[0] == '\n') {
        lineStarts_[++line] = pos + n;
        width = 0;
      } else {
        const int w = c->textWidth(glyph);
        if (width + w > maxWidth && width > 0 && !isClosingPunctuation(glyph)) {
          lineStarts_[++line] = pos;
          width = 0;
          if (glyph[0] == ' ') {
            lineStarts_[line] = pos + n;
            pos += n;
            continue;
          }
        }
        width += w;
      }
      pos += n;
    }
    lineStarts_[line + 1] = pos;
    cachedLines_ = line + 1;
    return cachedLines_;
  }
  char lineText[sizeof(cachedCaption_)];
  for (int i = skipLines; i < cachedLines_ && i < skipLines + lines; ++i) {
    const size_t begin = lineStarts_[i];
    size_t end = lineStarts_[i + 1];
    while (end > begin && (cachedCaption_[end - 1] == '\n' ||
                          cachedCaption_[end - 1] == '\r' ||
                          cachedCaption_[end - 1] == ' ')) --end;
    memcpy(lineText, cachedCaption_ + begin, end - begin);
    lineText[end - begin] = '\0';
    c->drawString(lineText, x, y + (i - skipLines) * lineHeight);
  }
  return cachedLines_;
}

void HudMouth::draw(M5Canvas* spi, m5avatar::BoundingRect rect,
                    m5avatar::DrawContext* ctx) {
  inner_->draw(spi, rect, ctx);
  const int depth = ctx->getColorDepth();
  const uint16_t fg =
      depth == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
  const uint16_t bg =
      depth == 1 ? 0 : ctx->getColorPalette()->get(COLOR_BACKGROUND);
  hud_->draw(spi, depth, fg, bg);
}
