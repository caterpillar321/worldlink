#include "barDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/managers/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <pango/pangocairo.h>

#include "globals.hpp"
#include <hyprland/src/managers/EventManager.hpp>

// ── SEKAI_DIALOG_BUTTONS: 대화상자에는 닫기 단추만 ─────────────────
//    SEKAI_FIXED_SIZE: 크기를 바꿀 수 없는 창에는 최대화 단추가 없다 (최소화·닫기만)
static bool sekaiDialogSkip(const PHLWINDOW& w, const std::string& icon) {
    if (!w)
        return false;
    return ((icon == "sekai:min" || icon == "sekai:max") && w->parent()) || (icon == "sekai:max" && w->sekaiFixedSize());
}

// ── SEKAI_SNAP: 끌어서 스냅 — 영역 판단은 셸이 (합성기의 sekaidrag 이벤트, SEKAI_DRAG_IPC) ─

#include "BarPassElement.hpp"

CHyprBar::CHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow = pWindow;

    static auto* const PCOLOR = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_color")->getDataStaticPtr();

    const auto         PMONITOR = pWindow->m_monitor.lock();
    PMONITOR->m_scheduledRecalc = true;

    //button events
    m_pMouseButtonCallback = HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "mouseButton", [&](void* self, SCallbackInfo& info, std::any param) { onMouseButton(info, std::any_cast<IPointer::SButtonEvent>(param)); });
    m_pTouchDownCallback = HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "touchDown", [&](void* self, SCallbackInfo& info, std::any param) { onTouchDown(info, std::any_cast<ITouch::SDownEvent>(param)); });
    m_pTouchUpCallback = HyprlandAPI::registerCallbackDynamic( //
        PHANDLE, "touchUp", [&](void* self, SCallbackInfo& info, std::any param) { handleUpEvent(info); });

    //move events
    m_pTouchMoveCallback = HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "touchMove", [&](void* self, SCallbackInfo& info, std::any param) { onTouchMove(info, std::any_cast<ITouch::SMotionEvent>(param)); });
    m_pMouseMoveCallback = HyprlandAPI::registerCallbackDynamic( //
        PHANDLE, "mouseMove", [&](void* self, SCallbackInfo& info, std::any param) { onMouseMove(std::any_cast<Vector2D>(param)); });

    m_pTextTex    = makeShared<CTexture>();
    m_pButtonsTex = makeShared<CTexture>();

    g_pAnimationManager->createAnimation(CHyprColor{**PCOLOR}, m_cRealBarColor, g_pConfigManager->getAnimationPropertyConfig("border"), pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });
}

CHyprBar::~CHyprBar() {
    HyprlandAPI::unregisterCallback(PHANDLE, m_pMouseButtonCallback);
    HyprlandAPI::unregisterCallback(PHANDLE, m_pTouchDownCallback);
    HyprlandAPI::unregisterCallback(PHANDLE, m_pTouchUpCallback);
    HyprlandAPI::unregisterCallback(PHANDLE, m_pTouchMoveCallback);
    HyprlandAPI::unregisterCallback(PHANDLE, m_pMouseMoveCallback);
    std::erase(g_pGlobalState->bars, m_self);
}

SDecorationPositioningInfo CHyprBar::getPositioningInfo() {
    static auto* const         PHEIGHT     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
    static auto* const         PENABLED    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:enabled")->getDataStaticPtr();
    static auto* const         PPRECEDENCE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_precedence_over_border")->getDataStaticPtr();

    SDecorationPositioningInfo info;
    info.policy         = m_hidden ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
    info.edges          = DECORATION_EDGE_TOP;
    info.priority       = **PPRECEDENCE ? 10005 : 5000;
    info.reserved       = true;
    info.desiredExtents = {{0, m_hidden || !**PENABLED ? 0 : **PHEIGHT}, {0, 0}};
    return info;
}

void CHyprBar::onPositioningReply(const SDecorationPositioningReply& reply) {
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
        m_bWindowSizeChanged = true;

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CHyprBar::getDisplayName() {
    return "Hyprbar";
}

bool CHyprBar::inputIsValid() {
    static auto* const PENABLED = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:enabled")->getDataStaticPtr();

    if (!**PENABLED)
        return false;

    // SEKAI_LOCK_BARS: 잠긴 동안 막대는 아무것도 하지 않는다 — 잠금 화면에서 보이지 않는 닫기·최소화 단추 자리를
    //   눌러도 창이 닫혔다 (화면이 새지는 않아도 잠금 너머로 작업을 망가뜨릴 수 있었다)
    if (g_pSessionLockManager->isSessionLocked())
        return false;

    if (!m_pWindow->m_workspace || !m_pWindow->m_workspace->isVisible() || !g_pInputManager->m_exclusiveLSes.empty() ||
        (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->m_wlSurface->resource())))
        return false;

    const auto WINDOWATCURSOR = g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(), RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != g_pCompositor->m_lastWindow)
        return false;

    // check if input is on top or overlay shell layers
    auto     PMONITOR     = g_pCompositor->m_lastMonitor.lock();
    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    // check top layer
    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);

    if (foundSurface)
        return false;
    // check overlay layer
    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords,
                                        &foundSurface);

    if (foundSurface)
        return false;

    return true;
}

