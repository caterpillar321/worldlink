#include "WLSurface.hpp"
#include "LayerSurface.hpp"
#include "../desktop/Window.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/LayerShell.hpp"
#include "../render/Renderer.hpp"
#include "../protocols/XDGShell.hpp" // SEKAI_GEOM_DAMAGE: 창 영역(geometry)

void CWLSurface::assign(SP<CWLSurfaceResource> pSurface) {
    m_resource = pSurface;
    init();
    m_inert = false;
}

void CWLSurface::assign(SP<CWLSurfaceResource> pSurface, PHLWINDOW pOwner) {
    m_windowOwner = pOwner;
    m_resource    = pSurface;
    init();
    m_inert = false;
}

void CWLSurface::assign(SP<CWLSurfaceResource> pSurface, PHLLS pOwner) {
    m_layerOwner = pOwner;
    m_resource   = pSurface;
    init();
    m_inert = false;
}

void CWLSurface::assign(SP<CWLSurfaceResource> pSurface, CSubsurface* pOwner) {
    m_subsurfaceOwner = pOwner;
    m_resource        = pSurface;
    init();
    m_inert = false;
}

void CWLSurface::assign(SP<CWLSurfaceResource> pSurface, CPopup* pOwner) {
    m_popupOwner = pOwner;
    m_resource   = pSurface;
    init();
    m_inert = false;
}

void CWLSurface::unassign() {
    destroy();
}

CWLSurface::~CWLSurface() {
    destroy();
}

bool CWLSurface::exists() const {
    return m_resource;
}

SP<CWLSurfaceResource> CWLSurface::resource() const {
    return m_resource.lock();
}

bool CWLSurface::small() const {
    if (!validMapped(m_windowOwner) || !exists())
        return false;

    if (!m_resource->m_current.texture)
        return false;

    const auto O = m_windowOwner.lock();

    return O->m_reportedSize.x > m_resource->m_current.size.x + 1 || O->m_reportedSize.y > m_resource->m_current.size.y + 1;
}

Vector2D CWLSurface::correctSmallVec() const {
    if (!validMapped(m_windowOwner) || !exists() || !small() || m_fillIgnoreSmall)
        return {};

    const auto SIZE = getViewporterCorrectedSize();
    const auto O    = m_windowOwner.lock();

    return Vector2D{(O->m_reportedSize.x - SIZE.x) / 2, (O->m_reportedSize.y - SIZE.y) / 2}.clamp({}, {INFINITY, INFINITY}) * (O->m_realSize->value() / O->m_reportedSize);
}

Vector2D CWLSurface::correctSmallVecBuf() const {
    if (!exists() || !small() || m_fillIgnoreSmall || !m_resource->m_current.texture)
        return {};

    const auto SIZE = getViewporterCorrectedSize();
    const auto BS   = m_resource->m_current.bufferSize;

    return Vector2D{(BS.x - SIZE.x) / 2, (BS.y - SIZE.y) / 2}.clamp({}, {INFINITY, INFINITY});
}

Vector2D CWLSurface::getViewporterCorrectedSize() const {
    if (!exists() || !m_resource->m_current.texture)
        return {};

    return m_resource->m_current.viewport.hasDestination ? m_resource->m_current.viewport.destination : m_resource->m_current.bufferSize;
}

CRegion CWLSurface::computeDamage() const {
    if (!m_resource->m_current.texture)
        return {};

    CRegion damage = m_resource->m_current.accumulateBufferDamage();
    damage.transform(wlTransformToHyprutils(m_resource->m_current.transform), m_resource->m_current.bufferSize.x, m_resource->m_current.bufferSize.y);

    const auto BUFSIZE    = m_resource->m_current.bufferSize;
    const auto CORRECTVEC = correctSmallVecBuf();

    if (m_resource->m_current.viewport.hasSource)
        damage.intersect(m_resource->m_current.viewport.source);

    const auto SCALEDSRCSIZE =
        m_resource->m_current.viewport.hasSource ? m_resource->m_current.viewport.source.size() * m_resource->m_current.scale : m_resource->m_current.bufferSize;

    damage.scale({BUFSIZE.x / SCALEDSRCSIZE.x, BUFSIZE.y / SCALEDSRCSIZE.y});
    damage.translate(CORRECTVEC);

    // go from buffer coords in the damage to hl logical

    const auto     BOX   = getSurfaceBoxGlobal();

    // SEKAI_GEOM_DAMAGE: 창의 주 면은 xdg 창 영역(geometry)만 잘라 창 자리에 그린다 (Renderer calculateUVForSurface) —
    //   다시 그릴 범위도 같게: 버퍼 → 면 좌표 → 창 영역 시작점만큼 빼고 → 창 영역 크기를 창 크기에 맞춘다.
    //   원본은 버퍼 전체를 창 크기에 눌러 담아, 그림자를 두는 앱(크롬·VS Code)에선 범위가 그림자 폭만큼 어긋나
    //   마우스 올림 동그라미가 가장자리만 남았다 (원본은 모든 창에 "최대화됨"을 알려 그림자가 없어 드러나지 않는다)
    if (const auto W = m_windowOwner.lock(); W && BOX.has_value() && !W->m_isX11 && W->m_xdgSurface && W->m_wlSurface && W->m_wlSurface.get() == this &&
        !m_resource->m_current.viewport.hasSource) {
        const auto G    = W->m_xdgSurface->m_current.geometry;
        const auto SURF = m_resource->m_current.size;
        const auto BUF  = m_resource->m_current.bufferSize;
        if (G.width > 0 && G.height > 0 && SURF.x > 0 && SURF.y > 0 && BUF.x > 0 && BUF.y > 0 && (G.x != 0 || G.y != 0 || G.width != SURF.x || G.height != SURF.y)) {
            damage.scale(SURF / BUF);
            damage.translate(-G.pos());
            damage.scale(BOX->size() / G.size());
            damage.expand(1); // 소수 자리 반올림으로 한 줄이 빠지지 않게
            return damage;
        }
    }
    const Vector2D SCALE = BOX.has_value() ? BOX->size() / m_resource->m_current.bufferSize :
                                             Vector2D{1.0 / m_resource->m_current.scale, 1.0 / m_resource->m_current.scale /* Wrong... but we can't really do better */};

    damage.scale(SCALE);

    if (m_windowOwner)
        damage.scale(m_windowOwner->m_X11SurfaceScaledBy); // fix xwayland:force_zero_scaling stuff that will be fucked by the above a bit

    return damage;
}

