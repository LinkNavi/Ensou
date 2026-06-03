#include "beatmap/difficulty.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>

// ── Finger/Hand layout ───────────────────────────────────────────────────────
enum class Hand   : uint8_t { Left, Right, Ambiguous };
enum class Finger : uint8_t { Index = 0, Middle = 1, Ring = 2, Thumb = 3 };
enum class FingerAction : uint8_t { Roll, SimpleJack, TechJack, Bracket, None };

// Lane → hand mapping for 4K and 7K (0-based columns)
static Hand LaneHand4K(int col) {
    return col < 2 ? Hand::Left : Hand::Right;
}
static Hand LaneHand7K(int col, Hand ambiguous = Hand::Right) {
    // 0,1,2 = Left; 3 = Ambiguous; 4,5,6 = Right
    if (col < 3)  return Hand::Left;
    if (col == 3) return ambiguous;
    return Hand::Right;
}
static Finger LaneFinger4K(int col) {
    // L:Middle,Index | R:Index,Middle
    static const Finger t[] = {Finger::Middle, Finger::Index, Finger::Index, Finger::Middle};
    return t[col & 3];
}
static Finger LaneFinger7K(int col) {
    static const Finger t[] = {
        Finger::Ring, Finger::Middle, Finger::Index, Finger::Thumb,
        Finger::Index, Finger::Middle, Finger::Ring
    };
    return t[col % 7];
}

// ── Strain constants (tuned to match Quaver's feel) ──────────────────────────
struct SC {
    // Chord grouping tolerance
    float chord_tolerance_ms    = 10.0f;

    // Roll params (alternating fingers, no jack)
    float roll_lower_ms         = 50.0f;   // fastest roll = max strain
    float roll_upper_ms         = 350.0f;  // slowest roll = min strain
    float roll_max_strain       = 3.5f;
    float roll_curve_exp        = 1.4f;

    // SimpleJack (same finger repeated)
    float sjack_lower_ms        = 50.0f;
    float sjack_upper_ms        = 350.0f;
    float sjack_max_strain      = 3.8f;
    float sjack_curve_exp       = 1.2f;

    // TechJack (jack within chord)
    float tjack_lower_ms        = 50.0f;
    float tjack_upper_ms        = 350.0f;
    float tjack_max_strain      = 3.6f;
    float tjack_curve_exp       = 1.3f;

    // Bracket (chord but not jack)
    float bracket_lower_ms      = 30.0f;
    float bracket_upper_ms      = 250.0f;
    float bracket_max_strain    = 3.2f;
    float bracket_curve_exp     = 1.5f;

    // Roll manipulation detection
    float roll_ratio_tolerance  = 2.5f;   // duration ratio to flag manipulation
    float roll_ratio_multiplier = 0.5f;
    float roll_max_length       = 15.0f;
    float roll_length_mult      = 0.6f;

    // Jack manipulation / vibro
    float vibro_duration_ms     = 88.0f;  // ~170bpm 1/4
    float vibro_tolerance_ms    = 35.0f;
    float vibro_multiplier      = 0.48f;
    float vibro_max_length      = 15.0f;
    float vibro_length_mult     = 0.6f;

    // LN layering
    float ln_layer_threshold_ms = 50.0f;
    float ln_layer_tolerance_ms = 60.0f;
    float ln_end_threshold_ms   = 25.0f;
    float ln_base_mult          = 0.2f;
    float ln_release_after_mult = 1.3f;
    float ln_release_before_mult= 1.15f;
    float ln_tap_mult           = 1.2f;
};

// ── Per-hit-object solver data ────────────────────────────────────────────────
struct HObj {
    int      time_ms;
    int      end_ms;
    int      column;
    NoteType type;
    Hand     hand;
    Finger   finger;

    // Computed
    FingerAction action           = FingerAction::None;
    float        action_coeff     = 1.0f;  // strain coefficient from action
    float        ln_mult          = 1.0f;  // LN layering multiplier
    float        manip_mult       = 1.0f;  // roll/vibro manipulation multiplier
    int          action_dur_ms    = 0;     // duration to next note on same hand

    // Finger state bitmask for chord detection (bit per finger)
    uint8_t      finger_bits      = 0;
    bool         is_chord         = false;

    // Linked list on same hand for pattern detection
    HObj*        next_same_hand   = nullptr;

    float TotalStrain() const {
        return action_coeff * ln_mult * manip_mult;
    }
};

// ── Coefficient curve (Quaver's GetCoefficientValue) ─────────────────────────
static float Coeff(float duration, float x_min, float x_max,
                   float strain_max, float exp) {
    float ratio = std::max(0.0f, (duration - x_min) / (x_max - x_min));
    ratio = 1.0f - std::min(1.0f, ratio);
    return 1.0f + (strain_max - 1.0f) * std::pow(ratio, exp);
}

