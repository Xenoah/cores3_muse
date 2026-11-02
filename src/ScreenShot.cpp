#include "ScreenShot.h"

#include <M5Unified.h>
#include <mbedtls/base64.h>

#include "AvatarFaceController.h"

void dumpScreen(AvatarFaceController& face) {
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const size_t lineBytes = width * 3;
  const size_t textBytes = lineBytes / 3 * 4 + 8;
  auto* line = static_cast<uint8_t*>(heap_caps_malloc(lineBytes, MALLOC_CAP_DMA));
  auto* text = static_cast<uint8_t*>(malloc(textBytes));
  if (line == nullptr || text == nullptr) {
    Serial.println("#SHOT-ERROR no memory");
    heap_caps_free(line);
    free(text);
    return;
  }

  // 描画タスクと SPI を取り合わないよう、描きかけのフレームを待ってから読む
  const bool wasPaused = face.isDrawingPaused();
  if (!wasPaused) face.pauseDrawing();
  M5.Display.waitDisplay();
  Serial.printf("\n#SHOT %d %d\n", width, height);
  for (int y = 0; y < height; ++y) {
    M5.Display.readRectRGB(0, y, width, 1, line);
    uint32_t sum = 0;
    for (size_t i = 0; i < lineBytes; ++i) sum += line[i];
    size_t written = 0;
    mbedtls_base64_encode(text, textBytes, &written, line, lineBytes);
    text[written] = '\0';
    Serial.printf("#L %d %u %s\n", y, static_cast<unsigned>(sum), text);
  }
  Serial.println("#END");
  Serial.flush();
  if (!wasPaused) face.resumeDrawing();

  heap_caps_free(line);
  free(text);
}
