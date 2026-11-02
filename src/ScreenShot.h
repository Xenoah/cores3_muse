#pragma once

class AvatarFaceController;

// LCD の実画面を読み出して USB シリアルへ送る。tools/capture_screen.py が PNG にする。
// 形式: "#SHOT w h" に続いて 1 行ずつ "#L y sum base64(RGB888)"、最後に "#END"
void dumpScreen(AvatarFaceController& face);
