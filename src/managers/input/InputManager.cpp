#include "InputManager.hpp"
#include "../../Compositor.hpp"
#include <aquamarine/output/Output.hpp>
#include <cstdint>
#include <hyprutils/math/Vector2D.hpp>
#include <ranges>
#include "../../config/ConfigValue.hpp"
#include "../../config/ConfigManager.hpp"
#include "../../desktop/Window.hpp"
#include "../../desktop/LayerSurface.hpp"
#include "../../protocols/CursorShape.hpp"
#include "../../protocols/IdleInhibit.hpp"
#include "../../protocols/RelativePointer.hpp"
#include "../../protocols/PointerConstraints.hpp"
#include "../../protocols/IdleNotify.hpp"
#include "../../protocols/SessionLock.hpp"
#include "../../protocols/InputMethodV2.hpp"
#include "../../protocols/VirtualKeyboard.hpp"
#include "../../protocols/VirtualPointer.hpp"
#include "../../protocols/LayerShell.hpp"
#include "../../protocols/core/Seat.hpp"
#include "../../protocols/core/DataDevice.hpp"
#include "../../protocols/core/Compositor.hpp"
#include "../../protocols/XDGShell.hpp"

#include "../../devices/Mouse.hpp"
#include "../../devices/VirtualPointer.hpp"
#include "../../devices/Keyboard.hpp"
#include "../../devices/VirtualKeyboard.hpp"
#include "../../devices/TouchDevice.hpp"

#include "../../managers/PointerManager.hpp"
#include "../../managers/SeatManager.hpp"
#include "../../managers/KeybindManager.hpp"
#include "../../render/Renderer.hpp"
#include "../../managers/HookSystemManager.hpp"
#include "../../managers/EventManager.hpp"
#include "../../managers/LayoutManager.hpp"
#include "../../managers/permissions/DynamicPermissionManager.hpp"

// ── SEKAI_BORDER_GRAB: 윈도우처럼 창 테두리로 크기 조절 ─────────────────
int                     g_sekaiResizeAxis = 0; // 0 모서리 · 1 세로만(위·아래 가장자리) · 2 가로만(왼쪽·오른쪽) — IHyprLayout 가 읽는다
static constexpr double SEKAI_INNER_GRAB  = 4;  // 창 안쪽으로도 이만큼은 가장자리
static constexpr double SEKAI_CORNER      = 20; // 가장자리 끝에서 이만큼은 모서리

// SEKAI_BORDER_EDGE: 창(제목줄 포함) 상자 B 의 변 중 화면 끝(모니터 끝·작업 표시줄 끝)에 붙은 변 — 방향 비트
static int sekaiScreenEdges(PHLWINDOW w, const CBox& B) {
    const auto M = w->m_monitor.lock();
    if (!M)
        return 0;
    const double L = M->m_position.x + M->m_reservedTopLeft.x, T = M->m_position.y + M->m_reservedTopLeft.y;
    const double R = M->m_position.x + M->m_size.x - M->m_reservedBottomRight.x, D = M->m_position.y + M->m_size.y - M->m_reservedBottomRight.y;
    return (B.x <= L + 1 ? 1 : 0) | (B.x + B.width >= R - 1 ? 2 : 0) | (B.y <= T + 1 ? 4 : 0) | (B.y + B.height >= D - 1 ? 8 : 0);
}

// 커서가 창(제목줄 포함) 테두리 근처면 방향 비트 (1 왼 · 2 오 · 4 위 · 8 아래), 아니면 0
static int sekaiBorderAt(PHLWINDOW w, const Vector2D& p, double outer) {
    const CBox real = {w->m_realPosition->value().x, w->m_realPosition->value().y, w->m_realSize->value().x, w->m_realSize->value().y};
    const auto EXT  = w->getFullWindowReservedArea(); // 제목줄 등 장식이 차지한 몫
    const CBox B    = {real.x - EXT.topLeft.x, real.y - EXT.topLeft.y, real.width + EXT.topLeft.x + EXT.bottomRight.x,
                       real.height + EXT.topLeft.y + EXT.bottomRight.y};
    if (!B.copy().expand(outer).containsPoint(p))
        return 0;

    int e = 0;
    if (!B.containsPoint(p)) { // 바깥 띠
        if (p.x < B.x)
            e |= 1;
        else if (p.x >= B.x + B.width)
            e |= 2;
        if (p.y < B.y)
            e |= 4;
        else if (p.y >= B.y + B.height)
            e |= 8;
    } else { // 안쪽 몇 px
        // SEKAI_BORDER_TOPONLY: 안쪽 띠는 위쪽만 (윈도우처럼 옆·아래 테두리는 창 바깥 — 창 안은 앱 몫)
        if (p.y < B.y + SEKAI_INNER_GRAB)
            e |= 4;
        if (!e && real.containsPoint(p) && w->isInCurvedCorner(p.x, p.y)) // 둥근 모서리 안쪽 (원래 동작)
            e = (p.x < real.x + real.width / 2 ? 1 : 2) | (p.y < real.y + real.height / 2 ? 4 : 8);
        e &= ~sekaiScreenEdges(w, B); // SEKAI_BORDER_EDGE: 화면 끝에 붙은 변의 안쪽은 창 몫
    }
    if (!e)
        return 0;
    // 가장자리 끝 가까이면 모서리로
    if (e & 12) {
        if (p.x < B.x + SEKAI_CORNER)
            e |= 1;
        else if (p.x > B.x + B.width - SEKAI_CORNER)
            e |= 2;
    }
    if (e & 3) {
        if (p.y < B.y + SEKAI_CORNER)
            e |= 4;
        else if (p.y > B.y + B.height - SEKAI_CORNER)
            e |= 8;
    }
    return e;
}

// ── SEKAI_CLIENT_MOVE: 창이 스스로 그린 제목줄(CSD) 끌기 ─────────────────
//   XDGShell.cpp 의 move 요청에서 시작, 버튼을 떼면 끝. 스냅 이벤트는 hyprbars 패치와 같다.
static bool         sekaiMoving    = false;
static std::string  sekaiMoveZone  = "none";
static PHLWINDOWREF sekaiMoveWin;
static bool         sekaiButtonHeld = false; // SEKAI_CLIENT_MOVE2: 지금 마우스 버튼이 눌려 있나 (onMouseButton 이 적는다)
static Vector2D     sekaiPressXY;            // SEKAI_CLIENT_MOVE3: 마지막으로 버튼을 누른 자리 (끌기 기준점)

static std::string  sekaiMoveZoneAt(const Vector2D& p, PHLMONITOR& mon) {
    mon = g_pCompositor->getMonitorFromVector(p);
    if (!mon)
        return "none";
    const double x = p.x - mon->m_position.x, y = p.y - mon->m_position.y;
    const double W = mon->m_size.x, H = mon->m_size.y;
    const double E = 4;                                  // 가장자리로 치는 두께
    const double C = std::max(48.0, std::min(W, H) / 8); // 모서리로 치는 길이
    const bool   L = x <= E, R = x >= W - 1 - E, T = y <= E, B = y >= H - 1 - E;
    if ((L && y < C) || (T && x < C))
        return "tl";
    if ((R && y < C) || (T && x > W - C))
        return "tr";
    if ((L && y > H - C) || (B && x < C))
        return "bl";
    if ((R && y > H - C) || (B && x > W - C))
        return "br";
    if (L)
        return "left";
    if (R)
        return "right";
    if (T)
        return "max";
    return "none";
}

void sekaiClientMoveStart(PHLWINDOW w) {
    if (!w || sekaiMoving || !sekaiButtonHeld || !g_pInputManager->m_currentlyDraggedWindow.expired())
        return;
    // SEKAI_MISCLICK: 커서 밑이 이 창이 아니면(빠르게 튕겨 이미 다른 창 위) 시작하지 않는다 — 다른 창을
    //   잡았다 되돌리면서 그 창의 초점·올림이 남았다
    if (!validMapped(w) || g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                                               RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING) != w)
        return;
    g_pKeybindManager->m_dispatchers["mouse"]("1movewindow");
    if (g_pInputManager->m_currentlyDraggedWindow.lock() != w) { // 커서 밑이 다른 창이었다 — 되돌린다
        g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
        return;
    }
    sekaiMoving   = true;
    sekaiMoveWin  = w;
    sekaiMoveZone = "none";
    // SEKAI_CLIENT_MOVE3: 기준점은 누른 자리 — 앱은 커서가 끌기 문턱(GTK 는 몇 px)을 넘은 뒤에야 move 를 청해, 그때의 커서를
    //   기준으로 잡으면 창이 그만큼 덜 따라와 커서가 제목줄의 같은 자리에서 밀려 났다 (hyprbars 막대와 같게)
    g_pLayoutManager->getCurrentLayout()->sekaiSetDragAnchor(sekaiPressXY);
    g_pEventManager->postEvent(SHyprIPCEvent{"sekaisnapstart", std::format("{:x}", (uintptr_t)w.get())});
}

#include "../../helpers/time/Time.hpp"

#include <aquamarine/input/Input.hpp>
#include <unordered_set>
#include <map>
#include <set>
#include "../eventLoop/EventLoopManager.hpp"
#include "SekaiA11yMonitor.hpp"

// SEKAI_LAYER_REFOCUS: 보이는 작업 공간에 마지막으로 초점을 가졌던 창이 있나
static bool sekaiLastWindowShown() {
    const auto W = g_pCompositor->m_lastWindow.lock();
    return W && W->m_isMapped && W->m_workspace && W->m_workspace->isVisibleNotCovered();
}

CInputManager::CInputManager() {
    m_listeners.setCursorShape = PROTO::cursorShape->m_events.setShape.listen([this](const CCursorShapeProtocol::SSetShapeEvent& event) {
        const bool SEKAIDEFER = m_cursorImageOverridden && m_borderIconDirection != BORDERICON_NONE && m_clickBehavior != CLICKMODE_KILL; // SEKAI_BORDER_FIX: 테두리 커서 동안이면 적어만 둔다
        if (!cursorImageUnlocked() && !SEKAIDEFER)
            return;

        if (!g_pSeatManager->m_state.pointerFocusResource)
            return;

        if (wl_resource_get_client(event.pMgr->resource()) != g_pSeatManager->m_state.pointerFocusResource->client())
            return;

        Debug::log(LOG, "cursorImage request: shape {} -> {}", (uint32_t)event.shape, event.shapeName);

        m_cursorSurfaceInfo.wlSurface->unassign();
        m_cursorSurfaceInfo.vHotspot = {};
        m_cursorSurfaceInfo.name     = event.shapeName;
        m_cursorSurfaceInfo.hidden   = false;

        m_cursorSurfaceInfo.inUse = !SEKAIDEFER;
        if (SEKAIDEFER)
            return;
        g_pHyprRenderer->setCursorFromName(m_cursorSurfaceInfo.name);
    });

    m_listeners.newIdleInhibitor   = PROTO::idleInhibit->m_events.newIdleInhibitor.listen([this](const auto& data) { this->newIdleInhibitor(data); });
    m_listeners.newVirtualKeyboard = PROTO::virtualKeyboard->m_events.newKeyboard.listen([this](const auto& keyboard) {
        this->newVirtualKeyboard(keyboard);
        updateCapabilities();
    });
    m_listeners.newVirtualMouse    = PROTO::virtualPointer->m_events.newPointer.listen([this](const auto& mouse) {
        this->newVirtualMouse(mouse);
        updateCapabilities();
    });
    m_listeners.setCursor          = g_pSeatManager->m_events.setCursor.listen([this](const auto& event) { this->processMouseRequest(event); });

    m_cursorSurfaceInfo.wlSurface = CWLSurface::create();
}

CInputManager::~CInputManager() {
    m_constraints.clear();
    m_keyboards.clear();
    m_pointers.clear();
    m_touches.clear();
    m_tablets.clear();
    m_tabletTools.clear();
    m_tabletPads.clear();
    m_idleInhibitors.clear();
    m_switches.clear();
}

void CInputManager::onMouseMoved(IPointer::SMotionEvent e) {
    static auto PNOACCEL = CConfigValue<Hyprlang::INT>("input:force_no_accel");

    if (SekaiA11y::g_zoomFocus) { // SEKAI_ZOOM_FOCUS: 마우스를 움직이면 돋보기는 다시 마우스를 따라간다
        SekaiA11y::g_zoomFocus.reset();
        for (auto const& m : g_pCompositor->m_monitors)
            g_pCompositor->scheduleFrameForMonitor(m);
    }

    Vector2D    delta   = e.delta;
    Vector2D    unaccel = e.unaccel;

    if (e.device) {
        if (e.device->m_isTouchpad) {
            if (e.device->m_flipX) {
                delta.x   = -delta.x;
                unaccel.x = -unaccel.x;
            }
            if (e.device->m_flipY) {
                delta.y   = -delta.y;
                unaccel.y = -unaccel.y;
            }
        }
    }

    const auto DELTA = *PNOACCEL == 1 ? unaccel : delta;

    if (g_pSeatManager->m_isPointerFrameSkipped)
        g_pPointerManager->storeMovement((uint64_t)e.timeMs, DELTA, unaccel);
    else
        g_pPointerManager->setStoredMovement((uint64_t)e.timeMs, DELTA, unaccel);

    PROTO::relativePointer->sendRelativeMotion((uint64_t)e.timeMs * 1000, DELTA, unaccel);

    if (e.mouse)
        recheckMouseWarpOnMouseInput();

    g_pPointerManager->move(DELTA);

    mouseMoveUnified(e.timeMs, false, e.mouse);

    m_lastCursorMovement.reset();

    m_lastInputTouch = false;

    if (e.mouse)
        m_lastMousePos = getMouseCoordsInternal();
}

