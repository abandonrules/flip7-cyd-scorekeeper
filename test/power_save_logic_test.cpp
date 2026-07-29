#include <cassert>
#include <cstdint>

#include "power_save.h"

int main() {
    constexpr uint32_t leadIn = 30000;
    constexpr uint32_t timeout = 60000;

    assert(!shouldStartIdleAquarium(0, 0, false, false, false, leadIn));
    assert(!shouldStartIdleAquarium(29999, 0, false, false, false, leadIn));
    assert(shouldStartIdleAquarium(30000, 0, false, false, false, leadIn));
    assert(!shouldStartIdleAquarium(30000, 0, true, false, false, leadIn));
    assert(!shouldStartIdleAquarium(30000, 0, false, true, false, leadIn));
    assert(!shouldStartIdleAquarium(30000, 0, false, false, true, leadIn));

    assert(!shouldEnterPowerSave(0, 0, false, false, timeout));
    assert(!shouldEnterPowerSave(59999, 0, false, false, timeout));
    assert(shouldEnterPowerSave(60000, 0, false, false, timeout));
    assert(!shouldEnterPowerSave(90000, 0, true, false, timeout));
    assert(!shouldEnterPowerSave(90000, 0, false, true, timeout));
    assert(shouldEnterPowerSave(20, UINT32_MAX - 59990, false, false, timeout));

    assert(powerSaveBacklightLevel(false) == 255);
    assert(powerSaveBacklightLevel(true) == 0);

    PowerSaveState state{1000, false, true};
    notePowerSaveActivity(state, 2500);
    assert(state.lastInteractionMs == 2500);
    assert(!state.active);
    assert(!state.idleAquariumStarted);

    state = PowerSaveState{1000, true, true};
    notePowerSaveActivity(state, 3500);
    assert(state.lastInteractionMs == 3500);
    assert(!state.active);
    assert(!state.idleAquariumStarted);

    return 0;
}