// SEKAI_BORDER_EDGE2: 창(제목줄·테두리 포함) 변 중 화면 끝(모니터 끝·작업 표시줄 끝)에 붙은 변 — 1 왼 · 2 오 · 4 위.
//   Hyprland 의 sekaiScreenEdges(patch-hyprland-bordergrab.py)와 같은 상자·기준
static int sekaiBarScreenEdges(PHLWINDOW w) {
    const auto M = w ? w->m_monitor.lock() : nullptr;
    if (!M)
        return 0;
    const auto   EXT = w->getFullWindowReservedArea();
    const auto   POS = w->m_realPosition->value(), SIZE = w->m_realSize->value();
    const double L = M->m_position.x + M->m_reservedTopLeft.x, T = M->m_position.y + M->m_reservedTopLeft.y;
    const double R = M->m_position.x + M->m_size.x - M->m_reservedBottomRight.x;
    return (POS.x - EXT.topLeft.x <= L + 1 ? 1 : 0) | (POS.x + SIZE.x + EXT.bottomRight.x >= R - 1 ? 2 : 0) | (POS.y - EXT.topLeft.y <= T + 1 ? 4 : 0);
}

// SEKAI_BUTTON_SLOT: 단추 칸 — 아이콘(바 좌표 iconX = currentPos.x + 간격) ± 간격의 반, 제목줄 높이 전체.
//   맨 위 4px 이 창 테두리로 넘어가는 창(크기 조절이 되고 화면 맨 위에 붙지 않은 창)이면 그 4px 은 뺀다 (onMouseButton 과 같은 판정)
static bool sekaiInButton(PHLWINDOW w, const Vector2D& c, const Vector2D& currentPos, double size, double pad, double barH) {
    static auto* const PSEKAIRESIZE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "general:resize_on_border")->getDataStaticPtr();
    const bool         TOPBORDER    = **PSEKAIRESIZE && w && !w->isFullscreen() && !w->m_sekaiMaximized && !w->isX11OverrideRedirect() && !(sekaiBarScreenEdges(w) & 4);
    const double       X0 = currentPos.x + pad / 2.0, X1 = currentPos.x + pad + size + pad / 2.0;
    return c.x >= X0 && c.x < X1 && c.y >= (TOPBORDER ? 4 : 0) && c.y < barH;
}

void CHyprBar::onMouseButton(SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED && (m_bDraggingThis || m_iSekaiArmed >= 0)) { // SEKAI_SNAP_DROP · SEKAI_BUTTON_RELEASE
        handleUpEvent(info);
        return;
    }

    if (!inputIsValid())
        return;

    // SEKAI_MODAL: 모달 대화상자가 떠 있는 창의 막대는 아무것도 하지 않는다 — 합성기가 대화상자를 앞으로 (윈도우처럼)
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
        if (const auto SEKAI_PW = m_pWindow.lock(); SEKAI_PW && SEKAI_PW->sekaiModalChild())
            return;

    // SEKAI_WINDOW_MENU: 막대를 오른쪽 클릭 → 셸(패널)이 윈도우식 창 메뉴를 띄운다 (단추 위는 아무것도 안 함).
    //   inputIsValid 는 커서가 막대 위인지 보지 않는다 — 막대 밖(창 본문)의 오른쪽 클릭은 앱 몫이니 꼭 막대 안에서만
    if (e.button == 0x111 /* BTN_RIGHT */) {
        static auto* const PSEKAIH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            const auto RC = cursorRelativeToBar();
            if (!m_hidden && VECINRECT(RC, 0, 0, assignedBoxGlobal().w, **PSEKAIH - 1)) {
                if (sekaiButtonAt(RC) < 0) {
                    const auto C = g_pInputManager->getMouseCoordsInternal();
                    g_pEventManager->postEvent(
                        SHyprIPCEvent{"sekaiwinmenu", std::format("{:x},{},{}", (uintptr_t)m_pWindow.lock().get(), (int)C.x, (int)C.y)});
                }
                m_bSekaiRightEaten = true;
                info.cancelled     = true;
                return;
            }
        } else if (m_bSekaiRightEaten) {
            m_bSekaiRightEaten = false;
            info.cancelled     = true;
            return;
        }
    }

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        handleUpEvent(info);
        return;
    }

    // SEKAI_BORDER_GRAB: 제목줄 가장자리 4px 은 창 테두리 — Hyprland 가 크기 조절하게 넘긴다
    //   SEKAI_BORDER_GRAB2: Hyprland 가 테두리 크기 조절을 하는 창일 때만 (최대화·전체 화면은 아니다)
    static auto* const PSEKAIRESIZE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "general:resize_on_border")->getDataStaticPtr();
    const auto         SEKAI_C      = cursorRelativeToBar();
    const auto         SEKAI_W      = m_pWindow.lock();
    if (**PSEKAIRESIZE && SEKAI_W && !SEKAI_W->isFullscreen() && !SEKAI_W->m_sekaiMaximized && !SEKAI_W->isX11OverrideRedirect()) { // SEKAI_MAXIMIZE2
        // SEKAI_BORDER_EDGE: 화면 끝(모니터 끝·작업 표시줄 끝)에 붙은 변은 넘기지 않는다 — 제목줄 몫
        const auto SEKAI_B = assignedBoxGlobal();
        const int  SEKAI_E = sekaiBarScreenEdges(SEKAI_W); // SEKAI_BORDER_EDGE2: Hyprland 와 같은 상자로
        // SEKAI_BUTTON_SLOT: 위쪽 4px 만 창 테두리 — 옆 테두리는 창 바깥 (Hyprland SEKAI_BORDER_TOPONLY 와 같게)
        (void)SEKAI_B;
        if (SEKAI_C.y < 4 && !(SEKAI_E & 4))
            return;
    }

    handleDownEvent(info, std::nullopt);
}

void CHyprBar::onTouchDown(SCallbackInfo& info, ITouch::SDownEvent e) {
    if (!inputIsValid())
        return;

    auto PMONITOR = g_pCompositor->getMonitorFromName(!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "");
    PMONITOR      = PMONITOR ? PMONITOR : g_pCompositor->m_lastMonitor.lock();
    g_pCompositor->warpCursorTo({PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y}, true);

    handleDownEvent(info, e);
}