void CInputManager::onMouseWarp(IPointer::SMotionAbsoluteEvent e) {
    SekaiA11y::g_zoomFocus.reset(); // SEKAI_ZOOM_FOCUS
    g_pPointerManager->warpAbsolute(e.absolute, e.device);

    mouseMoveUnified(e.timeMs);

    m_lastCursorMovement.reset();

    m_lastInputTouch = false;
}

void CInputManager::simulateMouseMovement() {
    m_lastCursorPosFloored = m_lastCursorPosFloored - Vector2D(1, 1); // hack: force the mouseMoveUnified to report without making this a refocus.
    mouseMoveUnified(Time::millis(Time::steadyNow()));
}

void CInputManager::sendMotionEventsToFocused() {
    if (!g_pCompositor->m_lastFocus || isConstrained())
        return;

    // todo: this sucks ass
    const auto PWINDOW = g_pCompositor->getWindowFromSurface(g_pCompositor->m_lastFocus.lock());
    const auto PLS     = g_pCompositor->getLayerSurfaceFromSurface(g_pCompositor->m_lastFocus.lock());

    const auto LOCAL = getMouseCoordsInternal() - (PWINDOW ? PWINDOW->m_realPosition->goal() : (PLS ? Vector2D{PLS->m_geometry.x, PLS->m_geometry.y} : Vector2D{}));

    m_emptyFocusCursorSet = false;

    g_pSeatManager->setPointerFocus(g_pCompositor->m_lastFocus.lock(), LOCAL);
}

void CInputManager::mouseMoveUnified(uint32_t time, bool refocus, bool mouse) {
    m_lastInputMouse = mouse;

    if (sekaiMoving) { // SEKAI_CLIENT_MOVE — 끄는 중 커서가 닿은 영역 (스냅 미리보기)
        PHLMONITOR mon;
        const auto z = sekaiMoveZoneAt(getMouseCoordsInternal(), mon);
        if (z != sekaiMoveZone) {
            sekaiMoveZone = z;
            g_pEventManager->postEvent(SHyprIPCEvent{"sekaisnap", std::format("{},{}", z, mon ? mon->m_name : "")});
        }
    }

    if (!g_pCompositor->m_readyToProcess || g_pCompositor->m_isShuttingDown || g_pCompositor->m_unsafeState)
        return;

    Vector2D const mouseCoords        = getMouseCoordsInternal();
    auto const     MOUSECOORDSFLOORED = mouseCoords.floor();

    if (MOUSECOORDSFLOORED == m_lastCursorPosFloored && !refocus)
        return;

    static auto PFOLLOWMOUSE          = CConfigValue<Hyprlang::INT>("input:follow_mouse");
    static auto PFOLLOWMOUSETHRESHOLD = CConfigValue<Hyprlang::FLOAT>("input:follow_mouse_threshold");
    static auto PMOUSEREFOCUS         = CConfigValue<Hyprlang::INT>("input:mouse_refocus");
    static auto PFOLLOWONDND          = CConfigValue<Hyprlang::INT>("misc:always_follow_on_dnd");
    static auto PFLOATBEHAVIOR        = CConfigValue<Hyprlang::INT>("input:float_switch_override_focus");
    static auto PMOUSEFOCUSMON        = CConfigValue<Hyprlang::INT>("misc:mouse_move_focuses_monitor");
    static auto PRESIZEONBORDER       = CConfigValue<Hyprlang::INT>("general:resize_on_border");
    static auto PRESIZECURSORICON     = CConfigValue<Hyprlang::INT>("general:hover_icon_on_border");

    const auto  FOLLOWMOUSE = *PFOLLOWONDND && PROTO::data->dndActive() ? 1 : *PFOLLOWMOUSE;

    if (FOLLOWMOUSE == 1 && m_lastCursorMovement.getSeconds() < 0.5)
        m_mousePosDelta += MOUSECOORDSFLOORED.distance(m_lastCursorPosFloored);
    else
        m_mousePosDelta = 0;

    m_foundSurfaceToFocus.reset();
    m_foundLSToFocus.reset();
    m_foundWindowToFocus.reset();
    SP<CWLSurfaceResource> foundSurface;
    Vector2D               surfaceCoords;
    Vector2D               surfacePos = Vector2D(-1337, -1337);
    PHLWINDOW              pFoundWindow;
    PHLLS                  pFoundLayerSurface;

    EMIT_HOOK_EVENT_CANCELLABLE("mouseMove", MOUSECOORDSFLOORED);

    m_lastCursorPosFloored = MOUSECOORDSFLOORED;

    const auto PMONITOR = isLocked() && g_pCompositor->m_lastMonitor ? g_pCompositor->m_lastMonitor.lock() : g_pCompositor->getMonitorFromCursor();

    // this can happen if there are no displays hooked up to Hyprland
    if (PMONITOR == nullptr)
        return;

    if (PMONITOR->m_cursorZoom->value() != 1.f)
        g_pHyprRenderer->damageMonitor(PMONITOR);

    bool skipFrameSchedule = PMONITOR->shouldSkipScheduleFrameOnMouseEvent();

    if (!PMONITOR->m_solitaryClient.lock() && g_pHyprRenderer->shouldRenderCursor() && g_pPointerManager->softwareLockedFor(PMONITOR->m_self.lock()) && !skipFrameSchedule)
        g_pCompositor->scheduleFrameForMonitor(PMONITOR, Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);

    // constraints
    if (!g_pSeatManager->m_mouse.expired() && isConstrained()) {
        const auto SURF       = CWLSurface::fromResource(g_pCompositor->m_lastFocus.lock());
        const auto CONSTRAINT = SURF ? SURF->constraint() : nullptr;

        if (CONSTRAINT) {
            if (CONSTRAINT->isLocked()) {
                const auto HINT = CONSTRAINT->logicPositionHint();
                g_pCompositor->warpCursorTo(HINT, true);
            } else {
                const auto RG           = CONSTRAINT->logicConstraintRegion();
                const auto CLOSEST      = RG.closestPoint(mouseCoords);
                const auto BOX          = SURF->getSurfaceBoxGlobal();
                const auto CLOSESTLOCAL = (CLOSEST - (BOX.has_value() ? BOX->pos() : Vector2D{})) * (SURF->getWindow() ? SURF->getWindow()->m_X11SurfaceScaledBy : 1.0);

                g_pCompositor->warpCursorTo(CLOSEST, true);
                g_pSeatManager->sendPointerMotion(time, CLOSESTLOCAL);
                PROTO::relativePointer->sendRelativeMotion((uint64_t)time * 1000, {}, {});
            }

            return;

        } else
            Debug::log(ERR, "BUG THIS: Null SURF/CONSTRAINT in mouse refocus. Ignoring constraints. {:x} {:x}", (uintptr_t)SURF.get(), (uintptr_t)CONSTRAINT.get());
    }

    if (PMONITOR != g_pCompositor->m_lastMonitor && (*PMOUSEFOCUSMON || refocus) && m_forcedFocus.expired())
        g_pCompositor->setActiveMonitor(PMONITOR);

    // check for windows that have focus priority like our permission popups
    pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, FOCUS_PRIORITY);
    if (pFoundWindow)
        foundSurface = g_pCompositor->vectorWindowToSurface(mouseCoords, pFoundWindow, surfaceCoords);

    if (!foundSurface && g_pSessionLockManager->isSessionLocked()) {

        // set keyboard focus on session lock surface regardless of layers
        const auto PSESSIONLOCKSURFACE = g_pSessionLockManager->getSessionLockSurfaceForMonitor(PMONITOR->m_id);
        const auto foundLockSurface    = PSESSIONLOCKSURFACE ? PSESSIONLOCKSURFACE->surface->surface() : nullptr;

        g_pCompositor->focusSurface(foundLockSurface);

        // search for interactable abovelock surfaces for pointer focus, or use session lock surface if not found
        for (auto& lsl : PMONITOR->m_layerSurfaceLayers | std::views::reverse) {
            foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &lsl, &surfaceCoords, &pFoundLayerSurface, true);

            if (foundSurface)
                break;
        }

        if (!foundSurface) {
            surfaceCoords = mouseCoords - PMONITOR->m_position;
            foundSurface  = foundLockSurface;
        }

        if (refocus) {
            m_foundLSToFocus      = pFoundLayerSurface;
            m_foundWindowToFocus  = pFoundWindow;
            m_foundSurfaceToFocus = foundSurface;
        }

        g_pSeatManager->setPointerFocus(foundSurface, surfaceCoords);
        g_pSeatManager->sendPointerMotion(time, surfaceCoords);

        return;
    }

    PHLWINDOW forcedFocus = m_forcedFocus.lock();

    if (!forcedFocus)
        forcedFocus = g_pCompositor->getForceFocus();

    if (forcedFocus && !foundSurface) {
        pFoundWindow = forcedFocus;
        surfacePos   = pFoundWindow->m_realPosition->value();
        foundSurface = pFoundWindow->m_wlSurface->resource();
    }

    // if we are holding a pointer button,
    // and we're not dnd-ing, don't refocus. Keep focus on last surface.
    if (!PROTO::data->dndActive() && !m_currentlyHeldButtons.empty() && g_pCompositor->m_lastFocus && g_pCompositor->m_lastFocus->m_mapped &&
        g_pSeatManager->m_state.pointerFocus && !m_hardInput) {
        foundSurface = g_pSeatManager->m_state.pointerFocus.lock();

        // IME popups aren't desktop-like elements
        // TODO: make them.
        CInputPopup* foundPopup = m_relay.popupFromSurface(foundSurface);
        if (foundPopup) {
            surfacePos             = foundPopup->globalBox().pos();
            m_focusHeldByButtons   = true;
            m_refocusHeldByButtons = refocus;
        } else {
            auto HLSurface = CWLSurface::fromResource(foundSurface);

            if (HLSurface) {
                const auto BOX = HLSurface->getSurfaceBoxGlobal();

                if (BOX) {
                    const auto PWINDOW = HLSurface->getWindow();
                    surfacePos         = BOX->pos();
                    pFoundLayerSurface = HLSurface->getLayer();
                    if (!pFoundLayerSurface)
                        pFoundWindow = !PWINDOW || PWINDOW->isHidden() ? g_pCompositor->m_lastWindow.lock() : PWINDOW;
                } else // reset foundSurface, find one normally
                    foundSurface = nullptr;
            } else // reset foundSurface, find one normally
                foundSurface = nullptr;
        }
    }

    g_pLayoutManager->getCurrentLayout()->onMouseMove(getMouseCoordsInternal());

    // forced above all
    if (!g_pInputManager->m_exclusiveLSes.empty()) {
        if (!foundSurface)
            foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &g_pInputManager->m_exclusiveLSes, &surfaceCoords, &pFoundLayerSurface);

        if (!foundSurface) {
            foundSurface = (*g_pInputManager->m_exclusiveLSes.begin())->m_surface->resource();
            surfacePos   = (*g_pInputManager->m_exclusiveLSes.begin())->m_realPosition->goal();
        }
    }

    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerPopupSurface(mouseCoords, PMONITOR, &surfaceCoords, &pFoundLayerSurface);

    // overlays are above fullscreen
    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &pFoundLayerSurface);

    // also IME popups
    if (!foundSurface) {
        auto popup = g_pInputManager->m_relay.popupFromCoords(mouseCoords);
        if (popup) {
            foundSurface = popup->getSurface();
            surfacePos   = popup->globalBox().pos();
        }
    }

    // also top layers
    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &pFoundLayerSurface);

    // then, we check if the workspace doesnt have a fullscreen window
    const auto PWORKSPACE   = PMONITOR->m_activeSpecialWorkspace ? PMONITOR->m_activeSpecialWorkspace : PMONITOR->m_activeWorkspace;
    const auto PWINDOWIDEAL = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);
    if (PWORKSPACE->m_hasFullscreenWindow && !foundSurface && PWORKSPACE->m_fullscreenMode == FSMODE_FULLSCREEN) {
        pFoundWindow = PWORKSPACE->getFullscreenWindow();

        if (!pFoundWindow) {
            // what the fuck, somehow happens occasionally??
            PWORKSPACE->m_hasFullscreenWindow = false;
            return;
        }

        if (PWINDOWIDEAL &&
            ((PWINDOWIDEAL->m_isFloating && (PWINDOWIDEAL->m_createdOverFullscreen || PWINDOWIDEAL->m_pinned)) /* floating over fullscreen or pinned */
             || (PMONITOR->m_activeSpecialWorkspace == PWINDOWIDEAL->m_workspace) /* on an open special workspace */))
            pFoundWindow = PWINDOWIDEAL;

        if (!pFoundWindow->m_isX11) {
            foundSurface = g_pCompositor->vectorWindowToSurface(mouseCoords, pFoundWindow, surfaceCoords);
            surfacePos   = Vector2D(-1337, -1337);
        } else {
            foundSurface = pFoundWindow->m_wlSurface->resource();
            surfacePos   = pFoundWindow->m_realPosition->value();
        }
    }

    // then windows
    if (!foundSurface) {
        if (PWORKSPACE->m_hasFullscreenWindow && PWORKSPACE->m_fullscreenMode == FSMODE_MAXIMIZED) {
            if (!foundSurface) {
                if (PMONITOR->m_activeSpecialWorkspace) {
                    if (pFoundWindow != PWINDOWIDEAL)
                        pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

                    if (pFoundWindow && !pFoundWindow->onSpecialWorkspace()) {
                        pFoundWindow = PWORKSPACE->getFullscreenWindow();
                    }
                } else {
                    // if we have a maximized window, allow focusing on a bar or something if in reserved area.
                    if (g_pCompositor->isPointOnReservedArea(mouseCoords, PMONITOR)) {
                        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], &surfaceCoords,
                                                                           &pFoundLayerSurface);
                    }

                    if (!foundSurface) {
                        if (pFoundWindow != PWINDOWIDEAL)
                            pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

                        if (!(pFoundWindow && (pFoundWindow->m_isFloating && (pFoundWindow->m_createdOverFullscreen || pFoundWindow->m_pinned))))
                            pFoundWindow = PWORKSPACE->getFullscreenWindow();
                    }
                }
            }

        } else {
            if (pFoundWindow != PWINDOWIDEAL)
                pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);
        }

        if (pFoundWindow) {
            if (!pFoundWindow->m_isX11) {
                foundSurface = g_pCompositor->vectorWindowToSurface(mouseCoords, pFoundWindow, surfaceCoords);
                if (!foundSurface) {
                    foundSurface = pFoundWindow->m_wlSurface->resource();
                    surfacePos   = pFoundWindow->m_realPosition->value();
                }
            } else {
                foundSurface = pFoundWindow->m_wlSurface->resource();
                surfacePos   = pFoundWindow->m_realPosition->value();
            }
        }
    }

    // then surfaces below
    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], &surfaceCoords, &pFoundLayerSurface);

    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], &surfaceCoords, &pFoundLayerSurface);

    if (g_pPointerManager->softwareLockedFor(PMONITOR->m_self.lock()) > 0 && !skipFrameSchedule)
        g_pCompositor->scheduleFrameForMonitor(g_pCompositor->m_lastMonitor.lock(), Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);

    // FIXME: This will be disabled during DnD operations because we do not exactly follow the spec
    // xdg-popup grabs should be keyboard-only, while they are absolute in our case...
    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(foundSurface) && !PROTO::data->dndActive()) {
        if (m_hardInput || refocus) {
            g_pSeatManager->setGrab(nullptr);
            return; // setGrab will refocus
        } else {
            // we need to grab the last surface.
            foundSurface = g_pSeatManager->m_state.pointerFocus.lock();

            auto HLSurface = CWLSurface::fromResource(foundSurface);

            if (HLSurface) {
                const auto BOX = HLSurface->getSurfaceBoxGlobal();

                if (BOX.has_value())
                    surfacePos = BOX->pos();
            }
        }
    }

    if (!foundSurface) {
        if (!m_emptyFocusCursorSet) {
            if (*PRESIZEONBORDER && *PRESIZECURSORICON && m_borderIconDirection != BORDERICON_NONE) {
                m_borderIconDirection = BORDERICON_NONE;
                unsetCursorImage();
            }

            // TODO: maybe wrap?
            if (m_clickBehavior == CLICKMODE_KILL)
                setCursorImageOverride("crosshair");
            else
                setCursorImageOverride("left_ptr");

            m_emptyFocusCursorSet = true;
        }

        g_pSeatManager->setPointerFocus(nullptr, {});

        if (refocus || g_pCompositor->m_lastWindow.expired()) // if we are forcing a refocus, and we don't find a surface, clear the kb focus too!
            g_pCompositor->focusWindow(nullptr);

        return;
    }

    m_emptyFocusCursorSet = false;

    Vector2D surfaceLocal = surfacePos == Vector2D(-1337, -1337) ? surfaceCoords : mouseCoords - surfacePos;

    if (pFoundWindow && !pFoundWindow->m_isX11 && surfacePos != Vector2D(-1337, -1337)) {
        // calc for oversized windows... fucking bullshit.
        CBox geom = pFoundWindow->m_xdgSurface->m_current.geometry;

        surfaceLocal = mouseCoords - surfacePos + geom.pos();
    }

    if (pFoundWindow && pFoundWindow->m_isX11) // for x11 force scale zero
        surfaceLocal = surfaceLocal * pFoundWindow->m_X11SurfaceScaledBy;

    bool allowKeyboardRefocus = true;

    if (!refocus && g_pCompositor->m_lastFocus) {
        const auto PLS = g_pCompositor->getLayerSurfaceFromSurface(g_pCompositor->m_lastFocus.lock());

        if (PLS && PLS->m_layerSurface->m_current.interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
            allowKeyboardRefocus = false;
    }

    // set the values for use
    if (refocus) {
        m_foundLSToFocus      = pFoundLayerSurface;
        m_foundWindowToFocus  = pFoundWindow;
        m_foundSurfaceToFocus = foundSurface;
    }

    if (m_currentlyDraggedWindow.lock() && pFoundWindow != m_currentlyDraggedWindow) {
        g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);
        return;
    }

    if (pFoundWindow && foundSurface == pFoundWindow->m_wlSurface->resource() && !m_cursorImageOverridden) {
        const auto BOX = pFoundWindow->getWindowMainSurfaceBox();
        if (VECNOTINRECT(mouseCoords, BOX.x, BOX.y, BOX.x + BOX.width, BOX.y + BOX.height))
            setCursorImageOverride("left_ptr");
        else
            restoreCursorIconToApp();
    }

    if (pFoundWindow) {
        // change cursor icon if hovering over border
        if (*PRESIZEONBORDER && *PRESIZECURSORICON) {
            if (!pFoundWindow->isFullscreen() && !pFoundWindow->hasPopupAt(mouseCoords)) {
                setCursorIconOnBorder(pFoundWindow);
            } else if (m_borderIconDirection != BORDERICON_NONE) {
                unsetCursorImage();
            }
        }

        if (FOLLOWMOUSE != 1 && !refocus) {
            if (pFoundWindow != g_pCompositor->m_lastWindow.lock() && g_pCompositor->m_lastWindow.lock() &&
                ((pFoundWindow->m_isFloating && *PFLOATBEHAVIOR == 2) || (g_pCompositor->m_lastWindow->m_isFloating != pFoundWindow->m_isFloating && *PFLOATBEHAVIOR != 0))) {
                // enter if change floating style
                if (FOLLOWMOUSE != 3 && allowKeyboardRefocus)
                    g_pCompositor->focusWindow(pFoundWindow, foundSurface);
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);
            } else if (FOLLOWMOUSE == 2 || FOLLOWMOUSE == 3)
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);

            if (pFoundWindow == g_pCompositor->m_lastWindow)
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);

            if (FOLLOWMOUSE != 0 || pFoundWindow == g_pCompositor->m_lastWindow)
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);

            if (g_pSeatManager->m_state.pointerFocus == foundSurface)
                g_pSeatManager->sendPointerMotion(time, surfaceLocal);

            m_lastFocusOnLS = false;
            return; // don't enter any new surfaces
        } else {
            if (allowKeyboardRefocus && ((FOLLOWMOUSE != 3 && (*PMOUSEREFOCUS || m_lastMouseFocus.lock() != pFoundWindow)) || refocus)) {
                if (m_lastMouseFocus.lock() != pFoundWindow || g_pCompositor->m_lastWindow.lock() != pFoundWindow || g_pCompositor->m_lastFocus != foundSurface || refocus) {
                    m_lastMouseFocus = pFoundWindow;

                    // TODO: this looks wrong. When over a popup, it constantly is switching.
                    // Temp fix until that's figured out. Otherwise spams windowrule lookups and other shit.
                    if (m_lastMouseFocus.lock() != pFoundWindow || g_pCompositor->m_lastWindow.lock() != pFoundWindow) {
                        if (m_mousePosDelta > *PFOLLOWMOUSETHRESHOLD || refocus) {
                            const bool hasNoFollowMouse = pFoundWindow && pFoundWindow->m_windowData.noFollowMouse.valueOrDefault();

                            if (refocus || !hasNoFollowMouse)
                                g_pCompositor->focusWindow(pFoundWindow, foundSurface);
                        }
                    } else
                        g_pCompositor->focusSurface(foundSurface, pFoundWindow);
                }
            }
        }

        if (g_pSeatManager->m_state.keyboardFocus == nullptr)
            g_pCompositor->focusWindow(pFoundWindow, foundSurface);

        m_lastFocusOnLS = false;
    } else {
        if (*PRESIZEONBORDER && *PRESIZECURSORICON && m_borderIconDirection != BORDERICON_NONE) {
            m_borderIconDirection = BORDERICON_NONE;
            unsetCursorImage();
        }

        if (pFoundLayerSurface && (pFoundLayerSurface->m_layerSurface->m_current.interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) && FOLLOWMOUSE != 3 &&
            !(FOLLOWMOUSE == 2 && pFoundLayerSurface->m_layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM && (!refocus || sekaiLastWindowShown())) && // SEKAI_LAYER_HOVER
            (allowKeyboardRefocus || pFoundLayerSurface->m_layerSurface->m_current.interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)) {
            g_pCompositor->focusSurface(foundSurface);
        }

        if (pFoundLayerSurface)
            m_lastFocusOnLS = true;
    }

    g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);
    g_pSeatManager->sendPointerMotion(time, surfaceLocal);
}

