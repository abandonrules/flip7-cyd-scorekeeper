#include <cassert>
#include <cstdint>

#include "aquarium_logic.h"

int main() {
    int16_t x[7]{};
    int16_t y[7]{};
    for (uint8_t i = 0; i < 7; ++i) {
        x[i] = aquariumInitialFishX(i);
        y[i] = aquariumInitialFishY(i);
        assert(y[i] >= kAquariumMinFishY);
        assert(y[i] <= kAquariumMaxFishY);
        assert(aquariumInitialHunger(i) > 0);
        for (uint8_t j = 0; j < i; ++j) {
            const int16_t dx = x[i] > x[j] ? x[i] - x[j] : x[j] - x[i];
            const int16_t dy = y[i] > y[j] ? y[i] - y[j] : y[j] - y[i];
            assert(dx >= 14 || dy >= 10);
        }
    }
    assert(!aquariumSnapshotLooksCollapsed(x, y, 7));

    int16_t collapsedX[7] = {10, 10, 10, 10, 10, 10, 10};
    int16_t collapsedY[7] = {60, 60, 60, 60, 60, 60, 60};
    assert(aquariumSnapshotLooksCollapsed(collapsedX, collapsedY, 7));

    int16_t zeroX[7]{};
    int16_t zeroY[7]{};
    assert(aquariumSnapshotLooksCollapsed(zeroX, zeroY, 7));

    assert(aquariumDrainSatiety(10, kAquariumHungerTickMs - 1) == 10);
    assert(aquariumDrainSatiety(10, kAquariumHungerTickMs) == 11);
    assert(aquariumDrainSatiety(254, kAquariumHungerTickMs * 4) == 255);
    assert(aquariumSatisfyHunger(200, 96) == 104);
    assert(aquariumSatisfyHunger(40, 96) == 0);

    assert(!aquariumFishShouldChaseFood(0, 100, 100, 112, 108));
    assert(aquariumFishShouldChaseFood(120, 100, 100, 160, 140));
    assert(aquariumFishShouldChaseFood(220, 100, 100, 198, 168));
    assert(!aquariumFishShouldChaseFood(120, 100, 100, 205, 170));
    assert(aquariumFishReachedFood(100, 100, 108, 106));
    assert(!aquariumFishReachedFood(100, 100, 120, 100));
}