void CHyprBar::onMouseMove(Vector2D coords) {
    // ensure proper redraws of button icons on hover when using hardware cursors
    static auto* const PICONONHOVER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:icon_on_hover")->getDataStaticPtr();
    (void)PICONONHOVER;
    damageOnButtonHover(); // SEKAI_BUTTON_HOVER — 배경 효과를 위해 언제나

    if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow))
        return;

    m_bDragPending = false;
    handleMovement();
}

void CHyprBar::onTouchMove(SCallbackInfo& info, ITouch::SMotionEvent e) {
    if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow))
        return;

    g_pInputManager->mouseMoveUnified(e.timeMs);
    handleMovement();
}

void CHyprBar::handleDownEvent(SCallbackInfo& info, std::optional<ITouch::SDownEvent> touchEvent) {
    m_bTouchEv       = touchEvent.has_value();
    m_bCancelledDown = false; // SEKAI_BAR_INPUT: 지난 누름의 표시가 남아 이번 뗌(본문 클릭)을 삼키지 않게
    // SEKAI_BAR_HIDDEN: 막대를 숨긴 창(nobar — 제목줄을 스스로 그리는 크롬·탐색기)의 누름은 앱 몫이다
    //   (전엔 화면 끝까지 넓힌 판정(EDGE2)이 최대화한 창의 탭 줄·창 단추를 가로챘다)
    if (m_hidden) {
        m_bDragPending = false;
        return;
    }

    const auto         PWINDOW = m_pWindow.lock();

    const auto         COORDS = cursorRelativeToBar();

    static auto* const PHEIGHT           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
    static auto* const PBARBUTTONPADDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_button_padding")->getDataStaticPtr();
    static auto* const PBARPADDING       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_padding")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    static auto* const PONDOUBLECLICK    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:on_double_click")->getDataStaticPtr();

    const bool         BUTTONSRIGHT    = std::string{*PALIGNBUTTONS} != "left";
    const std::string  ON_DOUBLE_CLICK = *PONDOUBLECLICK;

    // SEKAI_BORDER_EDGE2: 화면 끝에 붙은 변 쪽은 제목줄 바깥 테두리 픽셀도 제목줄 몫 (화면 맨 위·끝을 눌러도 끌기)
    double SEKAI_X0 = 0, SEKAI_Y0 = 0, SEKAI_X1 = assignedBoxGlobal().w;
    if (PWINDOW && !m_bDraggingThis) {
        const int  SE  = sekaiBarScreenEdges(PWINDOW);
        const auto BB  = assignedBoxGlobal();
        const auto EXT = PWINDOW->getFullWindowReservedArea();
        const auto POS = PWINDOW->m_realPosition->value(), SIZE = PWINDOW->m_realSize->value();
        if (SE & 1)
            SEKAI_X0 = std::min(0.0, (POS.x - EXT.topLeft.x) - BB.x);
        if (SE & 2)
            SEKAI_X1 = std::max(BB.w, (POS.x + SIZE.x + EXT.bottomRight.x) - BB.x);
        if (SE & 4)
            SEKAI_Y0 = std::min(0.0, (POS.y - EXT.topLeft.y) - BB.y);
    }
    if (!VECINRECT(COORDS, SEKAI_X0, SEKAI_Y0, SEKAI_X1, **PHEIGHT - 1)) {

        if (m_bDraggingThis) {
            if (m_bTouchEv) {
                ITouch::SDownEvent e = touchEvent.value();
                g_pCompositor->warpCursorTo(Vector2D(e.pos.x, e.pos.y));
                g_pInputManager->mouseMoveUnified(e.timeMs);
            }
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            Debug::log(LOG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        m_bTouchEv      = false;
        return;
    }

    // SEKAI_BAR_FOCUS: 마지막 창이어도 키보드가 레이어(바탕화면 등)에 가 있으면 다시 초점
    if (g_pCompositor->m_lastWindow.lock() != PWINDOW || g_pCompositor->getLayerSurfaceFromSurface(g_pSeatManager->m_state.keyboardFocus.lock()))
        g_pCompositor->focusWindow(PWINDOW);

    if (PWINDOW->m_isFloating)
        g_pCompositor->changeWindowZOrder(PWINDOW, true);

    info.cancelled   = true;
    m_bCancelledDown = true;

    if (doButtonPress(PBARPADDING, PBARBUTTONPADDING, PHEIGHT, COORDS, BUTTONSRIGHT))
        return;

    if (!ON_DOUBLE_CLICK.empty() && !(PWINDOW && (PWINDOW->parent() || PWINDOW->sekaiFixedSize())) /* SEKAI_DIALOG_BUTTONS · SEKAI_FIXED_SIZE */ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
        // SEKAI_MAXIMIZE2: 두 번 누르기 = 이 창의 최대화 켜고 끄기 (exec 로 hyprctl 을 띄우면 도착했을 때의 초점 창에 먹었다)
        if (PWINDOW && (ON_DOUBLE_CLICK.find("fullscreen 1") != std::string::npos || ON_DOUBLE_CLICK.find("sekaimaximize") != std::string::npos))
            g_pKeybindManager->m_dispatchers["sekaimaximize"](std::format("toggle,address:0x{:x}", (uintptr_t)PWINDOW.get()));
        else
            g_pKeybindManager->m_dispatchers["exec"](ON_DOUBLE_CLICK);
        m_bDragPending = false;
    } else {
        m_lastMouseDown = Time::steadyNow();
        m_bDragPending  = true;
        m_sekaiPressXY  = g_pInputManager->getMouseCoordsInternal(); // SEKAI_DRAG_ANCHOR
    }
}

void CHyprBar::handleUpEvent(SCallbackInfo& info) {
    // SEKAI_BUTTON_RELEASE: 창 단추는 같은 단추 위에서 뗄 때 실행한다 (윈도우처럼 — 누른 채 벗어나 떼면 취소).
    //   전에는 누르는 순간 실행해서, 닫기를 잘못 누르고 손을 빼도 창이 닫혔다 (저장 안 한 메모장도)
    if (m_iSekaiArmed >= 0 && g_pSessionLockManager->isSessionLocked()) // SEKAI_LOCK_BARS: 누른 채 잠겼다 — 실행하지 않는다
        m_iSekaiArmed = -1;
    if (m_iSekaiArmed >= 0) {
        const int ARMED = m_iSekaiArmed;
        m_iSekaiArmed   = -1;
        if (m_bCancelledDown)
            info.cancelled = true;
        m_bCancelledDown = false;
        m_bDragPending   = false;
        m_bTouchEv       = false;
        const auto PW    = m_pWindow.lock();
        if (PW && validMapped(PW) && inputIsValid() && sekaiButtonAt(cursorRelativeToBar()) == ARMED)
            sekaiRunButton(ARMED);
        damageOnButtonHover();
        return;
    }
    if (m_pWindow.lock() != g_pCompositor->m_lastWindow.lock()) {
        if (m_bDraggingThis) { // SEKAI_SNAP_GONE: 초점이 옮겨 가(창이 닫힘) 놓음이 여기서 끝났다
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            m_bDraggingThis = false;
            m_bDragPending  = false;
        }
        // SEKAI_BAR_INPUT: 창이 초점을 잃은 사이(최소화 단추·저장할까요 창) 뗌 — 우리가 삼킨 누름의 뗌이면
        //   함께 삼키고, 표시는 모두 지운다
        if (m_bCancelledDown)
            info.cancelled = true;
        m_bCancelledDown = false;
        m_bDragPending   = false;
        m_bTouchEv       = false;
        return;
    }

    if (m_bCancelledDown)
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
        m_bDraggingThis = false;

        Debug::log(LOG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;
    m_bTouchEv     = false;
}

void CHyprBar::handleMovement() {
    g_pKeybindManager->m_dispatchers["mouse"]("1movewindow");
    // SEKAI_DRAG_ANCHOR: 끌기는 첫 움직임에서 시작한다 — 기준점은 누른 자리로 (첫 움직임만큼 창이 덜 따라오지 않게)
    if (!m_bTouchEv && g_pLayoutManager->getCurrentLayout())
        g_pLayoutManager->getCurrentLayout()->sekaiSetDragAnchor(m_sekaiPressXY);
    m_bDraggingThis = true;
    Debug::log(LOG, "[hyprbars] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
    return;
}

bool CHyprBar::doButtonPress(Hyprlang::INT* const* PBARPADDING, Hyprlang::INT* const* PBARBUTTONPADDING, Hyprlang::INT* const* PHEIGHT, Vector2D COORDS, const bool BUTTONSRIGHT) {
    (void)PBARPADDING; (void)PBARBUTTONPADDING; (void)PHEIGHT; (void)BUTTONSRIGHT;
    // SEKAI_BUTTON_RELEASE: 누를 때는 어느 단추인지 기억만 한다 — 실행은 뗄 때 (handleUpEvent)
    const int IDX = sekaiButtonAt(COORDS);
    if (IDX < 0)
        return false;
    m_iSekaiArmed  = IDX;
    m_bDragPending = false;
    return true;
}

int CHyprBar::sekaiButtonAt(Vector2D COORDS) {
    static auto* const PHEIGHT           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
    static auto* const PBARBUTTONPADDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_button_padding")->getDataStaticPtr();
    static auto* const PBARPADDING       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_padding")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    const bool         BUTTONSRIGHT      = std::string{*PALIGNBUTTONS} != "left";

    float              offset = **PBARPADDING;
    int                idx    = 0;
    for (auto& b : g_pGlobalState->buttons) {
        if (sekaiDialogSkip(m_pWindow.lock(), b.icon)) { // SEKAI_DIALOG_BUTTONS
            idx++;
            continue;
        }
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, **PHEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - **PBARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        if (sekaiInButton(m_pWindow.lock(), COORDS, currentPos, b.size, **PBARBUTTONPADDING, BARBUF.y)) // SEKAI_BUTTON_SLOT
            return idx;

        offset += **PBARBUTTONPADDING + b.size;
        idx++;
    }
    return -1;
}

void CHyprBar::sekaiRunButton(int idx) {
    if (idx < 0 || idx >= (int)g_pGlobalState->buttons.size())
        return;
    const auto& b = g_pGlobalState->buttons[idx];
    // SEKAI_BAR_INPUT: 창 조작 단추는 이 창에 곧바로 (exec 로 hyprctl 을 띄우면 도착했을 때의 초점 창에 먹었다)
    if (const auto W = m_pWindow.lock(); W && (b.icon == "sekai:close" || b.icon == "sekai:min" || b.icon == "sekai:max")) {
        const auto ADDR = std::format("address:0x{:x}", (uintptr_t)W.get());
        if (b.icon == "sekai:close")
            g_pKeybindManager->m_dispatchers["closewindow"](ADDR);
        else if (b.icon == "sekai:min")
            g_pKeybindManager->m_dispatchers["sekaiminimize"]("on," + ADDR); // SEKAI_MINIMIZE2
        else
            g_pKeybindManager->m_dispatchers["sekaimaximize"]("toggle," + ADDR); // SEKAI_MAXIMIZE2: 이 창을 집어서 (초점과 상관없이)
        return;
    }
    g_pKeybindManager->m_dispatchers["exec"](b.cmd);
}

void CHyprBar::renderText(SP<CTexture> out, const std::string& text, const CHyprColor& color, const Vector2D& bufferSize, const float scale, const int fontSize) {
    const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto CAIRO        = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    // SEKAI_VECTOR_CAPTION_ICONS — 창 조작 버튼은 글꼴이 아니라 선으로 그린다
    if (text == "sekai:min" || text == "sekai:max" || text == "sekai:restore" || text == "sekai:close") {
        const double S  = std::round(std::min(bufferSize.x, bufferSize.y) * 0.625); // 아이콘 한 변
        const double LW = std::max(1.0, std::round((double)scale));                // 선 굵기 (배율 1 = 1px)
        const double X0 = std::round((bufferSize.x - S) / 2.0);
        const double Y0 = std::round((bufferSize.y - S) / 2.0);
        const double HP = LW / 2.0;                                                  // 픽셀 격자 정렬
        cairo_set_source_rgba(CAIRO, color.r, color.g, color.b, color.a);
        cairo_set_line_width(CAIRO, LW);
        if (text == "sekai:min") {
            cairo_set_line_cap(CAIRO, CAIRO_LINE_CAP_BUTT);
            const double y = std::round(bufferSize.y / 2.0) + HP;
            cairo_move_to(CAIRO, X0, y);
            cairo_line_to(CAIRO, X0 + S, y);
        } else if (text == "sekai:max") {
            cairo_rectangle(CAIRO, X0 + HP, Y0 + HP, S - LW, S - LW);
        } else if (text == "sekai:restore") { // SEKAI_MAXIMIZE2: 겹친 두 네모 (윈도우의 "이전 크기로 복원")
            const double O = std::round(S * 0.22);
            cairo_rectangle(CAIRO, X0 + HP, Y0 + O + HP, S - O - LW, S - O - LW); // 앞 네모
            cairo_move_to(CAIRO, X0 + O + HP, Y0 + O);                            // 뒤 네모의 위·오른쪽 변
            cairo_line_to(CAIRO, X0 + O + HP, Y0 + HP);
            cairo_line_to(CAIRO, X0 + S - HP, Y0 + HP);
            cairo_line_to(CAIRO, X0 + S - HP, Y0 + S - O - HP);
            cairo_line_to(CAIRO, X0 + S - O, Y0 + S - O - HP);
        } else {
            cairo_set_line_cap(CAIRO, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(CAIRO, X0 + HP, Y0 + HP);
            cairo_line_to(CAIRO, X0 + S - HP, Y0 + S - HP);
            cairo_move_to(CAIRO, X0 + S - HP, Y0 + HP);
            cairo_line_to(CAIRO, X0 + HP, Y0 + S - HP);
        }
        cairo_stroke(CAIRO);
    } else {
    // draw title using Pango
    PangoLayout* layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, text.c_str(), -1);

    PangoFontDescription* fontDesc = pango_font_description_from_string("sans");
    pango_font_description_set_size(fontDesc, fontSize * scale * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);

    const int maxWidth = bufferSize.x;

    pango_layout_set_width(layout, maxWidth * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    cairo_set_source_rgba(CAIRO, color.r, color.g, color.b, color.a);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_extents(layout, &ink_rect, &logical_rect);

    const int    layoutWidth  = ink_rect.width;
    const int    layoutHeight = logical_rect.height;

    const double xOffset = (bufferSize.x / 2.0 - layoutWidth / PANGO_SCALE / 2.0);
    const double yOffset = (bufferSize.y / 2.0 - layoutHeight / PANGO_SCALE / 2.0);

    cairo_move_to(CAIRO, xOffset, yOffset);
    pango_cairo_show_layout(CAIRO, layout);

    g_object_unref(layout);
    } // SEKAI_VECTOR_CAPTION_ICONS

    cairo_surface_flush(CAIROSURFACE);

    // copy the data to an OpenGL texture we have
    const auto DATA = cairo_image_surface_get_data(CAIROSURFACE);
    out->allocate();
    glBindTexture(GL_TEXTURE_2D, out->m_texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

#ifndef GLES2
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bufferSize.x, bufferSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, DATA);

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);
}

void CHyprBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    static auto* const PCOLOR            = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:col.text")->getDataStaticPtr();
    static auto* const PSIZE             = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_text_size")->getDataStaticPtr();
    static auto* const PFONT             = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_text_font")->getDataStaticPtr();
    static auto* const PALIGN            = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_text_align")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    static auto* const PBARPADDING       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_padding")->getDataStaticPtr();
    static auto* const PBARBUTTONPADDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_button_padding")->getDataStaticPtr();

    const bool         BUTTONSRIGHT = std::string{*PALIGNBUTTONS} != "left";

    const auto         PWINDOW = m_pWindow.lock();

    const auto         BORDERSIZE = PWINDOW->getRealBorderSize();

    float              buttonSizes = **PBARBUTTONPADDING;
    for (auto& b : g_pGlobalState->buttons) {
        if (sekaiDialogSkip(m_pWindow.lock(), b.icon)) // SEKAI_DIALOG_BUTTONS
            continue;
        buttonSizes += b.size + **PBARBUTTONPADDING;
    }

    const auto       scaledSize        = **PSIZE * scale;
    const auto       scaledBorderSize  = BORDERSIZE * scale;
    const auto       scaledButtonsSize = buttonSizes * scale;
    const auto       scaledButtonsPad  = **PBARBUTTONPADDING * scale;
    const auto       scaledBarPadding  = **PBARPADDING * scale;

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(**PCOLOR);

    const auto       CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto       CAIRO        = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    // draw title using Pango
    PangoLayout* layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, m_szLastTitle.c_str(), -1);

    PangoFontDescription* fontDesc = pango_font_description_from_string(*PFONT);
    pango_font_description_set_size(fontDesc, scaledSize * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);

    PangoContext* context = pango_layout_get_context(layout);
    pango_context_set_base_dir(context, PANGO_DIRECTION_NEUTRAL);

    const int paddingTotal = scaledBarPadding * 2 + scaledButtonsSize + (std::string{*PALIGN} != "left" ? scaledButtonsSize : 0);
    const int maxWidth     = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

    pango_layout_set_width(layout, maxWidth * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    cairo_set_source_rgba(CAIRO, COLOR.r, COLOR.g, COLOR.b, COLOR.a);

    int layoutWidth, layoutHeight;
    pango_layout_get_size(layout, &layoutWidth, &layoutHeight);
    const int xOffset = std::string{*PALIGN} == "left" ? std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize)) :
                                                         std::round(((bufferSize.x - scaledBorderSize) / 2.0 - layoutWidth / PANGO_SCALE / 2.0));
    const int yOffset = std::round((bufferSize.y / 2.0 - layoutHeight / PANGO_SCALE / 2.0));

    cairo_move_to(CAIRO, xOffset, yOffset);
    pango_cairo_show_layout(CAIRO, layout);

    g_object_unref(layout);

    cairo_surface_flush(CAIROSURFACE);

    // copy the data to an OpenGL texture we have
    const auto DATA = cairo_image_surface_get_data(CAIROSURFACE);
    m_pTextTex->allocate();
    glBindTexture(GL_TEXTURE_2D, m_pTextTex->m_texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

#ifndef GLES2
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bufferSize.x, bufferSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, DATA);

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);
}