static void sekaiStickyClick(); // SEKAI_A11Y_KEYS — 아래 고정 키 상태 옆에

void CInputManager::onMouseButton(IPointer::SButtonEvent e) {
    // SEKAI_MISCLICK: 창이 그린 제목줄 끌기의 놓기는 훅보다 먼저 — 작업 보기(hyprexpo) 등이 버튼 이벤트를
    //   취소하면 놓기가 사라져 창이 버튼 없이 커서를 따라다녔다
    if (sekaiMoving && e.state != WL_POINTER_BUTTON_STATE_PRESSED) { // SEKAI_CLIENT_MOVE — 놓음
        sekaiMoving = false;
        g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
        PHLMONITOR mon;
        const auto z = sekaiMoveZoneAt(getMouseCoordsInternal(), mon);
        if (const auto w = sekaiMoveWin.lock())
            g_pEventManager->postEvent(SHyprIPCEvent{"sekaisnapdrop", std::format("{},{},{:x}", z, mon ? mon->m_name : "", (uintptr_t)w.get())});
        else // SEKAI_CLIENT_MOVE2: 끄던 창이 닫혔다 — 셸이 끌기를 끝내게
            g_pEventManager->postEvent(SHyprIPCEvent{"sekaisnapdrop", "none,,0"});
        sekaiMoveZone = "none";
    }

    EMIT_HOOK_EVENT_CANCELLABLE("mouseButton", e);


    if (e.mouse)
        recheckMouseWarpOnMouseInput();

    m_lastCursorMovement.reset();

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // SEKAI_MISCLICK_DRAG: 버튼이 하나도 안 눌려 있었는데 끌기가 남아 있다 — 놓기를 놓쳤다(끄는 중 마우스 연결이
        //   끊김 등). 그대로 두면 Hyprland 가 새 끌기를 모두 조용히 거절해, 다시 로그인할 때까지 창을 옮길 수 없었다
        if (m_currentlyHeldButtons.empty() && (m_dragMode != MBIND_INVALID || !m_currentlyDraggedWindow.expired())) {
            g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
            if (sekaiMoving) {
                sekaiMoving = false;
                g_pEventManager->postEvent(SHyprIPCEvent{"sekaisnapdrop", "none,,0"});
            }
        }
        if (m_currentlyHeldButtons.empty())
            sekaiPressXY = getMouseCoordsInternal(); // SEKAI_CLIENT_MOVE3
        m_currentlyHeldButtons.push_back(e.button);
    } else {
        if (std::ranges::find_if(m_currentlyHeldButtons, [&](const auto& other) { return other == e.button; }) == m_currentlyHeldButtons.end())
            return;
        std::erase_if(m_currentlyHeldButtons, [&](const auto& other) { return other == e.button; });
    }

    sekaiButtonHeld = !m_currentlyHeldButtons.empty(); // SEKAI_CLIENT_MOVE2

    switch (m_clickBehavior) {
        case CLICKMODE_DEFAULT: processMouseDownNormal(e); break;
        case CLICKMODE_KILL: processMouseDownKill(e); break;
        default: break;
    }

    if (e.state == WL_POINTER_BUTTON_STATE_RELEASED)
        sekaiStickyClick(); // SEKAI_A11Y_KEYS: 걸린 수식 키는 클릭 한 번에도 풀린다 (Ctrl 걸고 클릭 = Ctrl+클릭, 윈도우처럼)

    if (m_focusHeldByButtons && m_currentlyHeldButtons.empty() && e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (m_refocusHeldByButtons)
            refocus();
        else
            simulateMouseMovement();

        m_focusHeldByButtons   = false;
        m_refocusHeldByButtons = false;
    }
}

void CInputManager::processMouseRequest(const CSeatManager::SSetCursorEvent& event) {
    const bool SEKAIDEFER = m_cursorImageOverridden && m_borderIconDirection != BORDERICON_NONE && m_clickBehavior != CLICKMODE_KILL; // SEKAI_BORDER_FIX: 테두리 커서 동안이면 적어만 둔다
    if (!cursorImageUnlocked() && !SEKAIDEFER)
        return;

    Debug::log(LOG, "cursorImage request: surface {:x}", (uintptr_t)event.surf.get());

    if (event.surf != m_cursorSurfaceInfo.wlSurface->resource()) {
        m_cursorSurfaceInfo.wlSurface->unassign();

        if (event.surf)
            m_cursorSurfaceInfo.wlSurface->assign(event.surf);
    }

    if (event.surf) {
        m_cursorSurfaceInfo.vHotspot = event.hotspot;
        m_cursorSurfaceInfo.hidden   = false;
    } else {
        m_cursorSurfaceInfo.vHotspot = {};
        m_cursorSurfaceInfo.hidden   = true;
    }

    m_cursorSurfaceInfo.name = "";

    m_cursorSurfaceInfo.inUse = !SEKAIDEFER; // 미뤘으면 restoreCursorIconToApp 가 띄운다
    if (SEKAIDEFER)
        return;
    g_pHyprRenderer->setCursorSurface(m_cursorSurfaceInfo.wlSurface, event.hotspot.x, event.hotspot.y);
}

