// =============================================================================
// Seq — ui.h
//
// Top-level user-interface state machine. Header-only; no .cpp.
//
// Inputs each audio frame:
//   * Switch position    (Up = Perform, Middle = Edit, Down = momentary)
//   * Three knob values  (Main, X, Y) in 0..4095
//   * Three Track*       — UI mutates pending_length and base_sample
//
// Outputs read by main.cpp for LED feedback:
//   * CurrentMode(), CurrentPage()
//   * TrackCaught(t)   — for soft-takeover dim feedback
//   * BlinkOn()        — for length-pending visual feedback
//
// Architectural decisions (locked in M4; see PLAN.md):
//   1a: Switch Up=Perform, Middle=Edit, Down momentary cycles page (1→2→3→1)
//   2c: All 3 tracks edited simultaneously — Main=low, X=mid, Y=high,
//       on every page. One parameter per track per page.
//   3b: Length is detented to musical values {1..8, 12, 16, 24, 32}
//   4 : Soft takeover, quantisation-aware (equality, not tolerance)
//   5b: Catch state is per-knob-per-page (3 knobs × 3 pages = 9 catches)
//   6a: Length-pending shown as ~2 Hz blink on the track's right LED
//   7 : Page indicator on the left LED column, EDIT mode only
//
// Pages in M4:
//   PAGE_SAMPLE   — Main/X/Y choose drum_bank index for low/mid/high tracks
//   PAGE_LENGTH   — Main/X/Y choose pending_length (queued; applies at wrap)
//   PAGE_RESERVED — placeholder for M5's Euclidean page; no action in M4
// =============================================================================

#pragma once
#include <stdint.h>
#include "ComputerCard.h"
#include "sequencer.h"
#include "samples.h"
#include "soft_takeover.h"

class UI
{
public:
    enum Mode : uint8_t { PERFORM, EDIT };
    enum Page : uint8_t {
        PAGE_SAMPLE   = 0,
        PAGE_LENGTH   = 1,
        PAGE_RESERVED = 2
    };

    static constexpr int NUM_TRACKS = 3;
    static constexpr int NUM_PAGES  = 3;

    // Detented musical lengths — Decision 3-b.
    // 12 entries: 1..8 contiguous (fine control for short polymetric loops),
    // then 12/16/24/32 for the common longer bar lengths.
    static constexpr uint8_t LENGTH_TABLE[12] = {
        1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32
    };
    static constexpr int LENGTH_TABLE_SIZE = 12;

    void Update(Switch sw,
                int16_t knob_main, int16_t knob_x, int16_t knob_y,
                Track* tracks[NUM_TRACKS])
    {
        // ----- Down-edge detection (momentary page cycle) ----------------
        const bool down_edge = (sw == Switch::Down)
                            && (prev_switch != Switch::Down);
        prev_switch = sw;

        // ----- Mode resolution -------------------------------------------
        // Down does NOT change mode; it's a momentary action only. Up and
        // Middle are the two stable modes.
        const Mode prev_mode = mode;
        if      (sw == Switch::Up)     mode = PERFORM;
        else if (sw == Switch::Middle) mode = EDIT;
        // else (Down): leave mode unchanged

        // ----- Page cycle on Down edge -----------------------------------
        // Cycles even from Perform mode — user can pre-select page before
        // entering Edit. Harmless when not editing.
        if (down_edge) {
            page = (Page)((page + 1) % NUM_PAGES);
            if (mode == EDIT) ArmCatchesForPage(page);
        }

        // ----- Re-arm soft takeover on entering EDIT ---------------------
        if (mode == EDIT && prev_mode != EDIT) {
            ArmCatchesForPage(page);
        }

        // ----- Parameter edits (EDIT mode only) --------------------------
        if (mode == EDIT) {
            const int16_t knobs[NUM_TRACKS] = { knob_main, knob_x, knob_y };
            for (int t = 0; t < NUM_TRACKS; ++t) {
                EditTrack(t, knobs[t], tracks[t]);
            }
        }

        // ----- Visual blink phase (2 Hz at 48 kHz) -----------------------
        if (++blink_counter >= BLINK_PERIOD_FRAMES) blink_counter = 0;
    }

    Mode CurrentMode() const { return mode; }
    Page CurrentPage() const { return page; }

    // Catch status for track t's knob on the current page. PAGE_RESERVED
    // has no edits and therefore no meaningful "catch" — always true so
    // we don't paint a dim LED on a page that does nothing.
    bool TrackCaught(int t) const {
        if (page == PAGE_RESERVED) return true;
        return catches[t][page].caught;
    }

    // True for the first half of each blink cycle.
    bool BlinkOn() const { return blink_counter < BLINK_PERIOD_FRAMES / 2; }

private:
    Mode   mode = PERFORM;
    Page   page = PAGE_SAMPLE;
    Switch prev_switch = Switch::Middle;

    SoftTakeover catches[NUM_TRACKS][NUM_PAGES];

    uint32_t blink_counter = 0;
    static constexpr uint32_t BLINK_PERIOD_FRAMES = 24000;   // 2 Hz @ 48 kHz

    void ArmCatchesForPage(Page p) {
        for (int t = 0; t < NUM_TRACKS; ++t) catches[t][p].Arm();
    }

    void EditTrack(int t, int16_t knob, Track* track)
    {
        if (page == PAGE_SAMPLE) {
            const int kp = KnobToSampleIdx(knob);
            const int lp = (int)track->base_sample;
            if (catches[t][page].Update(kp, lp)) {
                track->base_sample = (uint8_t)kp;
            }
        }
        else if (page == PAGE_LENGTH) {
            const int kp = KnobToLengthIdx(knob);
            const int lp = LengthToIdx(track->pending_length);
            if (catches[t][page].Update(kp, lp)) {
                track->pending_length = LENGTH_TABLE[kp];
            }
        }
        // PAGE_RESERVED: no parameter
    }

    static int KnobToSampleIdx(int knob) {
        if (NUM_SAMPLES <= 0) return 0;
        int idx = (knob * NUM_SAMPLES) / 4096;
        if (idx >= NUM_SAMPLES) idx = NUM_SAMPLES - 1;
        return idx;
    }

    static int KnobToLengthIdx(int knob) {
        int idx = (knob * LENGTH_TABLE_SIZE) / 4096;
        if (idx >= LENGTH_TABLE_SIZE) idx = LENGTH_TABLE_SIZE - 1;
        return idx;
    }

    // Inverse of LENGTH_TABLE: find table index for a given length value.
    // Should always hit because every write to pending_length goes through
    // the table. Fallback (9 → length 16) protects against stale values
    // from a previous milestone's stress test.
    static int LengthToIdx(uint8_t length) {
        for (int i = 0; i < LENGTH_TABLE_SIZE; ++i) {
            if (LENGTH_TABLE[i] == length) return i;
        }
        return 9;
    }
};