size_t CHyprBar::getVisibleButtonCount(Hyprlang::INT* const* PBARBUTTONPADDING, Hyprlang::INT* const* PBARPADDING, const Vector2D& bufferSize, const float scale) {
    float  availableSpace = bufferSize.x - **PBARPADDING * scale * 2;
    size_t count          = 0;

    for (const auto& button : g_pGlobalState->buttons) {
        if (sekaiDialogSkip(m_pWindow.lock(), button.icon)) // SEKAI_DIALOG_BUTTONS
            continue;
        const float buttonSpace = (button.size + **PBARBUTTONPADDING) * scale;
        if (availableSpace >= buttonSpace) {
            count++;
            availableSpace -= buttonSpace;
        } else
            break;
    }

    return count;
}

void CHyprBar::renderBarButtons(const Vector2D& bufferSize, const float scale) {
    static auto* const PBARBUTTONPADDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_button_padding")->getDataStaticPtr();
    static auto* const PBARPADDING       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_padding")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    static auto* const PINACTIVECOLOR    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:inactive_button_color")->getDataStaticPtr();

    const bool         BUTTONSRIGHT = std::string{*PALIGNBUTTONS} != "left";
    const auto         visibleCount = getVisibleButtonCount(PBARBUTTONPADDING, PBARPADDING, bufferSize, scale);

    const auto         CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto         CAIRO        = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    // draw buttons
    int offset = **PBARPADDING * scale;
    for (size_t i = 0, sekaiShown = 0; i < g_pGlobalState->buttons.size() && sekaiShown < visibleCount; ++i) {
        const auto& button           = g_pGlobalState->buttons[i];
        if (sekaiDialogSkip(m_pWindow.lock(), button.icon)) // SEKAI_DIALOG_BUTTONS
            continue;
        ++sekaiShown;
        const auto  scaledButtonSize = button.size * scale;
        const auto  scaledButtonsPad = **PBARBUTTONPADDING * scale;

        const auto  pos   = Vector2D{BUTTONSRIGHT ? bufferSize.x - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0, bufferSize.y / 2.0}.floor();
        auto        color = button.bgcol;

        if (**PINACTIVECOLOR > 0) {
            color = m_bWindowHasFocus ? color : CHyprColor(**PINACTIVECOLOR);
            if (button.userfg && button.iconTex->m_texID != 0)
                button.iconTex->destroyTexture();
            if (button.userfg && button.iconTex2->m_texID != 0)
                button.iconTex2->destroyTexture();
        }

        cairo_set_source_rgba(CAIRO, color.r, color.g, color.b, color.a);
        cairo_arc(CAIRO, pos.x, pos.y, scaledButtonSize / 2, 0, 2 * M_PI);
        cairo_fill(CAIRO);

        offset += scaledButtonsPad + scaledButtonSize;
    }

    // copy the data to an OpenGL texture we have
    const auto DATA = cairo_image_surface_get_data(CAIROSURFACE);
    m_pButtonsTex->allocate();
    glBindTexture(GL_TEXTURE_2D, m_pButtonsTex->m_texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

#ifndef GLES2
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bufferSize.x, bufferSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, DATA);

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);
}

void CHyprBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    static auto* const PHEIGHT           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
    static auto* const PBARBUTTONPADDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_button_padding")->getDataStaticPtr();
    static auto* const PBARPADDING       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_padding")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    static auto* const PICONONHOVER      = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:icon_on_hover")->getDataStaticPtr();

    const bool         BUTTONSRIGHT = std::string{*PALIGNBUTTONS} != "left";
    const auto         visibleCount = getVisibleButtonCount(PBARBUTTONPADDING, PBARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const auto         COORDS       = cursorRelativeToBar();

    int                offset        = **PBARPADDING * scale;
    float              noScaleOffset = **PBARPADDING;

    for (size_t i = 0, sekaiShown = 0; i < g_pGlobalState->buttons.size() && sekaiShown < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        if (sekaiDialogSkip(m_pWindow.lock(), button.icon)) // SEKAI_DIALOG_BUTTONS
            continue;
        ++sekaiShown;
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = **PBARBUTTONPADDING * scale;

        // check if hovering here
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, **PHEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - **PBARBUTTONPADDING - button.size - noScaleOffset : noScaleOffset), (BARBUF.y - button.size) / 2.0}.floor();
        bool       hovering   = sekaiInButton(m_pWindow.lock(), COORDS, currentPos, button.size, **PBARBUTTONPADDING, BARBUF.y); // SEKAI_BUTTON_SLOT
        noScaleOffset += **PBARBUTTONPADDING + button.size;

        // SEKAI_MAXIMIZE2: 최대화한 창은 최대화 단추를 복원 모양으로
        const bool SEKAIRESTORE = button.icon == "sekai:max" && m_pWindow.lock() && m_pWindow.lock()->m_sekaiMaximized;
        const auto ICONTEX      = SEKAIRESTORE ? button.iconTex2 : button.iconTex;
        if (ICONTEX->m_texID == 0 /* icon is not rendered */ && !button.icon.empty()) {
            // render icon
            const Vector2D BUFSIZE = {scaledButtonSize, scaledButtonSize};
            auto           fgcol   = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);

            renderText(ICONTEX, SEKAIRESTORE ? std::string{"sekai:restore"} : button.icon, fgcol, BUFSIZE, scale, button.size * 0.62);
        }

        if (ICONTEX->m_texID == 0)
            continue;

        CBox pos = {barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize : offset), barBox->y + (barBox->height - scaledButtonSize) / 2.0, scaledButtonSize,
                    scaledButtonSize};

        if (hovering) { // SEKAI_BUTTON_HOVER
            const double INSETY = std::round(4.0 * scale);
            const double PADX   = std::round(scaledButtonsPad / 2.0 - 2.0 * scale);
            CBox         bg     = {pos.x - PADX, barBox->y + INSETY, pos.w + PADX * 2, barBox->height - INSETY * 2};
            CHyprColor   hc     = button.icon == "sekai:close" ? CHyprColor(0xc4 / 255.0, 0x2b / 255.0, 0x1c / 255.0, 1.0) :
                                                              CHyprColor(button.fgcol.r, button.fgcol.g, button.fgcol.b, 0.12); // SEKAI_THEME
            hc.a *= a;
            g_pHyprOpenGL->renderRect(bg.round(), hc, (int)std::round(6.0 * scale), 2.0f);
        }

        if (!**PICONONHOVER || (**PICONONHOVER && m_iButtonHoverState > 0))
            g_pHyprOpenGL->renderTexture(ICONTEX, pos, a);
        offset += scaledButtonsPad + scaledButtonSize;

        bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
        if (hovering != currentBit) {
            m_iButtonHoverState ^= (1 << i);
            // damage to get rid of some artifacts when icons are "hidden"
            damageEntire();
        }
    }
}