void CInputManager::restoreCursorIconToApp() {
    if (m_cursorSurfaceInfo.inUse)
        return;

    if (m_cursorSurfaceInfo.hidden) {
        g_pHyprRenderer->setCursorSurface(nullptr, 0, 0);
        return;
    }

    if (m_cursorSurfaceInfo.name.empty()) {
        if (m_cursorSurfaceInfo.wlSurface->exists())
            g_pHyprRenderer->setCursorSurface(m_cursorSurfaceInfo.wlSurface, m_cursorSurfaceInfo.vHotspot.x, m_cursorSurfaceInfo.vHotspot.y);
    } else {
        g_pHyprRenderer->setCursorFromName(m_cursorSurfaceInfo.name);
    }

    m_cursorSurfaceInfo.inUse = true;
}

void CInputManager::setCursorImageOverride(const std::string& name) {
    if (m_cursorImageOverridden)
        return;

    m_cursorSurfaceInfo.inUse = false;
    g_pHyprRenderer->setCursorFromName(name);
}

bool CInputManager::cursorImageUnlocked() {
    if (m_clickBehavior == CLICKMODE_KILL)
        return false;

    if (m_cursorImageOverridden)
        return false;

    return true;
}

eClickBehaviorMode CInputManager::getClickMode() {
    return m_clickBehavior;
}

void CInputManager::setClickMode(eClickBehaviorMode mode) {
    switch (mode) {
        case CLICKMODE_DEFAULT:
            Debug::log(LOG, "SetClickMode: DEFAULT");
            m_clickBehavior = CLICKMODE_DEFAULT;
            g_pHyprRenderer->setCursorFromName("left_ptr");
            break;

        case CLICKMODE_KILL:
            Debug::log(LOG, "SetClickMode: KILL");
            m_clickBehavior = CLICKMODE_KILL;

            // remove constraints
            g_pInputManager->unconstrainMouse();
            refocus();

            // set cursor
            g_pHyprRenderer->setCursorFromName("crosshair");
            break;
        default: break;
    }
}

void CInputManager::processMouseDownNormal(const IPointer::SButtonEvent& e) {

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) // SEKAI_BORDER_GRAB — 새로 누를 때마다 (Super+끌기 크기 조절은 모서리대로)
        g_sekaiResizeAxis = 0;

    // notify the keybind manager
    static auto PPASSMOUSE        = CConfigValue<Hyprlang::INT>("binds:pass_mouse_when_bound");
    const auto  PASS              = g_pKeybindManager->onMouseEvent(e);
    static auto PFOLLOWMOUSE      = CConfigValue<Hyprlang::INT>("input:follow_mouse");
    static auto PRESIZEONBORDER   = CConfigValue<Hyprlang::INT>("general:resize_on_border");
    static auto PBORDERSIZE       = CConfigValue<Hyprlang::INT>("general:border_size");
    static auto PBORDERGRABEXTEND = CConfigValue<Hyprlang::INT>("general:extend_border_grab_area");
    const auto  BORDER_GRAB_AREA  = *PRESIZEONBORDER ? *PBORDERSIZE + *PBORDERGRABEXTEND : 0;

    if (!PASS && !*PPASSMOUSE)
        return;

    const auto mouseCoords = g_pInputManager->getMouseCoordsInternal();
    const auto w           = g_pCompositor->vectorToWindowUnified(mouseCoords, ALLOW_FLOATING | RESERVED_EXTENTS | INPUT_EXTENTS);

    // SEKAI_MODAL: 모달 대화상자가 떠 있는 창을 누르면 그 누름은 앱에 주지 않고 대화상자를 앞으로 (윈도우처럼)
    static uint32_t sekaiModalEaten = 0;
    if (e.state == WL_POINTER_BUTTON_STATE_RELEASED && sekaiModalEaten == e.button) {
        sekaiModalEaten = 0;
        return;
    }
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED && w && !g_pSessionLockManager->isSessionLocked() && !m_lastFocusOnLS) {
        if (const auto MODAL = w->sekaiModalChild(); MODAL) {
            g_pCompositor->focusWindow(MODAL);
            g_pCompositor->changeWindowZOrder(MODAL, true);
            sekaiModalEaten = e.button;
            return;
        }
    }

    // SEKAI_BORDER_GRAB: 테두리를 누르면 크기 조절 — 제목줄 맨 위도 위쪽 가장자리라 제목줄보다 먼저 본다
    if (*PRESIZEONBORDER && w && !w->isFullscreen() && !w->isX11OverrideRedirect() && !g_pSessionLockManager->isSessionLocked() && !m_lastFocusOnLS &&
        e.state == WL_POINTER_BUTTON_STATE_PRESSED && !w->hasPopupAt(mouseCoords) && (g_pSeatManager->m_mouse.expired() || !isConstrained()) /* SEKAI_BORDER_FIX */) {
        const int EDGE = sekaiBorderAt(w, mouseCoords, BORDER_GRAB_AREA);
        if (EDGE) {
            const bool H      = EDGE & 3, V = EDGE & 12;
            g_sekaiResizeAxis = H && V ? 0 : (V ? 1 : 2); // 가장자리만 잡았으면 그 방향으로만
            g_pKeybindManager->resizeWithBorder(e);
            return;
        }
    }

    if (w && !m_lastFocusOnLS && !g_pSessionLockManager->isSessionLocked() && w->checkInputOnDecos(INPUT_TYPE_BUTTON, mouseCoords, e))
        return;

    // (원래의 테두리 크기 조절은 위 SEKAI_BORDER_GRAB 이 대신한다)

    switch (e.state) {
        case WL_POINTER_BUTTON_STATE_PRESSED: {
            if (*PFOLLOWMOUSE == 3) // don't refocus on full loose
                break;

            // SEKAI_LAYER_HOVER: 창이 아닌 곳(바탕화면 레이어)을 누르면 그 레이어에 키보드 초점 — 올림으로는 안 준다
            if (!w && *PFOLLOWMOUSE == 2) {
                const auto PSURF = g_pSeatManager->m_state.pointerFocus.lock();
                const auto PLS   = PSURF ? g_pCompositor->getLayerSurfaceFromSurface(PSURF) : nullptr;
                if (PLS && PLS->m_layerSurface->m_current.interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
                    // SEKAI_MISCLICK: 바탕화면을 누르면 어느 창도 활성이 아니다 (윈도우처럼) — 안 그러면 작업 표시줄
                    //   단추가 그 창을 최소화하고, Alt+F4 가 보이지 않는 그 창을 닫았다
                    if (!g_pCompositor->m_lastWindow.expired())
                        g_pCompositor->focusWindow(nullptr);
                    if (g_pSeatManager->m_state.keyboardFocus != PSURF)
                        g_pCompositor->focusSurface(PSURF);
                }
            }

            // SEKAI_LAYER_FOCUS: 누른 창이 마지막 창이어도 키보드가 레이어(바탕화면 등)에 가 있으면 다시 초점
            const bool SEKAIKBONLAYER = w && g_pCompositor->getLayerSurfaceFromSurface(g_pSeatManager->m_state.keyboardFocus.lock());
            if ((g_pSeatManager->m_mouse.expired() || !isConstrained()) /* No constraints */
                && (w && (g_pCompositor->m_lastWindow.lock() != w || SEKAIKBONLAYER)) /* window should change */) {
                // a bit hacky
                // if we only pressed one button, allow us to refocus. m_lCurrentlyHeldButtons.size() > 0 will stick the focus
                if (m_currentlyHeldButtons.size() == 1) {
                    const auto COPY = m_currentlyHeldButtons;
                    m_currentlyHeldButtons.clear();
                    refocus();
                    m_currentlyHeldButtons = COPY;
                } else
                    refocus();
            }

            // if clicked on a floating window make it top
            if (!g_pSeatManager->m_state.pointerFocus)
                break;

            auto HLSurf = CWLSurface::fromResource(g_pSeatManager->m_state.pointerFocus.lock());

            if (HLSurf && HLSurf->getWindow())
                g_pCompositor->changeWindowZOrder(HLSurf->getWindow(), true);

            break;
        }
        case WL_POINTER_BUTTON_STATE_RELEASED: break;
    }

    // notify app if we didnt handle it
    g_pSeatManager->sendPointerButton(e.timeMs, e.button, e.state);

    if (const auto PMON = g_pCompositor->getMonitorFromVector(mouseCoords); PMON != g_pCompositor->m_lastMonitor && PMON)
        g_pCompositor->setActiveMonitor(PMON);

    if (g_pSeatManager->m_seatGrab && e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_hardInput = true;
        simulateMouseMovement();
        m_hardInput = false;
    }
}