// ── Custom note type modifier ─────────────────────────────────────────────────
static float CustomNoteBonus(NoteType t) {
    switch (t) {
        case NoteType::MINE:    return 1.05f; // precision required
        case NoteType::SWAP:    return 1.10f; // column changes pattern
        case NoteType::REBOUND: return 1.20f; // must re-hit
        default:                return 1.0f;
    }
}

// ── Main calculator ───────────────────────────────────────────────────────────
float CalculateStarRating(const Beatmap& map, float rate) {
    if (map.notes.size() < 2) return 0.0f;

    int keys = std::max(1, map.difficulty.key_count);
    const SC sc;

    // Build solver objects
    std::vector<HObj> objs;
    objs.reserve(map.notes.size());

    for (const auto& n : map.notes) {
        HObj h;
        h.time_ms = n.time_ms;
        h.end_ms  = n.end_ms;
        h.column  = std::max(0, std::min(n.column, keys - 1));
        h.type    = n.type;

        if (keys <= 4) {
            h.hand   = LaneHand4K(h.column);
            h.finger = LaneFinger4K(h.column);
        } else {
            h.hand   = LaneHand7K(h.column);
            h.finger = LaneFinger7K(h.column);
        }
        h.finger_bits = (uint8_t)(1 << (int)h.finger);
        objs.push_back(h);
    }

    std::sort(objs.begin(), objs.end(),
        [](const HObj& a, const HObj& b){ return a.time_ms < b.time_ms; });

    // ── Step 1: Chord merging ─────────────────────────────────────────────────
    // Notes within chord_tolerance_ms on same hand merge their finger bits
    for (int i = 0; i < (int)objs.size() - 1; i++) {
        for (int j = i + 1; j < (int)objs.size(); j++) {
            float diff = (float)(objs[j].time_ms - objs[i].time_ms);
            if (diff > sc.chord_tolerance_ms) break;
            if (objs[i].hand == objs[j].hand) {
                // Merge j into i if no duplicate finger
                if (!(objs[i].finger_bits & objs[j].finger_bits)) {
                    objs[i].finger_bits |= objs[j].finger_bits;
                    objs[i].is_chord = true;
                    objs.erase(objs.begin() + j);
                    j--;
                }
            }
        }
    }

    // ── Step 2: Link next-on-same-hand pointers ───────────────────────────────
    // Process left and right hands separately, then combine for 7K ambiguous
    for (int pass = 0; pass < 2; pass++) {
        Hand target = (pass == 0) ? Hand::Left : Hand::Right;
        HObj* prev = nullptr;
        for (auto& h : objs) {
            if (h.hand == target) {
                if (prev) {
                    prev->next_same_hand   = &h;
                    prev->action_dur_ms    = h.time_ms - prev->time_ms;
                }
                prev = &h;
            }
        }
    }

    // ── Step 3: Finger action classification ─────────────────────────────────
    for (auto& h : objs) {
        HObj* next = h.next_same_hand;
        if (!next) continue;

        float dur = (float)(h.action_dur_ms) / rate;

        bool jack_found   = (next->finger_bits & h.finger_bits) != 0;
        bool chord_found  = h.is_chord || next->is_chord;
        bool same_state   = (h.finger_bits == next->finger_bits);

        if (!chord_found && !same_state) {
            // Roll: alternating fingers, no jack
            h.action = FingerAction::Roll;
            h.action_coeff = Coeff(dur,
                sc.roll_lower_ms, sc.roll_upper_ms,
                sc.roll_max_strain, sc.roll_curve_exp);
        } else if (same_state) {
            // SimpleJack: exact same finger(s)
            h.action = FingerAction::SimpleJack;
            h.action_coeff = Coeff(dur,
                sc.sjack_lower_ms, sc.sjack_upper_ms,
                sc.sjack_max_strain, sc.sjack_curve_exp);
        } else if (jack_found) {
            // TechJack: partial finger overlap (jack within chord)
            h.action = FingerAction::TechJack;
            h.action_coeff = Coeff(dur,
                sc.tjack_lower_ms, sc.tjack_upper_ms,
                sc.tjack_max_strain, sc.tjack_curve_exp);
        } else {
            // Bracket: chord, no finger overlap
            h.action = FingerAction::Bracket;
            h.action_coeff = Coeff(dur,
                sc.bracket_lower_ms, sc.bracket_upper_ms,
                sc.bracket_max_strain, sc.bracket_curve_exp);
        }

        // Apply custom note type bonus
        h.action_coeff *= CustomNoteBonus(h.type);
    }

    // ── Step 4: Roll manipulation detection ──────────────────────────────────
    // Penalises A→B→A patterns where one interval is much longer than the other
    int roll_manip_idx = 0;
    for (auto& h : objs) {
        bool found = false;
        HObj* mid  = h.next_same_hand;
        if (mid && mid->next_same_hand) {
            HObj* last = mid->next_same_hand;
            if (h.action == FingerAction::Roll && mid->action == FingerAction::Roll
                && h.finger_bits == last->finger_bits) {
                float a = (float)h.action_dur_ms;
                float b = (float)mid->action_dur_ms;
                float ratio = (a > b) ? (a / b) : (b / a);
                if (ratio >= sc.roll_ratio_tolerance) {
                    float dur_mult  = 1.0f / (1.0f + (ratio - 1.0f) * sc.roll_ratio_multiplier);
                    float len_mult  = 1.0f - ((roll_manip_idx / sc.roll_max_length)
                                      * (1.0f - sc.roll_length_mult));
                    h.manip_mult = dur_mult * len_mult;
                    found = true;
                    if (roll_manip_idx < (int)sc.roll_max_length) roll_manip_idx++;
                }
            }
        }
        if (!found && roll_manip_idx > 0) roll_manip_idx--;
    }

    // ── Step 5: Jack/vibro manipulation detection ─────────────────────────────
    // Penalises very fast same-finger jacks (vibro)
    int vibro_idx = 0;
    for (auto& h : objs) {
        bool found = false;
        HObj* next = h.next_same_hand;
        if (next && h.action == FingerAction::SimpleJack
                 && next->action == FingerAction::SimpleJack) {
            float dur = (float)h.action_dur_ms / rate;
            float vib_max = sc.vibro_duration_ms + sc.vibro_tolerance_ms;
            float dur_val = std::min(1.0f, std::max(0.0f,
                (vib_max - dur) / sc.vibro_tolerance_ms));
            float dur_mult = 1.0f - dur_val * (1.0f - sc.vibro_multiplier);
            float len_mult = 1.0f - ((vibro_idx / sc.vibro_max_length)
                             * (1.0f - sc.vibro_length_mult));
            h.manip_mult = std::min(h.manip_mult, dur_mult * len_mult);
            found = true;
            if (vibro_idx < (int)sc.vibro_max_length) vibro_idx++;
        }
        if (!found) vibro_idx = 0;
    }

    // ── Step 6: LN layering multiplier ───────────────────────────────────────
    for (auto& h : objs) {
        if (h.end_ms <= h.time_ms) continue; // not a hold
        float hold_len = (float)(h.end_ms - h.time_ms) / rate;

        // Base LN multiplier from hold length
        float dur_val = 1.0f - std::min(1.0f, std::max(0.0f,
            (sc.ln_layer_threshold_ms + sc.ln_layer_tolerance_ms - hold_len)
            / sc.ln_layer_tolerance_ms));
        h.ln_mult = 1.0f + (1.0f - dur_val) * sc.ln_base_mult;

        // Check if next note on same hand is layered inside this LN
        HObj* next = h.next_same_hand;
        if (!next) continue;
        float ns = (float)next->time_ms / rate;
        float hs = (float)h.time_ms / rate;
        float he = (float)h.end_ms  / rate;
        float thresh = sc.ln_end_threshold_ms / rate;

        if (ns < he - thresh && ns >= hs + thresh) {
            float ne = (float)next->end_ms / rate;
            if (ne > he + thresh)
                h.ln_mult *= sc.ln_release_after_mult;
            else if (next->end_ms > 0)
                h.ln_mult *= sc.ln_release_before_mult;
            else
                h.ln_mult *= sc.ln_tap_mult;
        }
    }

    // ── Step 7: Aggregate difficulty ─────────────────────────────────────────
    // For 7K, Quaver averages Left and Right hand difficulty separately.
    // For 4K (and other even keymodes), sum both hands.
    auto sum_hand = [&](Hand target) -> float {
        float total = 0.0f;
        int   count = 0;
        for (const auto& h : objs) {
            if (h.hand == target) {
                total += h.TotalStrain();
                count++;
            }
        }
        return count > 0 ? total / count : 0.0f;
    };

    float diff;
    if (keys == 7) {
        // Average of both hands (same as Quaver's 7K path)
        diff = (sum_hand(Hand::Left) + sum_hand(Hand::Right)) * 0.5f;
    } else {
        // Sum both hands, normalise by total count
        float total = 0.0f;
        for (const auto& h : objs)
            total += h.TotalStrain();
        diff = objs.empty() ? 0.0f : total / (float)objs.size();
    }

    // Scale to a roughly 0–50 range matching Quaver's output
    // Quaver's ratings typically sit between 1 and 50 for reasonable maps
    return diff * 10.0f;
}
