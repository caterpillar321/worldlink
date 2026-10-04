#pragma once

// SEKAI_A11Y_MONITOR: 화면 읽기(Orca)용 키보드 감시 — GNOME 48 의 org.freedesktop.a11y.KeyboardMonitor 규약의 합성기 몫.
//   D-Bus 쪽(이름·클라이언트·KeyEvent 신호)은 셸의 sekai-a11yd 가 맡고, 합성기는 키마다 가로챌지 판정하고
//   이벤트를 인스턴스 폴더의 .sekaia11y.sock 으로 흘린다 (정책은 셸에 — 합성기는 덜 고치게).
//
//   데몬 → 합성기 (줄 단위):  grab 0|1 · watch 0|1 · mods <keysym>… · strokes <keysym>:<수식 마스크>…
//   합성기 → 데몬:            k <뗌 0|1> <수식 마스크> <keysym> <유니코드> <키코드(evdev+8)> <가로챔 0|1>
//
//   연결이 끊기면 가로채기를 모두 푼다 (데몬이 죽어도 키보드가 잠기지 않게).

#include <cstdint>
#include <optional>
#include "../../devices/IKeyboard.hpp"
#include "../../helpers/math/Math.hpp"

namespace SekaiA11y {
    void start();

    // 이 키 이벤트를 앱·단축키에 넘기지 않고 가로채면 true
    bool onKey(const IKeyboard::SKeyEvent& event, SP<IKeyboard> kb);

    // 가로챈 키는 xkb 상태(Caps Lock 등)도 바꾸지 않는다 — Keyboard.cpp 가 키 이벤트를 보낸 직후 묻는다
    extern std::optional<uint32_t> g_xkbSkip;

    // SEKAI_ZOOM_FOCUS: 돋보기 중심을 키보드 포커스·글자 커서로 (셸이 hyprctl dispatch sekaizoomfocus x y 로 — 전체 화면 좌표).
    //   마우스를 움직이면 다시 마우스를 따라간다
    extern std::optional<Vector2D> g_zoomFocus;
}