void CInputManager::processMouseDownKill(const IPointer::SButtonEvent& e) {
    switch (e.state) {
        case WL_POINTER_BUTTON_STATE_PRESSED: {
            const auto PWINDOW = g_pCompositor->vectorToWindowUnified(getMouseCoordsInternal(), RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

            if (!PWINDOW) {
                Debug::log(ERR, "Cannot kill invalid window!");
                break;
            }

            // kill the mf
            kill(PWINDOW->getPID(), SIGKILL);
            break;
        }
        case WL_POINTER_BUTTON_STATE_RELEASED: break;
        default: break;
    }

    // reset click behavior mode
    m_clickBehavior = CLICKMODE_DEFAULT;
}

void CInputManager::onMouseWheel(IPointer::SAxisEvent e) {
    static auto POFFWINDOWAXIS        = CConfigValue<Hyprlang::INT>("input:off_window_axis_events");
    static auto PINPUTSCROLLFACTOR    = CConfigValue<Hyprlang::FLOAT>("input:scroll_factor");
    static auto PTOUCHPADSCROLLFACTOR = CConfigValue<Hyprlang::FLOAT>("input:touchpad:scroll_factor");
    static auto PEMULATEDISCRETE      = CConfigValue<Hyprlang::INT>("input:emulate_discrete_scroll");
    static auto PFOLLOWMOUSE          = CConfigValue<Hyprlang::INT>("input:follow_mouse");

    const bool  ISTOUCHPADSCROLL = *PTOUCHPADSCROLLFACTOR <= 0.f || e.source == WL_POINTER_AXIS_SOURCE_FINGER;
    auto        factor           = ISTOUCHPADSCROLL ? *PTOUCHPADSCROLLFACTOR : *PINPUTSCROLLFACTOR;

    const auto  EMAP = std::unordered_map<std::string, std::any>{{"event", e}};
    EMIT_HOOK_EVENT_CANCELLABLE("mouseAxis", EMAP);

    if (e.mouse)
        recheckMouseWarpOnMouseInput();

    bool passEvent = g_pKeybindManager->onAxisEvent(e);

    if (!passEvent)
        return;

    if (!m_lastFocusOnLS) {
        const auto MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
        const auto PWINDOW     = g_pCompositor->vectorToWindowUnified(MOUSECOORDS, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

        if (PWINDOW) {
            if (PWINDOW->checkInputOnDecos(INPUT_TYPE_AXIS, MOUSECOORDS, e))
                return;

            if (*POFFWINDOWAXIS != 1) {
                const auto BOX = PWINDOW->getWindowMainSurfaceBox();

                if (!BOX.containsPoint(MOUSECOORDS) && !PWINDOW->hasPopupAt(MOUSECOORDS)) {
                    if (*POFFWINDOWAXIS == 0)
                        return;

                    const auto TEMPCURX = std::clamp(MOUSECOORDS.x, BOX.x, BOX.x + BOX.w - 1);
                    const auto TEMPCURY = std::clamp(MOUSECOORDS.y, BOX.y, BOX.y + BOX.h - 1);

                    if (*POFFWINDOWAXIS == 3)
                        g_pCompositor->warpCursorTo({TEMPCURX, TEMPCURY}, true);

                    g_pSeatManager->sendPointerMotion(e.timeMs, Vector2D{TEMPCURX, TEMPCURY} - BOX.pos());
                    g_pSeatManager->sendPointerFrame();
                }
            }

            if (g_pSeatManager->m_state.pointerFocus) {
                const auto PCURRWINDOW = g_pCompositor->getWindowFromSurface(g_pSeatManager->m_state.pointerFocus.lock());

                if (*PFOLLOWMOUSE == 1 && PCURRWINDOW && PWINDOW != PCURRWINDOW)
                    simulateMouseMovement();
            }
            factor = ISTOUCHPADSCROLL ? PWINDOW->getScrollTouchpad() : PWINDOW->getScrollMouse();
        }
    }

    double discrete = (e.deltaDiscrete != 0) ? (factor * e.deltaDiscrete / std::abs(e.deltaDiscrete)) : 0;
    double delta    = e.delta * factor;

    if (e.source == 0) {
        // if an application supports v120, it should ignore discrete anyways
        if ((*PEMULATEDISCRETE >= 1 && std::abs(e.deltaDiscrete) != 120) || *PEMULATEDISCRETE >= 2) {

            const int interval = factor != 0 ? std::round(120 * (1 / factor)) : 120;

            // reset the accumulator when timeout is reached or direction/axis has changed
            if (std::signbit(e.deltaDiscrete) != m_scrollWheelState.lastEventSign || e.axis != m_scrollWheelState.lastEventAxis ||
                e.timeMs - m_scrollWheelState.lastEventTime > 500 /* 500ms taken from libinput default timeout */) {

                m_scrollWheelState.accumulatedScroll = 0;
                // send 1 discrete on first event for responsiveness
                discrete = std::copysign(1, e.deltaDiscrete);
            } else
                discrete = 0;

            for (int ac = m_scrollWheelState.accumulatedScroll; ac >= interval; ac -= interval) {
                discrete += std::copysign(1, e.deltaDiscrete);
                m_scrollWheelState.accumulatedScroll -= interval;
            }

            m_scrollWheelState.lastEventSign = std::signbit(e.deltaDiscrete);
            m_scrollWheelState.lastEventAxis = e.axis;
            m_scrollWheelState.lastEventTime = e.timeMs;
            m_scrollWheelState.accumulatedScroll += std::abs(e.deltaDiscrete);

            delta = 15.0 * discrete * factor;
        }
    }

    int32_t value120      = std::round(factor * e.deltaDiscrete);
    int32_t deltaDiscrete = std::abs(discrete) != 0 && std::abs(discrete) < 1 ? std::copysign(1, discrete) : std::round(discrete);

    g_pSeatManager->sendPointerAxis(e.timeMs, e.axis, delta, deltaDiscrete, value120, e.source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
}

Vector2D CInputManager::getMouseCoordsInternal() {
    return g_pPointerManager->position();
}

void CInputManager::newKeyboard(SP<Aquamarine::IKeyboard> keyboard) {
    const auto PNEWKEYBOARD = m_keyboards.emplace_back(CKeyboard::create(keyboard));

    setupKeyboard(PNEWKEYBOARD);

    Debug::log(LOG, "New keyboard created, pointers Hypr: {:x} and AQ: {:x}", (uintptr_t)PNEWKEYBOARD.get(), (uintptr_t)keyboard.get());
}

void CInputManager::newVirtualKeyboard(SP<CVirtualKeyboardV1Resource> keyboard) {
    const auto PNEWKEYBOARD = m_keyboards.emplace_back(CVirtualKeyboard::create(keyboard));

    setupKeyboard(PNEWKEYBOARD);

    Debug::log(LOG, "New virtual keyboard created at {:x}", (uintptr_t)PNEWKEYBOARD.get());
}

void CInputManager::setupKeyboard(SP<IKeyboard> keeb) {
    static auto PDPMS = CConfigValue<Hyprlang::INT>("misc:key_press_enables_dpms");

    m_hids.emplace_back(keeb);

    try {
        keeb->m_hlName = getNameForNewDevice(keeb->m_deviceName);
    } catch (std::exception& e) {
        Debug::log(ERR, "Keyboard had no name???"); // logic error
    }

    keeb->m_events.destroy.listenStatic([this, keeb = keeb.get()] {
        auto PKEEB = keeb->m_self.lock();

        if (!PKEEB)
            return;

        destroyKeyboard(PKEEB);
        Debug::log(LOG, "Destroyed keyboard {:x}", (uintptr_t)keeb);
    });

    keeb->m_keyboardEvents.key.listenStatic([this, keeb = keeb.get()](const IKeyboard::SKeyEvent& event) {
        auto PKEEB = keeb->m_self.lock();

        onKeyboardKey(event, PKEEB);

        if (PKEEB->m_enabled)
            PROTO::idle->onActivity();

        if (PKEEB->m_enabled && *PDPMS && !g_pCompositor->m_dpmsStateOn)
            g_pKeybindManager->dpms("on");
    });

    keeb->m_keyboardEvents.modifiers.listenStatic([this, keeb = keeb.get()] {
        auto PKEEB = keeb->m_self.lock();

        onKeyboardMod(PKEEB);

        if (PKEEB->m_enabled)
            PROTO::idle->onActivity();

        if (PKEEB->m_enabled && *PDPMS && !g_pCompositor->m_dpmsStateOn)
            g_pKeybindManager->dpms("on");
    });

    keeb->m_keyboardEvents.keymap.listenStatic([keeb = keeb.get()] {
        auto       PKEEB  = keeb->m_self.lock();
        const auto LAYOUT = PKEEB->getActiveLayout();

        if (PKEEB == g_pSeatManager->m_keyboard) {
            g_pSeatManager->updateActiveKeyboardData();
            g_pKeybindManager->m_keyToCodeCache.clear();
        }

        g_pEventManager->postEvent(SHyprIPCEvent{"activelayout", PKEEB->m_hlName + "," + LAYOUT});
        EMIT_HOOK_EVENT("activeLayout", (std::vector<std::any>{PKEEB, LAYOUT}));
    });

    disableAllKeyboards(false);

    applyConfigToKeyboard(keeb);

    g_pSeatManager->setKeyboard(keeb);

    keeb->updateLEDs();
}

void CInputManager::setKeyboardLayout() {
    for (auto const& k : m_keyboards)
        applyConfigToKeyboard(k);

    g_pKeybindManager->updateXKBTranslationState();
}

void CInputManager::applyConfigToKeyboard(SP<IKeyboard> pKeyboard) {
    auto       devname = pKeyboard->m_hlName;

    const auto HASCONFIG = g_pConfigManager->deviceConfigExists(devname);

    Debug::log(LOG, "ApplyConfigToKeyboard for \"{}\", hasconfig: {}", devname, (int)HASCONFIG);

    const auto REPEATRATE  = g_pConfigManager->getDeviceInt(devname, "repeat_rate", "input:repeat_rate");
    const auto REPEATDELAY = g_pConfigManager->getDeviceInt(devname, "repeat_delay", "input:repeat_delay");

    const auto NUMLOCKON         = g_pConfigManager->getDeviceInt(devname, "numlock_by_default", "input:numlock_by_default");
    const auto RESOLVEBINDSBYSYM = g_pConfigManager->getDeviceInt(devname, "resolve_binds_by_sym", "input:resolve_binds_by_sym");

    const auto FILEPATH = g_pConfigManager->getDeviceString(devname, "kb_file", "input:kb_file");
    const auto RULES    = g_pConfigManager->getDeviceString(devname, "kb_rules", "input:kb_rules");
    const auto MODEL    = g_pConfigManager->getDeviceString(devname, "kb_model", "input:kb_model");
    const auto LAYOUT   = g_pConfigManager->getDeviceString(devname, "kb_layout", "input:kb_layout");
    const auto VARIANT  = g_pConfigManager->getDeviceString(devname, "kb_variant", "input:kb_variant");
    const auto OPTIONS  = g_pConfigManager->getDeviceString(devname, "kb_options", "input:kb_options");

    const auto ENABLED    = HASCONFIG ? g_pConfigManager->getDeviceInt(devname, "enabled") : true;
    const auto ALLOWBINDS = HASCONFIG ? g_pConfigManager->getDeviceInt(devname, "keybinds") : true;

    pKeyboard->m_enabled           = ENABLED;
    pKeyboard->m_resolveBindsBySym = RESOLVEBINDSBYSYM;
    pKeyboard->m_allowBinds        = ALLOWBINDS;

    const auto PERM = g_pDynamicPermissionManager->clientPermissionModeWithString(-1, pKeyboard->m_hlName, PERMISSION_TYPE_KEYBOARD);
    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING) {
        const auto PROMISE = g_pDynamicPermissionManager->promiseFor(-1, pKeyboard->m_hlName, PERMISSION_TYPE_KEYBOARD);
        if (!PROMISE)
            Debug::log(ERR, "BUG THIS: No promise for client permission for keyboard");
        else {
            PROMISE->then([k = WP<IKeyboard>{pKeyboard}](SP<CPromiseResult<eDynamicPermissionAllowMode>> r) {
                if (r->hasError()) {
                    Debug::log(ERR, "BUG THIS: No permission returned for keyboard");
                    return;
                }

                if (!k)
                    return;

                k->m_allowed = r->result() == PERMISSION_RULE_ALLOW_MODE_ALLOW;
            });
        }
    } else
        pKeyboard->m_allowed = PERM == PERMISSION_RULE_ALLOW_MODE_ALLOW;

    try {
        if (NUMLOCKON == pKeyboard->m_numlockOn && REPEATDELAY == pKeyboard->m_repeatDelay && REPEATRATE == pKeyboard->m_repeatRate && !RULES.empty() &&
            RULES == pKeyboard->m_currentRules.rules && MODEL == pKeyboard->m_currentRules.model && LAYOUT == pKeyboard->m_currentRules.layout &&
            VARIANT == pKeyboard->m_currentRules.variant && OPTIONS == pKeyboard->m_currentRules.options && FILEPATH == pKeyboard->m_xkbFilePath) {
            Debug::log(LOG, "Not applying config to keyboard, it did not change.");
            return;
        }
    } catch (std::exception& e) {
        // can be libc errors for null std::string
        // we can ignore those and just apply
    }

    pKeyboard->m_repeatRate  = std::max(0, REPEATRATE);
    pKeyboard->m_repeatDelay = std::max(0, REPEATDELAY);
    pKeyboard->m_numlockOn   = NUMLOCKON;
    pKeyboard->m_xkbFilePath = FILEPATH;

    pKeyboard->setKeymap(IKeyboard::SStringRuleNames{LAYOUT, MODEL, VARIANT, OPTIONS, RULES});

    const auto LAYOUTSTR = pKeyboard->getActiveLayout();

    g_pEventManager->postEvent(SHyprIPCEvent{"activelayout", pKeyboard->m_hlName + "," + LAYOUTSTR});
    EMIT_HOOK_EVENT("activeLayout", (std::vector<std::any>{pKeyboard, LAYOUTSTR}));

    Debug::log(LOG, "Set the keyboard layout to {} and variant to {} for keyboard \"{}\"", pKeyboard->m_currentRules.layout, pKeyboard->m_currentRules.variant,
               pKeyboard->m_hlName);
}

void CInputManager::newVirtualMouse(SP<CVirtualPointerV1Resource> mouse) {
    const auto PMOUSE = m_pointers.emplace_back(CVirtualPointer::create(mouse));

    setupMouse(PMOUSE);

    Debug::log(LOG, "New virtual mouse created");
}

void CInputManager::newMouse(SP<Aquamarine::IPointer> mouse) {
    const auto PMOUSE = m_pointers.emplace_back(CMouse::create(mouse));

    setupMouse(PMOUSE);

    Debug::log(LOG, "New mouse created, pointer AQ: {:x}", (uintptr_t)mouse.get());
}

void CInputManager::setupMouse(SP<IPointer> mauz) {
    m_hids.emplace_back(mauz);

    try {
        mauz->m_hlName = getNameForNewDevice(mauz->m_deviceName);
    } catch (std::exception& e) {
        Debug::log(ERR, "Mouse had no name???"); // logic error
    }

    if (mauz->aq() && mauz->aq()->getLibinputHandle()) {
        const auto LIBINPUTDEV = mauz->aq()->getLibinputHandle();

        Debug::log(LOG, "New mouse has libinput sens {:.2f} ({:.2f}) with accel profile {} ({})", libinput_device_config_accel_get_speed(LIBINPUTDEV),
                   libinput_device_config_accel_get_default_speed(LIBINPUTDEV), (int)libinput_device_config_accel_get_profile(LIBINPUTDEV),
                   (int)libinput_device_config_accel_get_default_profile(LIBINPUTDEV));
    }

    g_pPointerManager->attachPointer(mauz);

    mauz->m_connected = true;

    setPointerConfigs();

    mauz->m_events.destroy.listenStatic([this, PMOUSE = mauz.get()] { destroyPointer(PMOUSE->m_self.lock()); });

    g_pSeatManager->setMouse(mauz);

    m_lastCursorMovement.reset();
}

void CInputManager::setPointerConfigs() {
    for (auto const& m : m_pointers) {
        auto       devname = m->m_hlName;

        const auto HASCONFIG = g_pConfigManager->deviceConfigExists(devname);

        if (HASCONFIG) {
            const auto ENABLED = g_pConfigManager->getDeviceInt(devname, "enabled");
            if (ENABLED && !m->m_connected) {
                g_pPointerManager->attachPointer(m);
                m->m_connected = true;
            } else if (!ENABLED && m->m_connected) {
                g_pPointerManager->detachPointer(m);
                m->m_connected = false;
            }
        }

        if (m->aq() && m->aq()->getLibinputHandle()) {
            const auto LIBINPUTDEV = m->aq()->getLibinputHandle();

            double     touchw = 0, touchh = 0;
            const auto ISTOUCHPAD = libinput_device_has_capability(LIBINPUTDEV, LIBINPUT_DEVICE_CAP_POINTER) &&
                libinput_device_get_size(LIBINPUTDEV, &touchw, &touchh) == 0; // pointer with size is a touchpad

            if (g_pConfigManager->getDeviceInt(devname, "clickfinger_behavior", "input:touchpad:clickfinger_behavior") == 0) // toggle software buttons or clickfinger
                libinput_device_config_click_set_method(LIBINPUTDEV, LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS);
            else
                libinput_device_config_click_set_method(LIBINPUTDEV, LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);

            if (g_pConfigManager->getDeviceInt(devname, "left_handed", "input:left_handed") == 0)
                libinput_device_config_left_handed_set(LIBINPUTDEV, 0);
            else
                libinput_device_config_left_handed_set(LIBINPUTDEV, 1);

            if (libinput_device_config_middle_emulation_is_available(LIBINPUTDEV)) { // middleclick on r+l mouse button pressed
                if (g_pConfigManager->getDeviceInt(devname, "middle_button_emulation", "input:touchpad:middle_button_emulation") == 1)
                    libinput_device_config_middle_emulation_set_enabled(LIBINPUTDEV, LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED);
                else
                    libinput_device_config_middle_emulation_set_enabled(LIBINPUTDEV, LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);

                const auto TAP_MAP = g_pConfigManager->getDeviceString(devname, "tap_button_map", "input:touchpad:tap_button_map");
                if (TAP_MAP.empty() || TAP_MAP == "lrm")
                    libinput_device_config_tap_set_button_map(LIBINPUTDEV, LIBINPUT_CONFIG_TAP_MAP_LRM);
                else if (TAP_MAP == "lmr")
                    libinput_device_config_tap_set_button_map(LIBINPUTDEV, LIBINPUT_CONFIG_TAP_MAP_LMR);
                else
                    Debug::log(WARN, "Tap button mapping unknown");
            }

            const auto SCROLLMETHOD = g_pConfigManager->getDeviceString(devname, "scroll_method", "input:scroll_method");
            if (SCROLLMETHOD.empty()) {
                libinput_device_config_scroll_set_method(LIBINPUTDEV, libinput_device_config_scroll_get_default_method(LIBINPUTDEV));
            } else if (SCROLLMETHOD == "no_scroll") {
                libinput_device_config_scroll_set_method(LIBINPUTDEV, LIBINPUT_CONFIG_SCROLL_NO_SCROLL);
            } else if (SCROLLMETHOD == "2fg") {
                libinput_device_config_scroll_set_method(LIBINPUTDEV, LIBINPUT_CONFIG_SCROLL_2FG);
            } else if (SCROLLMETHOD == "edge") {
                libinput_device_config_scroll_set_method(LIBINPUTDEV, LIBINPUT_CONFIG_SCROLL_EDGE);
            } else if (SCROLLMETHOD == "on_button_down") {
                libinput_device_config_scroll_set_method(LIBINPUTDEV, LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN);
            } else {
                Debug::log(WARN, "Scroll method unknown");
            }

            if (g_pConfigManager->getDeviceInt(devname, "tap-and-drag", "input:touchpad:tap-and-drag") == 0)
                libinput_device_config_tap_set_drag_enabled(LIBINPUTDEV, LIBINPUT_CONFIG_DRAG_DISABLED);
            else
                libinput_device_config_tap_set_drag_enabled(LIBINPUTDEV, LIBINPUT_CONFIG_DRAG_ENABLED);

            const auto TAP_DRAG_LOCK = g_pConfigManager->getDeviceInt(devname, "drag_lock", "input:touchpad:drag_lock");
            if (TAP_DRAG_LOCK >= 0 && TAP_DRAG_LOCK <= 2) {
                libinput_device_config_tap_set_drag_lock_enabled(LIBINPUTDEV, static_cast<libinput_config_drag_lock_state>(TAP_DRAG_LOCK));
            }

            if (libinput_device_config_tap_get_finger_count(LIBINPUTDEV)) // this is for tapping (like on a laptop)
                libinput_device_config_tap_set_enabled(LIBINPUTDEV,
                                                       g_pConfigManager->getDeviceInt(devname, "tap-to-click", "input:touchpad:tap-to-click") == 1 ? LIBINPUT_CONFIG_TAP_ENABLED :
                                                                                                                                                     LIBINPUT_CONFIG_TAP_DISABLED);

            if (libinput_device_config_scroll_has_natural_scroll(LIBINPUTDEV)) {

                if (ISTOUCHPAD)
                    libinput_device_config_scroll_set_natural_scroll_enabled(LIBINPUTDEV,
                                                                             g_pConfigManager->getDeviceInt(devname, "natural_scroll", "input:touchpad:natural_scroll"));
                else
                    libinput_device_config_scroll_set_natural_scroll_enabled(LIBINPUTDEV, g_pConfigManager->getDeviceInt(devname, "natural_scroll", "input:natural_scroll"));
            }

            if (libinput_device_config_3fg_drag_get_finger_count(LIBINPUTDEV) >= 3) {
                const auto DRAG_3FG_STATE = static_cast<libinput_config_3fg_drag_state>(g_pConfigManager->getDeviceInt(devname, "drag_3fg", "input:touchpad:drag_3fg"));
                libinput_device_config_3fg_drag_set_enabled(LIBINPUTDEV, DRAG_3FG_STATE);
            }

            if (libinput_device_config_dwt_is_available(LIBINPUTDEV)) {
                const auto DWT =
                    static_cast<enum libinput_config_dwt_state>(g_pConfigManager->getDeviceInt(devname, "disable_while_typing", "input:touchpad:disable_while_typing") != 0);
                libinput_device_config_dwt_set_enabled(LIBINPUTDEV, DWT);
            }

            const auto LIBINPUTSENS = std::clamp(g_pConfigManager->getDeviceFloat(devname, "sensitivity", "input:sensitivity"), -1.f, 1.f);
            libinput_device_config_accel_set_speed(LIBINPUTDEV, LIBINPUTSENS);

            m->m_flipX = g_pConfigManager->getDeviceInt(devname, "flip_x", "input:touchpad:flip_x") != 0;
            m->m_flipY = g_pConfigManager->getDeviceInt(devname, "flip_y", "input:touchpad:flip_y") != 0;

            const auto ACCELPROFILE = g_pConfigManager->getDeviceString(devname, "accel_profile", "input:accel_profile");
            const auto SCROLLPOINTS = g_pConfigManager->getDeviceString(devname, "scroll_points", "input:scroll_points");

            if (ACCELPROFILE.empty()) {
                libinput_device_config_accel_set_profile(LIBINPUTDEV, libinput_device_config_accel_get_default_profile(LIBINPUTDEV));
            } else if (ACCELPROFILE == "adaptive") {
                libinput_device_config_accel_set_profile(LIBINPUTDEV, LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE);
            } else if (ACCELPROFILE == "flat") {
                libinput_device_config_accel_set_profile(LIBINPUTDEV, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
            } else if (ACCELPROFILE.starts_with("custom")) {
                CVarList accelValues = {ACCELPROFILE, 0, ' '};

                try {
                    double              accelStep = std::stod(accelValues[1]);
                    std::vector<double> accelPoints;
                    for (size_t i = 2; i < accelValues.size(); ++i) {
                        accelPoints.push_back(std::stod(accelValues[i]));
                    }

                    const auto CONFIG = libinput_config_accel_create(LIBINPUT_CONFIG_ACCEL_PROFILE_CUSTOM);

                    if (!SCROLLPOINTS.empty()) {
                        CVarList scrollValues = {SCROLLPOINTS, 0, ' '};
                        try {
                            double              scrollStep = std::stod(scrollValues[0]);
                            std::vector<double> scrollPoints;
                            for (size_t i = 1; i < scrollValues.size(); ++i) {
                                scrollPoints.push_back(std::stod(scrollValues[i]));
                            }

                            libinput_config_accel_set_points(CONFIG, LIBINPUT_ACCEL_TYPE_SCROLL, scrollStep, scrollPoints.size(), scrollPoints.data());
                        } catch (std::exception& e) { Debug::log(ERR, "Invalid values in scroll_points"); }
                    }

                    libinput_config_accel_set_points(CONFIG, LIBINPUT_ACCEL_TYPE_MOTION, accelStep, accelPoints.size(), accelPoints.data());
                    libinput_device_config_accel_apply(LIBINPUTDEV, CONFIG);
                    libinput_config_accel_destroy(CONFIG);
                } catch (std::exception& e) { Debug::log(ERR, "Invalid values in custom accel profile"); }
            } else {
                Debug::log(WARN, "Unknown acceleration profile, falling back to default");
            }

            const auto SCROLLBUTTON = g_pConfigManager->getDeviceInt(devname, "scroll_button", "input:scroll_button");

            libinput_device_config_scroll_set_button(LIBINPUTDEV, SCROLLBUTTON == 0 ? libinput_device_config_scroll_get_default_button(LIBINPUTDEV) : SCROLLBUTTON);

            const auto SCROLLBUTTONLOCK = g_pConfigManager->getDeviceInt(devname, "scroll_button_lock", "input:scroll_button_lock");

            libinput_device_config_scroll_set_button_lock(LIBINPUTDEV,
                                                          SCROLLBUTTONLOCK == 0 ? LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_DISABLED : LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED);

            Debug::log(LOG, "Applied config to mouse {}, sens {:.2f}", m->m_hlName, LIBINPUTSENS);
        }
    }
}

static void removeFromHIDs(WP<IHID> hid) {
    std::erase_if(g_pInputManager->m_hids, [hid](const auto& e) { return e.expired() || e == hid; });
    g_pInputManager->updateCapabilities();
}

void CInputManager::destroyKeyboard(SP<IKeyboard> pKeyboard) {
    Debug::log(LOG, "Keyboard at {:x} removed", (uintptr_t)pKeyboard.get());

    std::erase_if(m_keyboards, [pKeyboard](const auto& other) { return other == pKeyboard; });

    if (!m_keyboards.empty()) {
        bool found = false;
        for (auto const& k : m_keyboards | std::views::reverse) {
            if (!k)
                continue;

            g_pSeatManager->setKeyboard(k);
            found = true;
            break;
        }

        if (!found)
            g_pSeatManager->setKeyboard(nullptr);
    } else
        g_pSeatManager->setKeyboard(nullptr);

    removeFromHIDs(pKeyboard);
}

void CInputManager::destroyPointer(SP<IPointer> mouse) {
    Debug::log(LOG, "Pointer at {:x} removed", (uintptr_t)mouse.get());

    std::erase_if(m_pointers, [mouse](const auto& other) { return other == mouse; });

    g_pSeatManager->setMouse(!m_pointers.empty() ? m_pointers.front() : nullptr);

    if (!g_pSeatManager->m_mouse.expired())
        unconstrainMouse();

    removeFromHIDs(mouse);
}

void CInputManager::destroyTouchDevice(SP<ITouch> touch) {
    Debug::log(LOG, "Touch device at {:x} removed", (uintptr_t)touch.get());

    std::erase_if(m_touches, [touch](const auto& other) { return other == touch; });

    removeFromHIDs(touch);
}

void CInputManager::destroyTablet(SP<CTablet> tablet) {
    Debug::log(LOG, "Tablet device at {:x} removed", (uintptr_t)tablet.get());

    std::erase_if(m_tablets, [tablet](const auto& other) { return other == tablet; });

    removeFromHIDs(tablet);
}

void CInputManager::destroyTabletTool(SP<CTabletTool> tool) {
    Debug::log(LOG, "Tablet tool at {:x} removed", (uintptr_t)tool.get());

    std::erase_if(m_tabletTools, [tool](const auto& other) { return other == tool; });

    removeFromHIDs(tool);
}

void CInputManager::destroyTabletPad(SP<CTabletPad> pad) {
    Debug::log(LOG, "Tablet pad at {:x} removed", (uintptr_t)pad.get());

    std::erase_if(m_tabletPads, [pad](const auto& other) { return other == pad; });

    removeFromHIDs(pad);
}

void CInputManager::updateKeyboardsLeds(SP<IKeyboard> pKeyboard) {
    if (!pKeyboard || pKeyboard->isVirtual())
        return;

    std::optional<uint32_t> leds = pKeyboard->getLEDs();

    if (!leds.has_value())
        return;

    for (auto const& k : m_keyboards) {
        k->updateLEDs(leds.value());
    }
}

// ── SEKAI_A11Y_KEYS: 고정 키 · 필터 키 (윈도우의 접근성 › 키보드) ─────────────────────
//   고정 키: 수식 키를 혼자 눌렀다 떼면 다음 키 하나에 걸린다(래치), 한 번 더 누르면 계속(잠금), 또 누르면 풀린다.
//     걸린 수식 키는 accumulateModsFromAllKBs 에 더해진다 — 앱(wl_keyboard.modifiers)과 단축키(Win 다음 E) 모두
//   필터 키: 반복 입력 무시(같은 키를 N ms 안에 다시 누르면 버림) · 누르고 있어야 입력(N ms 눌러야 들어감)
//   Shift 다섯 번 · 오른쪽 Shift 8초 → IPC sekaia11y>>sticky|filter — 셸이 켤지 묻는다
//   가상 키보드(화상 키보드)는 빼고 진짜 키보드만
static uint32_t                                     g_sekaiStickyLatched = 0, g_sekaiStickyLocked = 0;
static uint32_t                                     g_sekaiStickyDown    = 0;
static bool                                         g_sekaiStickyUsed    = false; // 수식 키를 누른 채 다른 키 — 그때는 걸지 않는다
static int                                          g_sekaiShiftTaps     = 0;
static Time::steady_tp                              g_sekaiShiftTapAt, g_sekaiRShiftAt;
static bool                                         g_sekaiRShiftDown = false;
using SSekaiKeyId = std::pair<const IKeyboard*, uint32_t>;           // 키보드마다 따로 (키보드 여럿이 서로 간섭하지 않게)
static std::map<SSekaiKeyId, Time::steady_tp>        g_sekaiLastUp;   // 반복 입력 무시 — 키마다 마지막으로 뗀 때
static std::set<SSekaiKeyId>                         g_sekaiDropped;  // 버린 누름 — 그 뗌도 버린다 (앱이 누름 없는 뗌을 받지 않게)
struct SSekaiSlowKey {
    IKeyboard::SKeyEvent ev;
    WP<IKeyboard>        kb;
};
static std::optional<SSekaiSlowKey> g_sekaiSlow;
static SP<CEventLoopTimer>          g_sekaiSlowTimer;
static bool                         g_sekaiSlowPass = false;

static uint32_t sekaiModOf(SP<IKeyboard> kb, uint32_t keycode) {
    if (!kb || !kb->m_xkbState)
        return 0;
    switch (xkb_state_key_get_one_sym(kb->m_xkbState, keycode + 8)) {
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R: return HL_MODIFIER_SHIFT;
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R: return HL_MODIFIER_CTRL;
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R: return HL_MODIFIER_ALT;
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R: return HL_MODIFIER_META;
        default: return 0;   // 한/영(ralt_hangul)·한자 키는 수식 키가 아니다
    }
}

static void sekaiStickyPost() {
    g_pEventManager->postEvent(SHyprIPCEvent{"sekaisticky", std::format("{},{}", g_sekaiStickyLatched, g_sekaiStickyLocked)});
}

static void sekaiStickyClick() {
    if (!g_sekaiStickyLatched)
        return;
    g_sekaiStickyLatched = 0;
    if (const auto KB = g_pSeatManager->m_keyboard.lock(); KB)
        g_pInputManager->onKeyboardMod(KB);
    sekaiStickyPost();
}

void CInputManager::onKeyboardKey(const IKeyboard::SKeyEvent& event, SP<IKeyboard> pKeyboard) {
    if (!pKeyboard->m_enabled || !pKeyboard->m_allowed)
        return;

    const bool DISALLOWACTION = pKeyboard->isVirtual() && shouldIgnoreVirtualKeyboard(pKeyboard);

    // SEKAI_DRAG_CANCEL: 창을 끌거나 크기를 바꾸는 중 Esc — 처음대로 되돌린다 (윈도우처럼). 그 Esc 는 앱에 주지 않는다
    static bool sekaiEatEscUp = false;
    if (event.keycode == 1 /* KEY_ESC */) {
        if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED && !m_currentlyDraggedWindow.expired() && g_pLayoutManager->getCurrentLayout() &&
            g_pLayoutManager->getCurrentLayout()->sekaiCancelDrag()) {
            if (sekaiMoving) {
                sekaiMoving   = false;
                sekaiMoveZone = "none";
            }
            sekaiEatEscUp = true;
            return;
        }
        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED && sekaiEatEscUp) {
            sekaiEatEscUp = false;
            return;
        }
    }

    // SEKAI_A11Y_KEYS
    static auto PSTICKY   = CConfigValue<Hyprlang::INT>("input:sekai_sticky_keys");
    static auto PBOUNCE   = CConfigValue<Hyprlang::INT>("input:sekai_bounce_keys");
    static auto PSLOW     = CConfigValue<Hyprlang::INT>("input:sekai_slow_keys");
    static auto PA11YKEYS = CConfigValue<Hyprlang::INT>("input:sekai_a11y_shortcuts");
    bool        sekaiClearLatch = false;
    if (!pKeyboard->isVirtual()) {
        const bool     PRESSED = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        const uint32_t MOD     = sekaiModOf(pKeyboard, event.keycode);
        const auto     NOW     = Time::steadyNow();
        if (*PA11YKEYS && !g_sekaiSlowPass) {
            if (MOD == HL_MODIFIER_SHIFT && PRESSED) {
                if (NOW - g_sekaiShiftTapAt > std::chrono::seconds(2))
                    g_sekaiShiftTaps = 0;
                g_sekaiShiftTapAt = NOW;
                if (++g_sekaiShiftTaps >= 5) {
                    g_sekaiShiftTaps = 0;
                    g_pEventManager->postEvent(SHyprIPCEvent{"sekaia11y", "sticky"});
                }
            } else if (PRESSED && MOD != HL_MODIFIER_SHIFT)
                g_sekaiShiftTaps = 0;
            if (event.keycode == 54 /* KEY_RIGHTSHIFT */) {
                if (PRESSED && !g_sekaiRShiftDown) {
                    g_sekaiRShiftDown = true;
                    g_sekaiRShiftAt   = NOW;
                } else if (!PRESSED && g_sekaiRShiftDown) {
                    g_sekaiRShiftDown = false;
                    if (NOW - g_sekaiRShiftAt >= std::chrono::seconds(8))
                        g_pEventManager->postEvent(SHyprIPCEvent{"sekaia11y", "filter"});
                }
            }
        }
        // 반복 입력 무시 — 같은 키를 방금 뗐는데 또 누르면 (손떨림) 버린다
        const SSekaiKeyId KID{pKeyboard.get(), event.keycode};
        // 버린 누름의 뗌은 (반복 입력 무시·누르고 있어야 입력 어느 쪽이 버렸든) 같이 버린다
        if (!PRESSED && !g_sekaiSlowPass && g_sekaiDropped.erase(KID))
            return;
        if (*PBOUNCE > 0 && !MOD && !g_sekaiSlowPass) {
            if (PRESSED) {
                const auto IT = g_sekaiLastUp.find(KID);
                if (IT != g_sekaiLastUp.end() && NOW - IT->second < std::chrono::milliseconds(*PBOUNCE)) {
                    g_sekaiDropped.insert(KID);
                    return;
                }
            } else
                g_sekaiLastUp[KID] = NOW;
        }
        // 누르고 있어야 입력 — N ms 동안 눌려 있어야 그 누름을 넘긴다. 그 전에 떼면 둘 다 버린다
        if (*PSLOW > 0 && !MOD && !g_sekaiSlowPass) {
            if (PRESSED) {
                if (!g_sekaiSlowTimer) {
                    g_sekaiSlowTimer = makeShared<CEventLoopTimer>(
                        std::nullopt,
                        [](SP<CEventLoopTimer> self, void* data) {
                            if (!g_sekaiSlow)
                                return;
                            const auto S = *g_sekaiSlow;
                            g_sekaiSlow.reset();
                            if (const auto KB = S.kb.lock(); KB) {
                                g_sekaiSlowPass = true;
                                g_pInputManager->onKeyboardKey(S.ev, KB);
                                g_sekaiSlowPass = false;
                            }
                        },
                        nullptr);
                    g_pEventLoopManager->addTimer(g_sekaiSlowTimer);
                }
                // 기다리던 다른 키는 버린다 — 윈도우처럼 한 번에 한 키만 (그 키의 뗌도 버려 앱에 누름 없는 뗌이 가지 않게)
                if (g_sekaiSlow && (g_sekaiSlow->ev.keycode != event.keycode || g_sekaiSlow->kb.lock() != pKeyboard))
                    g_sekaiDropped.insert({g_sekaiSlow->kb.lock().get(), g_sekaiSlow->ev.keycode});
                g_sekaiSlow = SSekaiSlowKey{event, pKeyboard};
                g_sekaiSlowTimer->updateTimeout(std::chrono::milliseconds(*PSLOW));
                return;
            } else if (g_sekaiSlow && g_sekaiSlow->ev.keycode == event.keycode && g_sekaiSlow->kb.lock() == pKeyboard) {
                g_sekaiSlow.reset();
                g_sekaiSlowTimer->updateTimeout(std::nullopt);
                return;
            }
        }
        if (*PSTICKY) {
            if (MOD) {
                if (PRESSED) {
                    if (!g_sekaiStickyDown)
                        g_sekaiStickyUsed = false;
                    g_sekaiStickyDown |= MOD;
                } else {
                    g_sekaiStickyDown &= ~MOD;
                    if (!g_sekaiStickyUsed) {
                        if (g_sekaiStickyLocked & MOD)
                            g_sekaiStickyLocked &= ~MOD;
                        else if (g_sekaiStickyLatched & MOD) {
                            g_sekaiStickyLatched &= ~MOD;
                            g_sekaiStickyLocked |= MOD;
                        } else
                            g_sekaiStickyLatched |= MOD;
                        sekaiStickyPost(); // 앱에 보낼 수식 키는 뒤따르는 onKeyboardMod 가 (걸린 것을 더해) 보낸다
                    }
                }
            } else if (PRESSED) {
                g_sekaiStickyUsed = true;
                sekaiClearLatch   = g_sekaiStickyLatched != 0; // 이 키를 넘긴 뒤 래치를 푼다
            }
        }
    }

    // SEKAI_A11Y_MONITOR: 화면 읽기(Orca)가 가로챈 키는 단축키·앱에 넘기지 않는다 (SekaiA11yMonitor.cpp)
    if (!DISALLOWACTION && SekaiA11y::onKey(event, pKeyboard))
        return;

    const auto EMAP = std::unordered_map<std::string, std::any>{{"keyboard", pKeyboard}, {"event", event}};
    EMIT_HOOK_EVENT_CANCELLABLE("keyPress", EMAP);

    bool passEvent = DISALLOWACTION || g_pKeybindManager->onKeyEvent(event, pKeyboard);

    if (passEvent) {
        const auto IME = m_relay.m_inputMethod.lock();

        if (IME && IME->hasGrab() && !DISALLOWACTION) {
            IME->setKeyboard(pKeyboard);
            IME->sendKey(event.timeMs, event.keycode, event.state);
        } else {
            g_pSeatManager->setKeyboard(pKeyboard);
            g_pSeatManager->sendKeyboardKey(event.timeMs, event.keycode, event.state);
        }

        updateKeyboardsLeds(pKeyboard);
    }

    if (sekaiClearLatch) { // SEKAI_A11Y_KEYS: 래치는 키 하나에만 — 풀고 수식 키를 다시 보낸다
        g_sekaiStickyLatched = 0;
        onKeyboardMod(pKeyboard);
        sekaiStickyPost();
    }
}

void CInputManager::onKeyboardMod(SP<IKeyboard> pKeyboard) {
    if (!pKeyboard->m_enabled)
        return;

    const bool DISALLOWACTION = pKeyboard->isVirtual() && shouldIgnoreVirtualKeyboard(pKeyboard);

    const auto ALLMODS = accumulateModsFromAllKBs();

    auto       MODS = pKeyboard->m_modifiersState;
    MODS.depressed  = ALLMODS;

    const auto IME = m_relay.m_inputMethod.lock();

    if (IME && IME->hasGrab() && !DISALLOWACTION) {
        IME->setKeyboard(pKeyboard);
        IME->sendMods(MODS.depressed, MODS.latched, MODS.locked, MODS.group);
    } else {
        g_pSeatManager->setKeyboard(pKeyboard);
        g_pSeatManager->sendKeyboardMods(MODS.depressed, MODS.latched, MODS.locked, MODS.group);
    }

    updateKeyboardsLeds(pKeyboard);

    if (pKeyboard->m_modifiersState.group != pKeyboard->m_activeLayout) {
        pKeyboard->m_activeLayout = pKeyboard->m_modifiersState.group;

        const auto LAYOUT = pKeyboard->getActiveLayout();

        Debug::log(LOG, "LAYOUT CHANGED TO {} GROUP {}", LAYOUT, MODS.group);

        g_pEventManager->postEvent(SHyprIPCEvent{"activelayout", pKeyboard->m_hlName + "," + LAYOUT});
        EMIT_HOOK_EVENT("activeLayout", (std::vector<std::any>{pKeyboard, LAYOUT}));
    }
}

bool CInputManager::shouldIgnoreVirtualKeyboard(SP<IKeyboard> pKeyboard) {
    if (!pKeyboard->isVirtual())
        return false;

    CVirtualKeyboard* vk = (CVirtualKeyboard*)pKeyboard.get();

    return !pKeyboard || (!m_relay.m_inputMethod.expired() && m_relay.m_inputMethod->grabClient() == vk->getClient());
}

void CInputManager::refocus() {
    mouseMoveUnified(0, true);
}

bool CInputManager::refocusLastWindow(PHLMONITOR pMonitor) {
    if (!m_exclusiveLSes.empty()) {
        Debug::log(LOG, "CInputManager::refocusLastWindow: ignoring, exclusive LS present.");
        return false;
    }

    if (!pMonitor) {
        refocus();
        return true;
    }

    Vector2D               surfaceCoords;
    PHLLS                  pFoundLayerSurface;
    SP<CWLSurfaceResource> foundSurface = nullptr;

    g_pInputManager->releaseAllMouseButtons();

    // then any surfaces above windows on the same monitor
    if (!foundSurface) {
        foundSurface = g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
                                                           &surfaceCoords, &pFoundLayerSurface);
        if (pFoundLayerSurface && pFoundLayerSurface->m_interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
            foundSurface = nullptr;
    }

    if (!foundSurface) {
        foundSurface = g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP],
                                                           &surfaceCoords, &pFoundLayerSurface);
        if (pFoundLayerSurface && pFoundLayerSurface->m_interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
            foundSurface = nullptr;
    }

    if (!foundSurface && g_pCompositor->m_lastWindow.lock() && g_pCompositor->m_lastWindow->m_workspace && g_pCompositor->m_lastWindow->m_workspace->isVisibleNotCovered()) {
        // then the last focused window if we're on the same workspace as it
        const auto PLASTWINDOW = g_pCompositor->m_lastWindow.lock();
        g_pCompositor->focusWindow(PLASTWINDOW);
    } else {
        // otherwise fall back to a normal refocus.

        if (foundSurface && !foundSurface->m_hlSurface->keyboardFocusable()) {
            const auto PLASTWINDOW = g_pCompositor->m_lastWindow.lock();
            g_pCompositor->focusWindow(PLASTWINDOW);
        }

        refocus();
    }

    return true;
}

