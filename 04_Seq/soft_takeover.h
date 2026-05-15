// =============================================================================
// Seq — soft_takeover.h
//
// One-knob catch state. Used everywhere a knob can drive a parameter that
// might already be at a different value (i.e. all the time).
//
// Behaviour:
//   * Default: NOT caught — knob is ignored.
//   * On Arm(): forget catch state. Caller does this when entering a new
//     (knob, page) combination.
//   * Each frame the caller passes (knob_param, live_param). When they
//     match, the catch latches; thereafter, the knob drives the parameter.
//
// Why equality and not a ±N tolerance window?
//   Every parameter in M4..M9 is integer-quantised — sample index,
//   length-table index, Euclidean hits / rotation, accent count. The
//   user-facing notion "catch when your knob is in the same zone as the
//   current value" is exactly knob_zone == live_zone. For a future
//   continuous parameter we'd compare with a tolerance — there are none.
// =============================================================================

#pragma once
#include <stdint.h>

struct SoftTakeover
{
    bool caught = false;

    void Arm() { caught = false; }

    bool Update(int knob_param, int live_param)
    {
        if (!caught && knob_param == live_param) caught = true;
        return caught;
    }
};