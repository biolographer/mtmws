// =============================================================================
// Seq — sequencer.h
//
// Per-track pattern state machine. One Sequencer instance per drum track.
//
// Lifecycle per step (driven by Clock):
//   main.cpp                  Sequencer
//   --------                  ---------
//   clock fires → seq.Tick()  reads hits[step] and accents[step] into
//                             a StepEvent, advances step, applies queued
//                             length change at wrap, returns event
//
// Design notes:
//   * The Track struct contains every per-track piece of state used now or
//     planned. We commit to the layout in M3 so M4..M9 don't trigger any
//     "where do I put this field?" refactors.
//   * `Tick()` returns a StepEvent (hit/accent/step/wrapped) rather than
//     a bare bool, because M6 (humanizer) needs the wrap signal and M9
//     (accent output) needs the accent flag. Cheap to compute, free to
//     ignore.
//   * Length-change policy (Decision 3-a): each track applies its own
//     pending_length at its OWN wrap. To retrofit Decision 3-b ("apply
//     all tracks' pending_length when track 0 wraps"), remove the inline
//     `track.length = track.pending_length` here and instead, in main.cpp,
//     when track 0's wrap event fires, write pending_length into all three
//     tracks. ~3 lines of change.
// =============================================================================

#pragma once
#include <stdint.h>

struct Track
{
    // ----- Playback state (mutated each Tick) ----------------------------
    uint8_t  length         = 16;   // current effective length, 1..32
    uint8_t  pending_length = 16;   // queued; applied at this track's wrap
    uint8_t  step           = 0;    // next step to fire (0..length-1)

    // ----- Pattern data (read each Tick, written by UI/Euclidean) --------
    uint32_t hits     = 0;          // bit n = step n triggers
    uint32_t accents  = 0;          // bit n = step n is accented (M9)

    // ----- Sound selection (read on trigger) -----------------------------
    uint8_t  base_sample = 0;       // index into drum_bank[]

    // ----- M5+ source-routing fields (in struct now to lock layout) ------
    enum Source : uint8_t { EUCLIDEAN, FLASH, EDITED };
    Source   source           = EUCLIDEAN;
    uint8_t  euc_hits         = 0;
    uint8_t  euc_rotation     = 0;
    uint8_t  flash_pattern_id = 0;
};

struct StepEvent
{
    bool    hit;       // step triggers a voice
    bool    accent;    // step is accented (used by M9)
    uint8_t step;      // step number that just fired (0..length-1)
    bool    wrapped;   // this tick wrapped the track (used by M6 humanizer)
};

class Sequencer
{
public:
    Track track;   // public for M3 simplicity; encapsulate later if needed

    // Read the just-fired step, then advance. The returned event refers to
    // the step we ARE on at call time; `step` is post-incremented for the
    // next call.
    StepEvent Tick()
    {
        StepEvent e;
        e.step   = track.step;
        e.hit    = (track.hits    >> track.step) & 1u;
        e.accent = (track.accents >> track.step) & 1u;

        ++track.step;
        e.wrapped = (track.step >= track.length);
        if (e.wrapped) {
            track.step   = 0;
            track.length = track.pending_length;   // queued change lands here
        }
        return e;
    }

    // Hard reset: jump to step 0. Does NOT apply pending_length — that
    // continues to wait for a natural wrap, which is the least surprising
    // behaviour for a reset jack.
    void Reset() { track.step = 0; }

    void SetPendingLength(uint8_t new_length)
    {
        if (new_length < 1)  new_length = 1;
        if (new_length > 32) new_length = 32;
        track.pending_length = new_length;
    }
};