void CWLSurface::destroy() {
    if (!m_resource)
        return;

    m_events.destroy.emit();

    m_constraint.reset();

    m_listeners.destroy.reset();
    m_resource->m_hlSurface.reset();
    m_windowOwner.reset();
    m_layerOwner.reset();
    m_popupOwner      = nullptr;
    m_subsurfaceOwner = nullptr;
    m_inert           = true;

    if (g_pHyprRenderer && g_pHyprRenderer->m_lastCursorData.surf && g_pHyprRenderer->m_lastCursorData.surf->get() == this)
        g_pHyprRenderer->m_lastCursorData.surf.reset();

    m_resource.reset();

    Debug::log(LOG, "CWLSurface {:x} called destroy()", (uintptr_t)this);
}

void CWLSurface::init() {
    if (!m_resource)
        return;

    RASSERT(!m_resource->m_hlSurface, "Attempted to duplicate CWLSurface ownership!");

    m_resource->m_hlSurface = m_self.lock();

    m_listeners.destroy = m_resource->m_events.destroy.listen([this] { destroy(); });

    Debug::log(LOG, "CWLSurface {:x} called init()", (uintptr_t)this);
}

PHLWINDOW CWLSurface::getWindow() const {
    return m_windowOwner.lock();
}

PHLLS CWLSurface::getLayer() const {
    return m_layerOwner.lock();
}

CPopup* CWLSurface::getPopup() const {
    return m_popupOwner;
}

CSubsurface* CWLSurface::getSubsurface() const {
    return m_subsurfaceOwner;
}

bool CWLSurface::desktopComponent() const {
    return !m_layerOwner.expired() || !m_windowOwner.expired() || m_subsurfaceOwner || m_popupOwner;
}

std::optional<CBox> CWLSurface::getSurfaceBoxGlobal() const {
    if (!desktopComponent())
        return {};

    if (!m_windowOwner.expired())
        return m_windowOwner->getWindowMainSurfaceBox();
    if (!m_layerOwner.expired())
        return m_layerOwner->m_geometry;
    if (m_popupOwner)
        return CBox{m_popupOwner->coordsGlobal(), m_popupOwner->size()};
    if (m_subsurfaceOwner)
        return CBox{m_subsurfaceOwner->coordsGlobal(), m_subsurfaceOwner->size()};

    return {};
}

void CWLSurface::appendConstraint(WP<CPointerConstraint> constraint) {
    m_constraint = constraint;
}

SP<CPointerConstraint> CWLSurface::constraint() const {
    return m_constraint.lock();
}

bool CWLSurface::visible() {
    if (!m_windowOwner.expired())
        return g_pHyprRenderer->shouldRenderWindow(m_windowOwner.lock());
    if (!m_layerOwner.expired())
        return true;
    if (m_popupOwner)
        return m_popupOwner->visible();
    if (m_subsurfaceOwner)
        return m_subsurfaceOwner->visible();
    return true; // non-desktop, we don't know much.
}

SP<CWLSurface> CWLSurface::fromResource(SP<CWLSurfaceResource> pSurface) {
    if (!pSurface)
        return nullptr;
    return pSurface->m_hlSurface.lock();
}

bool CWLSurface::keyboardFocusable() const {
    // SekaiOS: a layer surface's subsurface follows the layer's keyboard interactivity (before, it had no owner)
    if (m_subsurfaceOwner) {
        if (const auto LS = m_subsurfaceOwner->layerParent())
            return LS->m_layerSurface && LS->m_layerSurface->m_current.interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
        return true;
    }
    if (m_windowOwner || m_popupOwner)
        return true;
    if (m_layerOwner && m_layerOwner->m_layerSurface)
        return m_layerOwner->m_layerSurface->m_current.interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    return false;
}