void CInputManager::unconstrainMouse() {
    if (g_pSeatManager->m_mouse.expired())
        return;

    for (auto const& c : m_constraints) {
        const auto C = c.lock();

        if (!C)
            continue;

        if (!C->isActive())
            continue;

        C->deactivate();
    }
}

bool CInputManager::isConstrained() {
    return std::ranges::any_of(m_constraints, [](auto const& c) {
        const auto constraint = c.lock();
        return constraint && constraint->isActive() && constraint->owner()->resource() == g_pCompositor->m_lastFocus;
    });
}

bool CInputManager::isLocked() {
    if (!isConstrained())
        return false;

    const auto SURF       = CWLSurface::fromResource(g_pCompositor->m_lastFocus.lock());
    const auto CONSTRAINT = SURF ? SURF->constraint() : nullptr;

    return CONSTRAINT && CONSTRAINT->isLocked();
}

void CInputManager::updateCapabilities() {
    uint32_t caps = 0;

    for (auto const& h : m_hids) {
        if (h.expired())
            continue;

        caps |= h->getCapabilities();
    }

    g_pSeatManager->updateCapabilities(caps);
    m_capabilities = caps;
}

uint32_t CInputManager::accumulateModsFromAllKBs() {

    uint32_t finalMask = 0;

    for (auto const& kb : m_keyboards) {
        if (kb->isVirtual() && shouldIgnoreVirtualKeyboard(kb))
            continue;

        if (!kb->m_enabled)
            continue;

        finalMask |= kb->getModifiers();
    }

    // SEKAI_A11Y_KEYS: 고정 키로 걸린 수식 키 (끄면 풀린다)
    static auto PSTICKY = CConfigValue<Hyprlang::INT>("input:sekai_sticky_keys");
    if (*PSTICKY)
        finalMask |= g_sekaiStickyLatched | g_sekaiStickyLocked;
    else if (g_sekaiStickyLatched || g_sekaiStickyLocked) {
        g_sekaiStickyLatched = g_sekaiStickyLocked = 0;
        sekaiStickyPost();
    }

    return finalMask;
}

