#include "puzzle_logic.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

void test_periodic_spec_is_supported() {
    const PuzzleSpec spec{4, 3, PuzzleTheme::Elements};

    assert(isSupportedPuzzleSpec(spec));
}

void test_periodic_round_selects_distinct_sorted_elements() {
    uint8_t elements[kPeriodicPuzzleElementCount]{};

    selectPeriodicPuzzleElements(0x12345678, elements, kPeriodicPuzzleElementCount);

    for (uint8_t i = 0; i < kPeriodicPuzzleElementCount; ++i) {
        assert(elements[i] >= 1);
        assert(elements[i] <= kPeriodicElementCount);
        if (i > 0) {
            assert(elements[i - 1] < elements[i]);
        }
    }
}

void test_periodic_state_maps_tile_rank_to_element() {
    const PuzzleState state = makeInitialPuzzle(0x10, 42, {4, 3, PuzzleTheme::Elements});
    uint8_t elements[kPeriodicPuzzleElementCount]{};
    selectPeriodicPuzzleElements(state.gameId, elements, kPeriodicPuzzleElementCount);

    assert(periodicElementForTile(state, 1) == elements[0]);
    assert(periodicElementForTile(state, 11) == elements[10]);
    assert(periodicElementForTile(state, 0) == 0);
    assert(periodicElementForTile(state, 12) == 0);
}

void test_periodic_symbol_lookup() {
    assert(periodicElementSymbol(1)[0] == 'H');
    assert(periodicElementSymbol(8)[0] == 'O');
    assert(periodicElementSymbol(8)[1] == '\0');
    assert(periodicElementSymbol(118)[0] == 'O');
    assert(periodicElementSymbol(118)[1] == 'g');
    assert(periodicElementSymbol(0)[0] == '?');
}

int main() {
    test_periodic_spec_is_supported();
    test_periodic_round_selects_distinct_sorted_elements();
    test_periodic_state_maps_tile_rank_to_element();
    test_periodic_symbol_lookup();
}
