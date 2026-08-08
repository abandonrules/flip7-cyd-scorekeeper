#include <unity.h>

#include "DebugCanvas.h"

void test_debug_canvas_dimensions()
{
    TEST_ASSERT_EQUAL_INT16(240, DebugCanvas::WIDTH);
    TEST_ASSERT_EQUAL_INT16(320, DebugCanvas::HEIGHT);
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_debug_canvas_dimensions);
    UNITY_END();
}

void loop()
{
}