void CHyprBar::draw(PHLMONITOR pMonitor, const float& a) {
    static auto* const PENABLED = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:enabled")->getDataStaticPtr();

    if (m_bLastEnabledState != **PENABLED) {
        m_bLastEnabledState = **PENABLED;
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (m_hidden || !validMapped(m_pWindow) || !**PENABLED)
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_windowData.decorate.valueOrDefault())
        return;

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

void CHyprBar::renderPass(PHLMONITOR pMonitor, const float& a) {
    const auto         PWINDOW = m_pWindow.lock();

    static auto* const PCOLOR            = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_color")->getDataStaticPtr();
    static auto* const PHEIGHT           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
    static auto* const PPRECEDENCE       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_precedence_over_border")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    static auto* const PENABLETITLE      = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_title_enabled")->getDataStaticPtr();
    static auto* const PENABLEBLUR       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_blur")->getDataStaticPtr();
    static auto* const PENABLEBLURGLOBAL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:blur:enabled")->getDataStaticPtr();
    static auto* const PINACTIVECOLOR    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:inactive_button_color")->getDataStaticPtr();

    if (**PINACTIVECOLOR > 0) {
        bool currentWindowFocus = PWINDOW == g_pCompositor->m_lastWindow.lock();
        if (currentWindowFocus != m_bWindowHasFocus) {
            m_bWindowHasFocus = currentWindowFocus;
            m_bButtonsDirty   = true;
        }
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(**PCOLOR);
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    color.a *= a;
    const bool BUTTONSRIGHT = std::string{*PALIGNBUTTONS} != "left";
    const bool SHOULDBLUR   = **PENABLEBLUR && **PENABLEBLURGLOBAL && color.a < 1.F;

    if (**PHEIGHT < 1) {
        m_iLastHeight = **PHEIGHT;
        return;
    }

    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    const auto ROUNDING = PWINDOW->rounding() + (*PPRECEDENCE ? 0 : PWINDOW->getRealBorderSize());

    const auto scaledRounding = ROUNDING > 0 ? ROUNDING * pMonitor->m_scale - 2 /* idk why but otherwise it looks bad due to the gaps */ : 0;

    m_seExtents = {{0, **PHEIGHT}, {}};

    const auto DECOBOX = assignedBoxGlobal();

    const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

    CBox       titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w,
                              DECOBOX.h + ROUNDING * 3 /* to fill the bottom cuz we can't disable rounding there */};

    titleBarBox.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();

    if (titleBarBox.w < 1 || titleBarBox.h < 1)
        return;

    g_pHyprOpenGL->scissor(titleBarBox);

    if (ROUNDING) {
        // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
        CBox windowBox = {PWINDOW->m_realPosition->value().x + PWINDOW->m_floatingOffset.x - pMonitor->m_position.x + 1,
                          PWINDOW->m_realPosition->value().y + PWINDOW->m_floatingOffset.y - pMonitor->m_position.y + 1, PWINDOW->m_realSize->value().x - 2,
                          PWINDOW->m_realSize->value().y - 2};

        if (windowBox.w < 1 || windowBox.h < 1)
            return;

        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);

        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        windowBox.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(windowBox, CHyprColor(0, 0, 0, 0), scaledRounding, m_pWindow->roundingPower());
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_NOTEQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    if (SHOULDBLUR)
        g_pHyprOpenGL->renderRectWithBlur(titleBarBox, color, scaledRounding, m_pWindow->roundingPower(), a);
    else
        g_pHyprOpenGL->renderRect(titleBarBox, color, scaledRounding, m_pWindow->roundingPower());

    // render title
    if (**PENABLETITLE && (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || m_pTextTex->m_texID == 0 || m_bTitleColorChanged)) {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(BARBUF, pMonitor->m_scale);
    }

    if (ROUNDING) {
        // cleanup stencil
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }

    CBox textBox = {titleBarBox.x, titleBarBox.y, (int)BARBUF.x, (int)BARBUF.y};
    if (**PENABLETITLE)
        g_pHyprOpenGL->renderTexture(m_pTextTex, textBox, a);

    if (m_bButtonsDirty || m_bWindowSizeChanged) {
        renderBarButtons(BARBUF, pMonitor->m_scale);
        m_bButtonsDirty = false;
    }

    g_pHyprOpenGL->renderTexture(m_pButtonsTex, textBox, a);

    g_pHyprOpenGL->scissor(nullptr);

    renderBarButtonsText(&textBox, pMonitor->m_scale, a);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;

    // dynamic updates change the extents
    if (m_iLastHeight != **PHEIGHT) {
        g_pLayoutManager->getCurrentLayout()->recalculateWindow(PWINDOW);
        m_iLastHeight = **PHEIGHT;
    }
}

