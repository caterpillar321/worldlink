#include "ServerDecorationKDE.hpp"
#include <algorithm>
#include "core/Compositor.hpp"
#include "../Compositor.hpp"
#include "../desktop/Window.hpp"

CServerDecorationKDE::CServerDecorationKDE(SP<COrgKdeKwinServerDecoration> resource_, SP<CWLSurfaceResource> surf) : m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setRelease([this](COrgKdeKwinServerDecoration* pMgr) { PROTO::serverDecorationKDE->destroyResource(this); });
    m_resource->setOnDestroy([this](COrgKdeKwinServerDecoration* pMgr) { PROTO::serverDecorationKDE->destroyResource(this); });

    m_surface = surf;
    // SEKAI_CLIENT_DECO: 앱이 고른 모드를 기억만 한다 — 원본처럼 답하지 않는다 (server 는 만들 때 한 번 보냈다).
    //   요청마다 server 로 답하면 client 를 원하는 GTK3 창(헤더바 — 파일 탐색기 등)이 그 답을 받고 다시 client 를
    //   요청해 초당 수만 번 핑퐁하며 앱과 합성기가 쉬지 않고 CPU 를 썼다
    m_resource->setRequestMode([this](COrgKdeKwinServerDecoration*, uint32_t mode) {
        const bool CLIENT = mode == ORG_KDE_KWIN_SERVER_DECORATION_MODE_CLIENT;
        if (m_sekaiClient == CLIENT)
            return;
        m_sekaiClient = CLIENT;
        if (const auto S = m_surface.lock()) {
            if (const auto W = g_pCompositor->getWindowFromSurface(S); W && W->m_isMapped)
                W->updateDynamicRules();
        }
    });

    m_resource->sendMode(ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

bool CServerDecorationKDEProtocol::sekaiWantsClient(SP<CWLSurfaceResource> surf) {
    for (auto const& d : m_decos) {
        if (d->m_surface.lock() == surf)
            return d->m_sekaiClient;
    }
    return false;
}

bool CServerDecorationKDEProtocol::sekaiHas(SP<CWLSurfaceResource> surf) {
    return std::ranges::any_of(m_decos, [&](const auto& d) { return d->m_surface.lock() == surf; });
}

bool CServerDecorationKDE::good() {
    return m_resource->resource();
}

CServerDecorationKDEProtocol::CServerDecorationKDEProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CServerDecorationKDEProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<COrgKdeKwinServerDecorationManager>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](COrgKdeKwinServerDecorationManager* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setCreate([this](COrgKdeKwinServerDecorationManager* pMgr, uint32_t id, wl_resource* pointer) { this->createDecoration(pMgr, id, pointer); });

    // send default mode of SSD, as Hyprland will never ask for CSD. Screw Gnome and GTK.
    RESOURCE->sendDefaultMode(ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

void CServerDecorationKDEProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CServerDecorationKDEProtocol::destroyResource(CServerDecorationKDE* hayperlaaaand) {
    std::erase_if(m_decos, [&](const auto& other) { return other.get() == hayperlaaaand; });
}

void CServerDecorationKDEProtocol::createDecoration(COrgKdeKwinServerDecorationManager* pMgr, uint32_t id, wl_resource* surf) {
    const auto CLIENT = pMgr->client();
    const auto RESOURCE =
        m_decos.emplace_back(makeUnique<CServerDecorationKDE>(makeShared<COrgKdeKwinServerDecoration>(CLIENT, pMgr->version(), id), CWLSurfaceResource::fromResource(surf))).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_decos.pop_back();
        return;
    }
}
