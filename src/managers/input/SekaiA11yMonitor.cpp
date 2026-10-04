#include "SekaiA11yMonitor.hpp"
#include "../../Compositor.hpp"
#include "../../helpers/time/Time.hpp"
#include "../../debug/Log.hpp"
#include "../KeybindManager.hpp"
#include "../../helpers/Monitor.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace SekaiA11y {
    std::optional<uint32_t> g_xkbSkip;
    std::optional<Vector2D> g_zoomFocus;

    namespace {
        struct SConfig {
            bool                                       grabAll = false; // GrabKeyboard — 모든 키
            bool                                       watch   = false; // WatchKeyboard — 모든 키를 알린다 (가로채지는 않음)
            std::vector<uint32_t>                      mods;            // Orca 수식 키 (Insert·KP_Insert·Caps_Lock 등)
            std::vector<std::pair<uint32_t, uint32_t>> strokes;         // 수식 키 없이 가로챌 조합 (keysym, 수식 마스크)
        } g_cfg;

        int                                    g_listenFD = -1, g_clientFD = -1;
        wl_event_source*                       g_listenSrc = nullptr;
        wl_event_source*                       g_clientSrc = nullptr;
        std::string                            g_inBuf;

        std::unordered_set<uint32_t>           g_swallowed; // 누름을 가로챈 키 — 뗌도 가로챈다 (앱에 눌린 채 남지 않게)
        std::unordered_set<uint32_t>           g_passMods;  // 두 번 눌러 그대로 넘긴 Orca 수식 키
        std::unordered_map<uint32_t, uint32_t> g_modDown;   // 눌려 있는 Orca 수식 키 (키코드 → keysym)
        bool                                   g_modUsed = false;
        uint32_t                               g_soloSym = 0; // 혼자 눌렀다 뗀 Orca 수식 키 — 곧 또 누르면 그건 그대로
        Time::steady_tp                        g_soloAt;

        bool                                   contains(const std::vector<uint32_t>& v, uint32_t x) {
            return std::ranges::find(v, x) != v.end();
        }

        void closeClient() {
            if (g_clientSrc)
                wl_event_source_remove(g_clientSrc);
            g_clientSrc = nullptr;
            if (g_clientFD >= 0)
                close(g_clientFD);
            g_clientFD = -1;
            g_inBuf.clear();
            g_cfg = {}; // 가로채기를 모두 푼다 (눌린 채 가로챈 키의 뗌은 g_swallowed 로 마저 가로챈다)
            g_modDown.clear();
            g_soloSym = 0;
        }

        void applyLine(const std::string& line) {
            std::istringstream ss(line);
            std::string        cmd;
            ss >> cmd;
            if (cmd == "grab" || cmd == "watch") {
                int v = 0;
                ss >> v;
                (cmd == "grab" ? g_cfg.grabAll : g_cfg.watch) = v != 0;
            } else if (cmd == "mods") {
                g_cfg.mods.clear();
                uint32_t s;
                while (ss >> s)
                    g_cfg.mods.push_back(s);
            } else if (cmd == "strokes") {
                g_cfg.strokes.clear();
                std::string tok;
                while (ss >> tok) {
                    const auto C = tok.find(':');
                    if (C == std::string::npos)
                        continue;
                    try {
                        g_cfg.strokes.emplace_back((uint32_t)std::stoul(tok.substr(0, C)), (uint32_t)std::stoul(tok.substr(C + 1)));
                    } catch (...) {}
                }
            }
        }

        int onClient(int fd, uint32_t mask, void*) {
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
                closeClient();
                return 0;
            }
            char buf[4096];
            for (;;) {
                const auto N = read(fd, buf, sizeof(buf));
                if (N > 0) {
                    g_inBuf.append(buf, N);
                    continue;
                }
                if (N == 0) { // 끊김
                    closeClient();
                    return 0;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    closeClient();
                    return 0;
                }
                break;
            }
            size_t pos;
            while ((pos = g_inBuf.find('\n')) != std::string::npos) {
                applyLine(g_inBuf.substr(0, pos));
                g_inBuf.erase(0, pos + 1);
            }
            return 0;
        }

        int onListen(int fd, uint32_t mask, void*) {
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
                return 0;
            const int C = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (C < 0)
                return 0;
            closeClient(); // 새 데몬이 붙으면 옛 연결은 버린다
            g_clientFD  = C;
            g_clientSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, C, WL_EVENT_READABLE, onClient, nullptr);
            Debug::log(LOG, "SekaiA11y: 접근성 데몬이 붙음");
            return 0;
        }

        void sendEvent(bool released, uint32_t state, uint32_t sym, uint32_t uni, uint32_t keycode, bool consumed) {
            if (g_clientFD < 0)
                return;
            const auto LINE = std::format("k {} {} {} {} {} {}\n", released ? 1 : 0, state, sym, uni, keycode, consumed ? 1 : 0);
            if (send(g_clientFD, LINE.data(), LINE.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                closeClient();
        }
    }

    void start() {
        // SEKAI_ZOOM_FOCUS
        g_pKeybindManager->m_dispatchers["sekaizoomfocus"] = [](std::string args) -> SDispatchResult {
            std::istringstream ss(args);
            double             x = 0, y = 0;
            if (!(ss >> x >> y))
                g_zoomFocus.reset();
            else
                g_zoomFocus = Vector2D{x, y};
            for (auto const& m : g_pCompositor->m_monitors)
                g_pCompositor->scheduleFrameForMonitor(m);
            return {};
        };

        g_listenFD = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (g_listenFD < 0)
            return;
        sockaddr_un addr     = {.sun_family = AF_UNIX};
        const auto  PATH     = g_pCompositor->m_instancePath + "/.sekaia11y.sock";
        if (PATH.size() >= sizeof(addr.sun_path))
            return;
        strcpy(addr.sun_path, PATH.c_str());
        unlink(PATH.c_str());
        if (bind(g_listenFD, (sockaddr*)&addr, SUN_LEN(&addr)) < 0 || listen(g_listenFD, 2) < 0) {
            Debug::log(ERR, "SekaiA11y: 소켓을 열 수 없음 {}", PATH);
            close(g_listenFD);
            g_listenFD = -1;
            return;
        }
        chmod(PATH.c_str(), 0600);
        g_listenSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_listenFD, WL_EVENT_READABLE, onListen, nullptr);
    }

    bool onKey(const IKeyboard::SKeyEvent& event, SP<IKeyboard> kb) {
        // 데몬이 없고 가로챘던 키도 없으면 할 일 없음
        if (g_clientFD < 0 && g_swallowed.empty() && g_passMods.empty())
            return false;
        if (!kb || !kb->m_xkbState)
            return false;

        const bool     PRESSED = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        const uint32_t KC      = event.keycode + 8;
        const uint32_t SYM     = xkb_state_key_get_one_sym(kb->m_xkbState, KC);
        const uint32_t UNI     = xkb_state_key_get_utf32(kb->m_xkbState, KC);
        const uint32_t STATE   = xkb_state_serialize_mods(kb->m_xkbState, XKB_STATE_MODS_EFFECTIVE) & 0xff;
        const bool     ISMOD   = contains(g_cfg.mods, SYM);
        const auto     NOW     = Time::steadyNow();
        bool           consume = false;

        if (PRESSED) {
            if (g_swallowed.contains(event.keycode))
                consume = true;
            else if (g_cfg.grabAll)
                consume = true;
            else if (ISMOD) {
                const int DELAY = kb->m_repeatDelay > 0 ? kb->m_repeatDelay : 600;
                if (g_soloSym == SYM && NOW - g_soloAt <= std::chrono::milliseconds(DELAY)) {
                    g_soloSym = 0; // 두 번째 누름은 그대로 (Insert·Caps Lock 본래 기능)
                    g_passMods.insert(event.keycode);
                } else {
                    consume                  = true;
                    g_modDown[event.keycode] = SYM;
                    g_modUsed                = false;
                }
            } else {
                g_soloSym = 0;
                if (!g_modDown.empty()) {
                    consume   = true; // Orca 수식 키를 누른 채 — Orca 명령
                    g_modUsed = true;
                } else
                    for (const auto& [s, m] : g_cfg.strokes)
                        if (s == SYM && m == STATE) {
                            consume = true;
                            break;
                        }
            }
            if (consume)
                g_swallowed.insert(event.keycode);
        } else {
            if (g_passMods.erase(event.keycode))
                consume = false;
            else if (g_swallowed.erase(event.keycode)) {
                consume = true;
                if (const auto IT = g_modDown.find(event.keycode); IT != g_modDown.end()) {
                    if (!g_modUsed) {
                        g_soloSym = IT->second;
                        g_soloAt  = NOW;
                    }
                    g_modDown.erase(IT);
                }
            }
        }

        if (consume || g_cfg.watch)
            sendEvent(!PRESSED, STATE, SYM, UNI, KC, consume);

        if (consume)
            g_xkbSkip = event.keycode;
        return consume;
    }
}