eDecorationType CHyprBar::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CHyprBar::updateWindow(PHLWINDOW pWindow) {
    damageEntire();
}

void CHyprBar::damageEntire() {
    g_pHyprRenderer->damageBox(assignedBoxGlobal());
}

Vector2D CHyprBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

eDecorationLayer CHyprBar::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CHyprBar::getDecorationFlags() {
    static auto* const PPART = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_part_of_window")->getDataStaticPtr();
    return DECORATION_ALLOWS_MOUSE_INPUT | (**PPART ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox CHyprBar::assignedBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    CBox box = m_bAssignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW CHyprBar::getOwner() {
    return m_pWindow.lock();
}

void CHyprBar::updateRules() {
    const auto PWINDOW              = m_pWindow.lock();
    auto       rules                = PWINDOW->m_matchedRules;
    auto       prevHidden           = m_hidden;
    auto       prevForcedTitleColor = m_bForcedTitleColor;

    m_bForcedBarColor   = std::nullopt;
    m_bForcedTitleColor = std::nullopt;
    m_hidden            = false;

    for (auto& r : rules) {
        applyRule(r);
    }
    // SEKAI_CLIENT_DECO: 제목줄을 스스로 그리겠다고 한 앱(Discord·VS Code 같은 Electron, Firefox 탭 제목줄 …)에는 막대를
    //   그리지 않는다 — 앱 이름으로 하나씩 nobar 규칙을 다는 대신 앱의 요청을 따른다 (제목줄이 두 개로 보이던 것)
    if (!m_hidden && PWINDOW->sekaiClientDecoration())
        m_hidden = true;

    if (prevHidden != m_hidden)
        g_pDecorationPositioner->repositionDeco(this);
    if (prevForcedTitleColor != m_bForcedTitleColor)
        m_bTitleColorChanged = true;
}

void CHyprBar::applyRule(const SP<CWindowRule>& r) {
    auto arg = r->m_rule.substr(r->m_rule.find_first_of(' ') + 1);

    if (r->m_rule == "plugin:hyprbars:nobar")
        m_hidden = true;
    else if (r->m_rule.starts_with("plugin:hyprbars:bar_color"))
        m_bForcedBarColor = CHyprColor(configStringToInt(arg).value_or(0));
    else if (r->m_rule.starts_with("plugin:hyprbars:title_color"))
        m_bForcedTitleColor = CHyprColor(configStringToInt(arg).value_or(0));
}

void CHyprBar::damageOnButtonHover() {
    static auto* const PBARPADDING       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_padding")->getDataStaticPtr();
    static auto* const PBARBUTTONPADDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_button_padding")->getDataStaticPtr();
    static auto* const PHEIGHT           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_height")->getDataStaticPtr();
    static auto* const PALIGNBUTTONS     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprbars:bar_buttons_alignment")->getDataStaticPtr();
    const bool         BUTTONSRIGHT      = std::string{*PALIGNBUTTONS} != "left";

    float              offset = **PBARPADDING;

    const auto         COORDS = cursorRelativeToBar();

    for (auto& b : g_pGlobalState->buttons) {
        if (sekaiDialogSkip(m_pWindow.lock(), b.icon)) // SEKAI_DIALOG_BUTTONS
            continue;
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, **PHEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - **PBARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        bool       hover = sekaiInButton(m_pWindow.lock(), COORDS, currentPos, b.size, **PBARBUTTONPADDING, BARBUF.y); // SEKAI_BUTTON_SLOT

        const size_t IDX = &b - &g_pGlobalState->buttons[0];
        const bool   WAS = IDX < 32 && (m_iSekaiHover & (1u << IDX));
        if (hover != WAS) { // SEKAI_BUTTON_HOVER
            if (IDX < 32)
                m_iSekaiHover ^= (1u << IDX);
            if (b.icon == "sekai:max" && !m_bDraggingThis) { // SEKAI_SNAP_LAYOUT 스냅 레이아웃
                const auto BOX = assignedBoxGlobal();
                const auto PW  = m_pWindow.lock();
                if (hover)
                    g_pEventManager->postEvent(SHyprIPCEvent{"sekaimaxhover", std::format("on,{:x},{},{}", (uintptr_t)PW.get(),
                                                                                           (int)(BOX.x + currentPos.x + b.size / 2.0), (int)(BOX.y + **PHEIGHT))});
                else
                    g_pEventManager->postEvent(SHyprIPCEvent{"sekaimaxhover", std::format("off,{:x}", (uintptr_t)PW.get())});
            }
            m_bButtonHovered = hover;
            damageEntire();
        }

        offset += **PBARBUTTONPADDING + b.size;
    }
}
