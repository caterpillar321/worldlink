#include "Keyboard.hpp"
#include "../managers/input/SekaiA11yMonitor.hpp"
#include "../defines.hpp"

#include <aquamarine/input/Input.hpp>

SP<CKeyboard> CKeyboard::create(SP<Aquamarine::IKeyboard> keeb) {
    SP<CKeyboard> pKeeb = SP<CKeyboard>(new CKeyboard(keeb));

    pKeeb->m_self = pKeeb;

    return pKeeb;
}

bool CKeyboard::isVirtual() {
    return false;
}

SP<Aquamarine::IKeyboard> CKeyboard::aq() {
    return m_keyboard.lock();
}

CKeyboard::CKeyboard(SP<Aquamarine::IKeyboard> keeb) : m_keyboard(keeb) {
    if (!keeb)
        return;

    m_listeners.destroy = keeb->events.destroy.listen([this] {
        m_keyboard.reset();
        m_events.destroy.emit();
    });

    m_listeners.key = keeb->events.key.listen([this](const Aquamarine::IKeyboard::SKeyEvent& event) {
        SekaiA11y::g_xkbSkip.reset();
        m_keyboardEvents.key.emit(SKeyEvent{
            .timeMs  = event.timeMs,
            .keycode = event.key,
            .state   = event.pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
        });

        // SEKAI_A11Y_MONITOR: 화면 읽기가 가로챈 키는 xkb 상태(Caps Lock 켜짐 등)도 바꾸지 않는다
        if (SekaiA11y::g_xkbSkip != event.key)
            updateXkbStateWithKey(event.key + 8, event.pressed);
    });

    m_listeners.modifiers = keeb->events.modifiers.listen([this] {
        updateModifiersState();

        m_keyboardEvents.modifiers.emit(SModifiersEvent{
            .depressed = m_modifiersState.depressed,
            .latched   = m_modifiersState.latched,
            .locked    = m_modifiersState.locked,
            .group     = m_modifiersState.group,
        });
    });

    m_deviceName = keeb->getName();
}