void CInputManager::disableAllKeyboards(bool virt) {

    for (auto const& k : m_keyboards) {
        if (k->isVirtual() != virt)
            continue;

        k->m_active = false;
    }
}

void CInputManager::newTouchDevice(SP<Aquamarine::ITouch> pDevice) {
    const auto PNEWDEV = m_touches.emplace_back(CTouchDevice::create(pDevice));
    m_hids.emplace_back(PNEWDEV);

    try {
        PNEWDEV->m_hlName = getNameForNewDevice(PNEWDEV->m_deviceName);
    } catch (std::exception& e) {
        Debug::log(ERR, "Touch Device had no name???"); // logic error
    }

    setTouchDeviceConfigs(PNEWDEV);
    g_pPointerManager->attachTouch(PNEWDEV);

    PNEWDEV->m_events.destroy.listenStatic([this, dev = PNEWDEV.get()] {
        auto PDEV = dev->m_self.lock();

        if (!PDEV)
            return;

        destroyTouchDevice(PDEV);
    });

    Debug::log(LOG, "New touch device added at {:x}", (uintptr_t)PNEWDEV.get());
}

void CInputManager::setTouchDeviceConfigs(SP<ITouch> dev) {
    auto setConfig = [](SP<ITouch> PTOUCHDEV) -> void {
        if (PTOUCHDEV->aq() && PTOUCHDEV->aq()->getLibinputHandle()) {
            const auto LIBINPUTDEV = PTOUCHDEV->aq()->getLibinputHandle();

            const auto ENABLED = g_pConfigManager->getDeviceInt(PTOUCHDEV->m_hlName, "enabled", "input:touchdevice:enabled");
            const auto mode    = ENABLED ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
            if (libinput_device_config_send_events_get_mode(LIBINPUTDEV) != mode)
                libinput_device_config_send_events_set_mode(LIBINPUTDEV, mode);

            if (libinput_device_config_calibration_has_matrix(LIBINPUTDEV)) {
                Debug::log(LOG, "Setting calibration matrix for device {}", PTOUCHDEV->m_hlName);
                // default value of transform being -1 means it's unset.
                const int ROTATION = std::clamp(g_pConfigManager->getDeviceInt(PTOUCHDEV->m_hlName, "transform", "input:touchdevice:transform"), -1, 7);
                if (ROTATION > -1)
                    libinput_device_config_calibration_set_matrix(LIBINPUTDEV, MATRICES[ROTATION]);
            }

            auto       output     = g_pConfigManager->getDeviceString(PTOUCHDEV->m_hlName, "output", "input:touchdevice:output");
            bool       bound      = !output.empty() && output != STRVAL_EMPTY;
            const bool AUTODETECT = output == "[[Auto]]";
            if (!bound && AUTODETECT) {
                // FIXME:
                // const auto DEFAULTOUTPUT = PTOUCHDEV->wlr()->output_name;
                // if (DEFAULTOUTPUT) {
                //     output = DEFAULTOUTPUT;
                //     bound  = true;
                // }
            }
            PTOUCHDEV->m_boundOutput = bound ? output : "";
            const auto PMONITOR      = bound ? g_pCompositor->getMonitorFromName(output) : nullptr;
            if (PMONITOR) {
                Debug::log(LOG, "Binding touch device {} to output {}", PTOUCHDEV->m_hlName, PMONITOR->m_name);
                // wlr_cursor_map_input_to_output(g_pCompositor->m_sWLRCursor, &PTOUCHDEV->wlr()->base, PMONITOR->output);
            } else if (bound)
                Debug::log(ERR, "Failed to bind touch device {} to output '{}': monitor not found", PTOUCHDEV->m_hlName, output);
        }
    };

    if (dev) {
        setConfig(dev);
        return;
    }

    for (auto const& m : m_touches) {
        setConfig(m);
    }
}

