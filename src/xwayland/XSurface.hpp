#pragma once

#include "../helpers/signal/Signal.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/math/Math.hpp"
#include <vector>

class CWLSurfaceResource;
class CXWaylandSurfaceResource;

#ifdef NO_XWAYLAND
using xcb_pixmap_t         = uint32_t;
using xcb_window_t         = uint32_t;
using xcb_icccm_wm_hints_t = struct {
    int32_t      flags;
    uint32_t     input;
    int32_t      initial_state;
    xcb_pixmap_t icon_pixmap;
    xcb_window_t icon_window;
    int32_t      icon_x, icon_y;
    xcb_pixmap_t icon_mask;
    xcb_window_t window_group;
};
using xcb_size_hints_t = struct {
    uint32_t flags;
    int32_t  x, y;
    int32_t  width, height;
    int32_t  min_width, min_height;
    int32_t  max_width, max_height;
    int32_t  width_inc, height_inc;
    int32_t  min_aspect_num, min_aspect_den;
    int32_t  max_aspect_num, max_aspect_den;
    int32_t  base_width, base_height;
    uint32_t win_gravity;
};
#else
#include <xcb/xcb_icccm.h>
#endif

class CXWaylandSurface {
  public:
    WP<CWLSurfaceResource>       m_surface;
    WP<CXWaylandSurfaceResource> m_resource;

    struct {
        CSignalT<>     stateChanged;    // maximized, fs, minimized, etc.
        CSignalT<>     metadataChanged; // title, appid
        CSignalT<>     destroy;

        CSignalT<>     resourceChange; // associated / dissociated

        CSignalT<>     setGeometry;
        CSignalT<CBox> configureRequest;

        CSignalT<>     map;
        CSignalT<>     unmap;
        CSignalT<>     commit;

        CSignalT<>     activate;
    } m_events;

    struct {
        std::string title;
        std::string appid;

        // volatile state: is reset after the stateChanged signal fires
        std::optional<bool> requestsMaximize;
        std::optional<bool> requestsFullscreen;
        std::optional<bool> requestsMinimize;
    } m_state;

    uint32_t                          m_xID         = 0;
    uint64_t                          m_wlID        = 0;
    uint64_t                          m_wlSerial    = 0;
    uint32_t                          m_lastPingSeq = 0;
    pid_t                             m_pid         = 0;
    CBox                              m_geometry;
    // SEKAI_X11_POS: 앱이 직접 청한 처음 위치 — 창을 만들 때(CreateNotify)와 붙이기 전 위치 변경 요청(ConfigureRequest)에서만.
    //   m_geometry 는 합성기가 붙이기 전에 (기억한 크기를 넣으며) 0,0 으로 덮어써서 쓸 수 없다
    Vector2D m_sekaiReqPos;
    bool                              m_overrideRedirect = false;
    bool                              m_withdrawn        = false;
    bool                              m_fullscreen       = false;
    bool                              m_maximized        = false;
    bool                              m_minimized        = false;
    bool                              m_mapped           = false;
    bool                              m_modal            = false;

    WP<CXWaylandSurface>              m_parent;
    WP<CXWaylandSurface>              m_self;
    std::vector<WP<CXWaylandSurface>> m_children;

    UP<xcb_icccm_wm_hints_t>          m_hints;
    UP<xcb_size_hints_t>              m_sizeHints;
    std::vector<uint32_t>             m_atoms;
    std::vector<uint32_t>             m_protocols;
    std::string                       m_role      = "";
    bool                              m_transient = false;
    bool                              m_sekaiNoDecorations = false; // SEKAI_X11_CSD: _MOTIF_WM_HINTS 로 "장식 없음"을 알렸다 (제목줄을 앱이 그림)

    bool                              wantsFocus();
    void                              configure(const CBox& box);
    void                              activate(bool activate);
    void                              setFullscreen(bool fs);
    void                              setMaximized(bool mx); // SEKAI_MAXIMIZE2: _NET_WM_STATE_MAXIMIZED_* 를 알린다 (전엔 아무도 쓰지 않았다)
    void                              setMinimized(bool mz);
    void                              restackToTop();
    void                              close();
    void                              ping();

  private:
    CXWaylandSurface(uint32_t xID, CBox geometry, bool OR);

    void ensureListeners();
    void map();
    void unmap();
    void considerMap();
    void setWithdrawn(bool withdrawn);

    struct {
        CHyprSignalListener destroyResource;
        CHyprSignalListener destroySurface;
        CHyprSignalListener commitSurface;
    } m_listeners;

    friend class CXWM;
};