void CInputManager::setTabletConfigs() {
    for (auto const& t : m_tablets) {
        if (t->aq()->getLibinputHandle()) {
            const auto NAME        = t->m_hlName;
            const auto LIBINPUTDEV = t->aq()->getLibinputHandle();

            const auto RELINPUT = g_pConfigManager->getDeviceInt(NAME, "relative_input", "input:tablet:relative_input");
            t->m_relativeInput  = RELINPUT;

            const int ROTATION = std::clamp(g_pConfigManager->getDeviceInt(NAME, "transform", "input:tablet:transform"), -1, 7);
            Debug::log(LOG, "Setting calibration matrix for device {}", NAME);
            if (ROTATION > -1)
                libinput_device_config_calibration_set_matrix(LIBINPUTDEV, MATRICES[ROTATION]);

            if (g_pConfigManager->getDeviceInt(NAME, "left_handed", "input:tablet:left_handed") == 0)
                libinput_device_config_left_handed_set(LIBINPUTDEV, 0);
            else
                libinput_device_config_left_handed_set(LIBINPUTDEV, 1);

            const auto OUTPUT = g_pConfigManager->getDeviceString(NAME, "output", "input:tablet:output");
            if (OUTPUT != STRVAL_EMPTY) {
                Debug::log(LOG, "Binding tablet {} to output {}", NAME, OUTPUT);
                t->m_boundOutput = OUTPUT;
            } else
                t->m_boundOutput = "";

            const auto REGION_POS  = g_pConfigManager->getDeviceVec(NAME, "region_position", "input:tablet:region_position");
            const auto REGION_SIZE = g_pConfigManager->getDeviceVec(NAME, "region_size", "input:tablet:region_size");
            t->m_boundBox          = {REGION_POS, REGION_SIZE};

            const auto ABSOLUTE_REGION_POS = g_pConfigManager->getDeviceInt(NAME, "absolute_region_position", "input:tablet:absolute_region_position");
            t->m_absolutePos               = ABSOLUTE_REGION_POS;

            const auto ACTIVE_AREA_SIZE = g_pConfigManager->getDeviceVec(NAME, "active_area_size", "input:tablet:active_area_size");
            const auto ACTIVE_AREA_POS  = g_pConfigManager->getDeviceVec(NAME, "active_area_position", "input:tablet:active_area_position");
            if (ACTIVE_AREA_SIZE.x != 0 || ACTIVE_AREA_SIZE.y != 0) {
                t->m_activeArea = CBox{ACTIVE_AREA_POS.x / t->aq()->physicalSize.x, ACTIVE_AREA_POS.y / t->aq()->physicalSize.y,
                                       (ACTIVE_AREA_POS.x + ACTIVE_AREA_SIZE.x) / t->aq()->physicalSize.x, (ACTIVE_AREA_POS.y + ACTIVE_AREA_SIZE.y) / t->aq()->physicalSize.y};
            }
        }
    }
}

void CInputManager::newSwitch(SP<Aquamarine::ISwitch> pDevice) {
    const auto PNEWDEV = &m_switches.emplace_back();
    PNEWDEV->pDevice   = pDevice;

    Debug::log(LOG, "New switch with name \"{}\" added", pDevice->getName());

    PNEWDEV->listeners.destroy = pDevice->events.destroy.listen([this, PNEWDEV] { destroySwitch(PNEWDEV); });

    PNEWDEV->listeners.fire = pDevice->events.fire.listen([PNEWDEV](const Aquamarine::ISwitch::SFireEvent& event) {
        const auto NAME = PNEWDEV->pDevice->getName();

        Debug::log(LOG, "Switch {} fired, triggering binds.", NAME);

        g_pKeybindManager->onSwitchEvent(NAME);

        if (event.enable) {
            Debug::log(LOG, "Switch {} turn on, triggering binds.", NAME);
            g_pKeybindManager->onSwitchOnEvent(NAME);
        } else {
            Debug::log(LOG, "Switch {} turn off, triggering binds.", NAME);
            g_pKeybindManager->onSwitchOffEvent(NAME);
        }
    });
}

void CInputManager::destroySwitch(SSwitchDevice* pDevice) {
    m_switches.remove(*pDevice);
}

void CInputManager::setCursorImageUntilUnset(std::string name) {
    g_pHyprRenderer->setCursorFromName(name);
    m_cursorImageOverridden   = true;
    m_cursorSurfaceInfo.inUse = false;
}

void CInputManager::unsetCursorImage() {
    if (!m_cursorImageOverridden)
        return;

    m_cursorImageOverridden = false;
    restoreCursorIconToApp();
}

std::string CInputManager::deviceNameToInternalString(std::string in) {
    std::ranges::replace(in, ' ', '-');
    std::ranges::replace(in, '\n', '-');
    std::ranges::transform(in, in.begin(), ::tolower);
    return in;
}

std::string CInputManager::getNameForNewDevice(std::string internalName) {

    auto proposedNewName = deviceNameToInternalString(internalName);
    int  dupeno          = 0;

    auto makeNewName = [&]() { return (proposedNewName.empty() ? "unknown-device" : proposedNewName) + (dupeno == 0 ? "" : ("-" + std::to_string(dupeno))); };

    while (std::ranges::find_if(m_hids, [&](const auto& other) { return other->m_hlName == makeNewName(); }) != m_hids.end())
        dupeno++;

    return makeNewName();
}

void CInputManager::releaseAllMouseButtons() {
    const auto buttonsCopy = m_currentlyHeldButtons;

    if (PROTO::data->dndActive())
        return;

    for (auto const& mb : buttonsCopy) {
        g_pSeatManager->sendPointerButton(Time::millis(Time::steadyNow()), mb, WL_POINTER_BUTTON_STATE_RELEASED);
    }

    m_currentlyHeldButtons.clear();
}

void CInputManager::setCursorIconOnBorder(PHLWINDOW w) {
    // SEKAI_BORDER_GRAB: 끄는 중(마우스 바인드)일 때만 건드리지 않는다.
    //   원래 조건이 거꾸로(expired)라 테두리 위에서 커서가 한 번도 바뀌지 않았다
    if (!g_pInputManager->m_currentlyDraggedWindow.expired()) {
        m_borderIconDirection = BORDERICON_NONE; // SEKAI_BORDER_FIX: 끝나면 다음 움직임에 다시 판정
        return;
    }

    // ignore X11 OR windows, they shouldn't be touched
    if (w->m_isX11 && w->isX11OverrideRedirect())
        return;

    static auto          PEXTENDBORDERGRAB = CConfigValue<Hyprlang::INT>("general:extend_border_grab_area");
    const auto           mouseCoords       = getMouseCoordsInternal();
    eBorderIconDirection direction         = BORDERICON_NONE;

    if (!w->hasPopupAt(mouseCoords) && m_currentlyHeldButtons.empty()) {
        switch (sekaiBorderAt(w, mouseCoords, w->getRealBorderSize() + *PEXTENDBORDERGRAB)) { // 누를 때와 같은 판정
            case 1: direction = BORDERICON_LEFT; break;
            case 2: direction = BORDERICON_RIGHT; break;
            case 4: direction = BORDERICON_UP; break;
            case 8: direction = BORDERICON_DOWN; break;
            case 5: direction = BORDERICON_UP_LEFT; break;
            case 6: direction = BORDERICON_UP_RIGHT; break;
            case 9: direction = BORDERICON_DOWN_LEFT; break;
            case 10: direction = BORDERICON_DOWN_RIGHT; break;
            default: break;
        }
    }

    if (direction == m_borderIconDirection)
        return;

    m_borderIconDirection = direction;

    switch (direction) {
        case BORDERICON_NONE: unsetCursorImage(); break;
        case BORDERICON_UP: setCursorImageUntilUnset("ns-resize"); break;
        case BORDERICON_DOWN: setCursorImageUntilUnset("ns-resize"); break;
        case BORDERICON_LEFT: setCursorImageUntilUnset("ew-resize"); break;
        case BORDERICON_RIGHT: setCursorImageUntilUnset("ew-resize"); break;
        case BORDERICON_UP_LEFT: setCursorImageUntilUnset("nwse-resize"); break;
        case BORDERICON_DOWN_LEFT: setCursorImageUntilUnset("nesw-resize"); break;
        case BORDERICON_UP_RIGHT: setCursorImageUntilUnset("nesw-resize"); break;
        case BORDERICON_DOWN_RIGHT: setCursorImageUntilUnset("nwse-resize"); break;
    }
}

void CInputManager::recheckMouseWarpOnMouseInput() {
    static auto PWARPFORNONMOUSE = CConfigValue<Hyprlang::INT>("cursor:warp_back_after_non_mouse_input");

    if (!m_lastInputMouse && *PWARPFORNONMOUSE)
        g_pPointerManager->warpTo(m_lastMousePos);
}
