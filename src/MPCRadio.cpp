// MPC Radio - an FM band that lives inside the plugin. An MPC insert effect by Steph.
//
// NOW ALWAYS LIVE: the dial holds only the eleven internet stations. The Live, Key and Sync controls
// are kept in their places so saved projects load, but Live is no longer read, so the radio's own
// synthesised stations below never sound. The Speaker stage has six types, chosen with Speaker Type:
//   low cut -> cone grit (tanh) -> high cut -> body resonance -> fold to mono, level made up.
// What follows describes the two bands as they were built:
// The radio has two bands. With Live off, every station is synthesised here. With Live on, the dial
// holds eleven internet stations instead (listed in online.json, read through mpforge::AudioStream):
// the one nearest the dial is opened, static fills the second or two while it arrives, and its
// sound then takes the place of "clean" and "ghost bus" below and runs through the same air and effects.
// The plugin's own code never uses the network; MP Forge's stream does the fetching.
//
// Signal flow:
//
//   Tune (glided) --> for each of 7 stations: clean gain / fringe gain / far-ghost gain
//
//   stations (only the audible ones are rendered)
//     87.9 LINE   the track's own audio             93.9 BREAKS  swung drum break
//     90.9 KEYS   electric piano, bass, licks       96.9 TALK    formant-synth talk show
//     99.9 PULSE  kick, clap, hats, pumping bass   102.9 DRIFT   slow pads, drone, glass pings
//    105.9 BELLS  kalimba pattern and bass note
//          |                         |
//      clean (stereo)        ghost bus (mono) -> band-pass -> tanh
//          |                         |
//          +----------+--------------+---- hiss + crackle (Static)
//                     |               ---- whistles + utility signals (Squeal)
//                     v                    (all air is scaled by "how far from locked")
//      Wow (wobbling delay) -> Drive (tanh, level held) -> Speaker (HP + LP, level made up)
//                     -> Echo (send, filtered feedback, soft clip) -> Room (4-line network)
//                     -> (Mix: always all radio now, the control is unread) -> Volume -> safety clip (linear below 0.9)
//
// Station clocks follow the song's tempo and bar position (Sync on) or run at their own
// tempo (Sync off). Patterns are computed from the bar number, so they repeat exactly when the
// song is played from the same place. Everything slow runs on a fixed 16-sample control grid that
// does not depend on the host's block size.
//
// Real-time rules: all memory is allocated in prepare(); process() never allocates, locks or logs.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"
#include "dsp/noise.h"
#include "dsp/onepole.h"
#include "dsp/svf.h"
#include "dsp/delay_line.h"
#include "dsp/oscillator.h"
#include "mp_forge_stream.h"

#include <vector>

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------------------------

constexpr int   kCtl  = 16;        // control grid, in samples
constexpr float kCtlF = 16.0f;

constexpr int kNumStations = 7;    // 0 = LINE (the track), 1..6 = broadcasts
const float kStationMHz[kNumStations] = {87.9f, 90.9f, 93.9f, 96.9f, 99.9f, 102.9f, 105.9f};
const float kOwnBpm[kNumStations]     = {120.0f, 82.0f, 92.0f, 100.0f, 124.0f, 66.0f, 104.0f};
const float kSwing[kNumStations]      = {0.5f, 0.57f, 0.58f, 0.5f, 0.5f, 0.5f, 0.54f};

constexpr int kNumAliens = 4;
const float kAlienMHz[kNumAliens] = {89.4f, 95.4f, 101.4f, 107.4f};

// The live band: internet stations, in the order of online.json. Only one of them plays at a time.
//   SOUL  HIPHOP  FUNK  REGGAE  JAZZ  AMBIENT  ROCK  HOUSE  WORLD  TALK  CLASSIC
constexpr int kNumLive = 11;
const float kLiveMHz[kNumLive] = {88.6f, 90.4f, 92.2f, 94.0f, 95.8f, 97.6f, 99.4f, 101.2f, 103.0f, 104.8f, 106.6f};
// The speakers the Speaker knob can turn the radio into. Each row is what the sound becomes with
// Speaker all the way up; lower down, the corners sit part of the way there from full range.
// Pocket is the speaker the radio always had, number for number.
enum { kSpkHp = 0, kSpkHpRes, kSpkLp, kSpkLpRes, kSpkGain, kSpkPkHz, kSpkPkRes, kSpkPkAmt, kSpkMono, kSpkGrit, kSpkVals };
constexpr int kNumSpk = 6;
const float kSpk[kNumSpk][kSpkVals] = {
    //  low cut      high cut      level  body resonance        mono  cone grit
    {  380.0f, 0.40f,  3200.0f, 0.40f, 1.80f, 1000.0f, 0.50f, 0.0f, 0.00f, 0.00f },   // Pocket: a small plastic transistor radio
    {   55.0f, 0.60f, 11000.0f, 0.45f, 0.70f,   95.0f, 0.59f, 0.8f, 0.00f, 0.15f },   // Boombox: big woofers, a thump at 95 Hz, a bright top
    {   70.0f, 0.50f,  5200.0f, 0.20f, 0.90f,  140.0f, 0.67f, 1.0f, 0.35f, 0.10f },   // Car: boomy door speakers, a dull top, narrow
    {  420.0f, 0.35f,  2900.0f, 0.55f, 1.20f, 1900.0f, 0.81f, 1.0f, 1.00f, 0.50f },   // Phone: a telephone earpiece, hard and nasal, mono
    {  110.0f, 0.45f,  4200.0f, 0.15f, 1.00f,  210.0f, 0.62f, 0.8f, 1.00f, 0.20f },   // Wood: an old cabinet set, boxy and warm, mono
    {  650.0f, 0.55f,  3600.0f, 0.60f, 1.00f, 1250.0f, 0.84f, 1.4f, 1.00f, 0.90f },   // Horn: a tannoy horn, honking and overdriven, mono
};
constexpr float kLiveGain = 0.5f;  // a stream can arrive at full scale; the radio's own stations sit lower

// Sine table shared by every voice. Built in the plugin's constructor, never on the audio thread.
constexpr int kSinSize = 2048;
float gSin[kSinSize + 1];
float gNoteHz[128];

void buildTables()
{
    for (int i = 0; i <= kSinSize; ++i)
        gSin[i] = (float)std::sin(6.283185307179586 * (double)i / (double)kSinSize);
    for (int n = 0; n < 128; ++n)
        gNoteHz[n] = (float)(440.0 * std::pow(2.0, ((double)n - 69.0) / 12.0));
}

// Sine of a phase in cycles. Valid for any phase above -16.
inline float sinT(float p)
{
    p += 16.0f;
    p -= (float)(int)p;
    const float x = p * (float)kSinSize;
    const int   i = (int)x;
    const float f = x - (float)i;
    return gSin[i] + f * (gSin[i + 1] - gSin[i]);
}

inline float noteHz(int n) { return gNoteHz[n < 0 ? 0 : (n > 127 ? 127 : n)]; }

inline int fdiv(int a, int b) { int q = a / b; if ((a % b) != 0 && ((a < 0) != (b < 0))) --q; return q; }
inline int imod(int a, int b) { const int m = a % b; return m < 0 ? m + b : m; }

inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = (a * 0x9E3779B1u) ^ ((b + 0x7F4A7C15u) * 0x85EBCA6Bu) ^ ((c + 0x165667B1u) * 0xC2B2AE35u);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// A short run of repeatable random numbers from one seed.
struct Dice
{
    uint32_t h;
    explicit Dice(uint32_t seed) : h(seed) {}
    uint32_t bits()
    {
        h += 0x9E3779B9u;
        uint32_t z = h;
        z ^= z >> 16; z *= 0x7FEB352Du; z ^= z >> 15; z *= 0x846CA68Bu; z ^= z >> 16;
        return z;
    }
    float next() { return (float)(bits() >> 8) * (1.0f / 16777216.0f); }
};

inline float smooth01(float e0, float e1, float x)
{
    const float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Sample-rate dependent constants handed to every voice.
struct Ctx
{
    float sr = 44100.0f, invSr = 1.0f / 44100.0f, tickRate = 44100.0f / kCtlF, keyRatio = 1.0f;
    float slewHalf = 0.05f, slew1 = 0.022f, slew2 = 0.011f, slew5 = 0.0045f, slew12 = 0.0019f;

    void setSampleRate(float s)
    {
        sr = s < 8000.0f ? 8000.0f : s;
        invSr = 1.0f / sr;
        tickRate = sr / kCtlF;
        slewHalf = slew(0.5f); slew1 = slew(1.0f); slew2 = slew(2.0f); slew5 = slew(5.0f); slew12 = slew(12.0f);
    }
    // One-pole coefficient that covers 63 % of a step in `ms`.
    float slew(float ms) const
    {
        const float n = ms * 0.001f * sr;
        return n <= 1.0f ? 1.0f : 1.0f - std::exp(-1.0f / n);
    }
    // Per-sample multiplier that falls 60 dB in `seconds`.
    float t60(float seconds) const { return std::exp(-6.9078f / (seconds * sr)); }
    float t60Tick(float seconds) const { return std::exp(-6.9078f / (seconds * tickRate)); }
};

// A gain that is ramped in a straight line across each 16-sample control step.
struct Ramp
{
    float cur = 0.0f, tgt = 0.0f, inc = 0.0f;
    void to(float t) { cur = tgt; tgt = t; inc = (tgt - cur) * (1.0f / kCtlF); }
    void snap(float t) { cur = tgt = t; inc = 0.0f; }
    float next() { cur += inc; return cur; }
    bool live() const { return cur > 1.0e-6f || tgt > 1.0e-6f; }
};

// ---------------------------------------------------------------------------------------------
// A two-operator FM voice. Phase is never reset and the level is slewed, so a retrigger cannot click.
// ---------------------------------------------------------------------------------------------
struct FmVoice
{
    float phC = 0.0f, phM = 0.0f, incC = 0.0f, incM = 0.0f;
    float amp = 0.0f, ampT = 0.0f, ampDecay = 1.0f, ampSlew = 0.02f;
    float idx = 0.0f, idxT = 0.0f, idxDecay = 1.0f, idxFloor = 0.0f, idxSlew = 0.02f;
    float gL = 0.1f, gR = 0.1f;

    bool active() const { return amp > 1.0e-5f || ampT > 1.0e-5f; }
    void kill() { amp = ampT = 0.0f; idx = idxT = 0.0f; }
    void setPan(float gain, float pan01) { gL = gain * (1.0f - pan01) * 1.4f; gR = gain * pan01 * 1.4f; }

    // index values are in cycles (0.16 is a modulation index of 1).
    void hit(const Ctx& c, float hz, float ratio, float level, float index, float ampSeconds,
             float idxSeconds, float floorIndex, float attackMs)
    {
        incC = clamp(hz * c.invSr, 0.0f, 0.2f);
        incM = clamp(incC * ratio, 0.0f, 0.45f);
        ampT = level;
        ampDecay = c.t60(ampSeconds);
        ampSlew = c.slew(attackMs);
        idxT = index;
        idxDecay = c.t60(idxSeconds);
        idxFloor = floorIndex;
        idxSlew = c.slew1;
    }

    void render(float* L, float* R, int n)
    {
        if (!active()) { amp = 0.0f; ampT = 0.0f; return; }
        for (int i = 0; i < n; ++i)
        {
            ampT *= ampDecay;
            amp += ampSlew * (ampT - amp);
            idxT = idxFloor + (idxT - idxFloor) * idxDecay;
            idx += idxSlew * (idxT - idx);
            const float y = sinT(phC + sinT(phM) * idx) * amp;
            phC += incC; if (phC >= 1.0f) phC -= 1.0f;
            phM += incM; if (phM >= 1.0f) phM -= 1.0f;
            L[i] += y * gL;
            R[i] += y * gR;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Kick, snare / clap and hat, shared by BREAKS and PULSE.
// ---------------------------------------------------------------------------------------------
struct DrumKit
{
    Xorshift32 rng{0x51ED270Bu};
    float invSr = 1.0f / 44100.0f, atk = 0.05f;
    // kick
    float kPh = 0.0f, kPitch = 0.0f, kPitchDecay = 0.99f, kAmp = 0.0f, kAmpT = 0.0f, kAmpDecay = 0.999f;
    float kBase = 47.0f, kSweep = 150.0f, kLevel = 0.5f;
    // snare or clap
    float sPh = 0.0f, sTone = 0.0f, sToneT = 0.0f, sToneDecay = 0.99f, sNoise = 0.0f, sNoiseT = 0.0f, sNoiseDecay = 0.99f;
    float sHz = 190.0f, sToneLevel = 0.3f, sNoiseLevel = 0.5f, sLpA = 0.5f, sHpA = 0.1f, sLp = 0.0f, sHp = 0.0f;
    // hat
    float hAmp = 0.0f, hAmpT = 0.0f, hDecay = 0.99f, hClosed = 0.99f, hOpen = 0.999f, hLpA = 0.6f, hLp = 0.0f, hLp2 = 0.0f;
    float hLevel = 0.2f;

    static float lpCoef(float hz, float sr) { return clamp(1.0f - std::exp(-6.2832f * hz / sr), 0.0f, 1.0f); }

    void prepare(const Ctx& c, bool dance)
    {
        invSr = c.invSr;
        atk = c.slewHalf;
        if (dance)
        {
            kBase = 50.0f; kSweep = 170.0f; kLevel = 0.45f;
            kPitchDecay = c.t60(0.10f); kAmpDecay = c.t60(0.42f);
            sHz = 0.0f; sToneLevel = 0.0f; sNoiseLevel = 0.55f;
            sToneDecay = c.t60(0.05f); sNoiseDecay = c.t60(0.24f);
            sLpA = lpCoef(3400.0f, c.sr); sHpA = lpCoef(850.0f, c.sr);
            hLevel = 0.16f;
        }
        else
        {
            kBase = 47.0f; kSweep = 140.0f; kLevel = 0.45f;
            kPitchDecay = c.t60(0.13f); kAmpDecay = c.t60(0.5f);
            sHz = 188.0f; sToneLevel = 0.26f; sNoiseLevel = 0.5f;
            sToneDecay = c.t60(0.11f); sNoiseDecay = c.t60(0.21f);
            sLpA = lpCoef(7000.0f, c.sr); sHpA = lpCoef(1100.0f, c.sr);
            hLevel = 0.19f;
        }
        hLpA = lpCoef(7000.0f, c.sr);
        hClosed = c.t60(0.045f);
        hOpen = c.t60(0.30f);
    }

    void kill()
    {
        kAmp = kAmpT = kPitch = 0.0f;
        sTone = sToneT = sNoise = sNoiseT = 0.0f; sLp = sHp = 0.0f;
        hAmp = hAmpT = 0.0f; hLp = hLp2 = 0.0f;
    }

    void kick(float vel) { if (kAmp < 0.02f) kPh = 0.0f; kPitch = 1.0f; kAmpT = vel; }
    void snare(float vel) { sToneT = vel; sNoiseT = vel; }
    void hat(float vel, bool open) { hAmpT = vel; hDecay = open ? hOpen : hClosed; }

    bool active() const
    {
        return kAmp > 1.0e-5f || kAmpT > 1.0e-5f || sNoise > 1.0e-5f || sNoiseT > 1.0e-5f
            || sTone > 1.0e-5f || sToneT > 1.0e-5f || hAmp > 1.0e-5f || hAmpT > 1.0e-5f;
    }

    void render(float* L, float* R, int n)
    {
        if (!active()) return;
        for (int i = 0; i < n; ++i)
        {
            const float w = rng.nextBipolar();
            // kick: a sine that drops in pitch
            kPitch *= kPitchDecay;
            kAmpT *= kAmpDecay;
            kAmp += atk * (kAmpT - kAmp);
            kPh += (kBase + kSweep * kPitch) * invSr; if (kPh >= 1.0f) kPh -= 1.0f;
            const float k = sinT(kPh) * kAmp * kLevel;
            // snare: a short tone and band-limited noise
            sToneT *= sToneDecay; sTone += atk * (sToneT - sTone);
            sNoiseT *= sNoiseDecay; sNoise += atk * (sNoiseT - sNoise);
            sPh += sHz * invSr; if (sPh >= 1.0f) sPh -= 1.0f;
            sLp += sLpA * (w - sLp);
            sHp += sHpA * (sLp - sHp);
            const float s = sinT(sPh) * sTone * sToneLevel + (sLp - sHp) * sNoise * sNoiseLevel;
            // hat: noise with the lows taken out twice
            hAmpT *= hDecay; hAmp += atk * (hAmpT - hAmp);
            hLp += hLpA * (w - hLp);
            const float h1 = w - hLp;
            hLp2 += hLpA * (h1 - hLp2);
            const float h = (h1 - hLp2) * hAmp * hLevel;
            const float centre = k + s;
            L[i] += centre + h * 0.75f;
            R[i] += centre + h * 1.2f;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// 90.9 KEYS - electric piano chords, a soft bass and small licks. No drums.
// ---------------------------------------------------------------------------------------------
const int8_t   kKeysChord[6][4] = {{51, 55, 58, 62}, {56, 60, 63, 67}, {55, 58, 60, 63},
                                   {53, 59, 63, 68}, {53, 56, 60, 62}, {53, 58, 62, 67}};
const int8_t   kKeysBass[6]     = {36, 41, 44, 43, 38, 43};
const uint8_t  kKeysProg[3][4]  = {{0, 1, 2, 3}, {0, 2, 4, 3}, {1, 5, 2, 3}};
const uint16_t kKeysComp[4]     = {0x0441, 0x0909, 0x4081, 0x1044};
const int8_t   kKeysLead[6]     = {72, 75, 77, 79, 82, 84};

struct KeysStation
{
    FmVoice v[8];
    FmVoice lead[2];
    FmVoice bass;
    int next = 0, nextLead = 0;
    float tremPh = 0.0f, invSr = 1.0f / 44100.0f;

    void prepare(const Ctx& c) { invSr = c.invSr; bass.setPan(0.30f, 0.5f); }
    void kill() { for (FmVoice& x : v) x.kill(); for (FmVoice& x : lead) x.kill(); bass.kill(); }

    static int chordAt(int bar)
    {
        const int prog = (int)(hash3(11u, (uint32_t)fdiv(bar, 4), 0u) % 3u);
        return kKeysProg[prog][imod(bar, 4)];
    }

    void playChord(const Ctx& c, int chord, float vel)
    {
        static const float pans[4] = {0.32f, 0.44f, 0.56f, 0.68f};
        for (int k = 0; k < 4; ++k)
        {
            FmVoice& x = v[next];
            next = (next + 1) & 7;
            x.setPan(0.115f, pans[k]);
            x.hit(c, noteHz(kKeysChord[chord][k]) * c.keyRatio, 1.0f, vel, 0.10f + 0.36f * vel, 2.8f, 0.45f, 0.035f, 2.0f);
        }
        // the chord before this one is damped, as if the keys were let go
        const float damp = c.t60(0.7f);
        for (int k = 0; k < 4; ++k) v[(next + k) & 7].ampDecay = damp;
    }

    void playBass(const Ctx& c, int note, float vel)
    {
        bass.hit(c, noteHz(note) * c.keyRatio, 1.0f, vel, 0.12f, 1.3f, 0.3f, 0.02f, 3.0f);
    }

    void activate(const Ctx& c, int step)
    {
        const int chord = chordAt(fdiv(step, 16));
        playChord(c, chord, 0.55f);
        playBass(c, kKeysBass[chord], 0.6f);
    }

    void onStep(const Ctx& c, int step)
    {
        const int s = imod(step, 16), bar = fdiv(step, 16);
        const int chord = chordAt(bar);
        Dice d(hash3(12u, (uint32_t)bar, (uint32_t)s));
        const uint16_t comp = kKeysComp[hash3(13u, (uint32_t)fdiv(bar, 2), 0u) & 3u];
        if ((comp >> s) & 1u) playChord(c, chord, 0.6f + 0.38f * d.next());
        if (s == 0) playBass(c, kKeysBass[chord], 0.95f);
        else if (s == 10 && d.next() < 0.6f) playBass(c, kKeysBass[chord] + (d.next() < 0.5f ? 12 : 7), 0.6f);
        // a two-bar lick, played twice in each four-bar phrase
        Dice l(hash3(14u, (uint32_t)fdiv(bar, 4), (uint32_t)(imod(bar, 2) * 16 + s)));
        if ((s & 1) == 0 && s >= 2 && l.next() < 0.2f)
        {
            FmVoice& x = lead[nextLead];
            nextLead ^= 1;
            const int note = kKeysLead[l.bits() % 6u];
            x.setPan(0.10f, 0.35f + 0.3f * l.next());
            x.hit(c, noteHz(note) * c.keyRatio, 1.0f, 0.55f + 0.4f * l.next(), 0.22f, 1.5f, 0.3f, 0.02f, 2.0f);
        }
    }

    void render(float* L, float* R, int n)
    {
        for (FmVoice& x : v) x.render(L, R, n);
        // slow stereo tremolo, the way a suitcase piano sways
        tremPh += 4.3f * (float)n * invSr; if (tremPh >= 1.0f) tremPh -= 1.0f;
        const float t = 0.22f * sinT(tremPh);
        for (int i = 0; i < n; ++i) { L[i] *= 1.0f + t; R[i] *= 1.0f - t; }
        for (FmVoice& x : lead) x.render(L, R, n);
        bass.render(L, R, n);
    }
};

// ---------------------------------------------------------------------------------------------
// 93.9 BREAKS - a swung drum break with ghost notes and a fill every fourth bar.
// ---------------------------------------------------------------------------------------------
const uint16_t kBreakKick[4] = {0x0481, 0x2409, 0x0605, 0x8441};

struct BreaksStation
{
    DrumKit kit;
    void prepare(const Ctx& c) { kit.prepare(c, false); }
    void kill() { kit.kill(); }

    void onStep(const Ctx&, int step)
    {
        const int s = imod(step, 16), bar = fdiv(step, 16);
        Dice d(hash3(21u, (uint32_t)bar, (uint32_t)s));
        const uint16_t kick = kBreakKick[hash3(22u, (uint32_t)fdiv(bar, 4), (uint32_t)imod(bar, 2)) & 3u];
        if ((kick >> s) & 1u) kit.kick(s == 0 ? 1.0f : 0.78f + 0.2f * d.next());
        const bool fill = imod(bar, 4) == 3 && s >= 12;
        if (s == 4 || s == 12) kit.snare(1.0f);
        else if (fill && d.next() < 0.7f) kit.snare(0.35f + 0.14f * (float)(s - 12));
        else if ((s == 7 || s == 9 || s == 14) && d.next() < 0.3f) kit.snare(0.2f);
        if ((s & 1) == 0)
        {
            const bool open = (s == 6 || s == 14) && d.next() < 0.3f;
            kit.hat(open ? 0.8f : ((s & 3) == 0 ? 0.78f : 0.55f), open);
        }
        else if (d.next() < 0.35f) kit.hat(0.3f, false);
    }

    void render(float* L, float* R, int n) { kit.render(L, R, n); }
};

// ---------------------------------------------------------------------------------------------
// 96.9 TALK - a talk show in no language. A glottal saw through three moving formants, with
// noise for the consonants. Two speakers take turns.
// ---------------------------------------------------------------------------------------------
const float kVowel[7][3] = {{730.0f, 1090.0f, 2440.0f}, {530.0f, 1840.0f, 2480.0f}, {270.0f, 2290.0f, 3010.0f},
                            {570.0f, 840.0f, 2410.0f},  {300.0f, 870.0f, 2240.0f},  {500.0f, 1500.0f, 2500.0f},
                            {660.0f, 1720.0f, 2410.0f}};

struct TalkStation
{
    PolyBlepOsc glot;
    SVF f1, f2, f3, fric;
    OnePoleLP tilt;
    Xorshift32 rng{0x7A11C0DEu};
    Xorshift32 nz{0x0BADF00Du};
    float ft[3] = {500.0f, 1500.0f, 2500.0f}, fc[3] = {500.0f, 1500.0f, 2500.0f};
    float pitch = 110.0f, pitchT = 110.0f, vibPh = 0.0f;
    float vAmp = 0.0f, vAmpT = 0.0f, nAmp = 0.0f, nAmpT = 0.0f, fricHz = 4000.0f;
    float base = 112.0f, scale = 1.0f, tickRate = 2756.0f;
    float vSlew = 0.004f, nSlew = 0.01f;
    int phase = 0, ticksLeft = 1, sylLeft = 0, sylTotal = 1, phraseLeft = 2, burstTicks = 0;
    bool female = false, burstPending = false;

    void prepare(const Ctx& c)
    {
        glot.setSampleRate(c.sr); glot.setWave(Wave::Saw); glot.setFrequency(pitch);
        f1.setSampleRate(c.sr); f2.setSampleRate(c.sr); f3.setSampleRate(c.sr); fric.setSampleRate(c.sr);
        tilt.setCutoff(1400.0f, c.sr);
        tickRate = c.tickRate;
        vSlew = c.slew5; nSlew = c.slew2;
        setFilters();
    }
    void kill()
    {
        vAmp = vAmpT = nAmp = nAmpT = 0.0f;
        f1.reset(); f2.reset(); f3.reset(); fric.reset(); tilt.reset();
        phase = 0; ticksLeft = 1; burstTicks = 0; burstPending = false;
    }
    float r() { return rng.nextFloat(); }
    int ms(float m) const { const int t = (int)(m * 0.001f * tickRate); return t < 1 ? 1 : t; }

    void setFilters()
    {
        f1.set(fc[0], 0.93f); f2.set(fc[1], 0.95f); f3.set(fc[2], 0.95f);
        fric.set(fricHz, 0.6f);
    }

    void beginPause()
    {
        phase = 0; vAmpT = 0.0f; nAmpT = 0.0f;
        if (--phraseLeft <= 0)
        {
            female = !female;
            base = female ? 196.0f + 24.0f * r() : 104.0f + 16.0f * r();
            scale = female ? 1.17f : 1.0f;
            phraseLeft = 2 + (int)(rng.next() % 3u);
            ticksLeft = ms(420.0f + 420.0f * r());
        }
        else ticksLeft = ms(150.0f + 380.0f * r());
    }
    void beginConsonant()
    {
        phase = 1;
        burstPending = false;
        switch (rng.next() % 8u)
        {
            case 0: case 1:   // a stop: a moment of nothing, then a burst into the vowel
                vAmpT = 0.0f; nAmpT = 0.0f; fricHz = 1800.0f + 2400.0f * r(); burstPending = true;
                ticksLeft = ms(40.0f + 30.0f * r()); break;
            case 2:           // s
                vAmpT = 0.0f; nAmpT = 0.5f; fricHz = 5200.0f + 1200.0f * r();
                ticksLeft = ms(60.0f + 50.0f * r()); break;
            case 3:           // sh
                vAmpT = 0.0f; nAmpT = 0.55f; fricHz = 2600.0f + 500.0f * r();
                ticksLeft = ms(60.0f + 50.0f * r()); break;
            case 4:           // m, n
                vAmpT = 0.42f; nAmpT = 0.0f;
                ft[0] = 250.0f * scale; ft[1] = (1100.0f + 500.0f * r()) * scale; ft[2] = 2300.0f * scale;
                ticksLeft = ms(50.0f + 40.0f * r()); break;
            case 5:           // f, h
                vAmpT = 0.0f; nAmpT = 0.2f; fricHz = 1400.0f + 800.0f * r();
                ticksLeft = ms(45.0f + 40.0f * r()); break;
            default:          // l, w, y: the vowels run into each other
                vAmpT = 0.6f; nAmpT = 0.0f;
                ft[0] = 310.0f * scale; ft[1] = (900.0f + 1200.0f * r()) * scale; ft[2] = 2400.0f * scale;
                ticksLeft = ms(30.0f + 30.0f * r()); break;
        }
    }
    void beginVowel()
    {
        phase = 2;
        const float* vw = kVowel[rng.next() % 7u];
        for (int i = 0; i < 3; ++i) ft[i] = vw[i] * scale;
        const float progress = 1.0f - (float)sylLeft / (float)(sylTotal < 1 ? 1 : sylTotal);
        const bool stress = r() < 0.3f;
        pitchT = base * (1.10f - 0.22f * progress + (stress ? 0.12f : 0.0f) + 0.06f * (r() - 0.5f));
        vAmpT = stress ? 1.0f : 0.8f;
        if (burstPending) { nAmpT = 0.6f; burstTicks = ms(12.0f); burstPending = false; }
        else nAmpT = 0.0f;
        float len = stress ? 130.0f + 110.0f * r() : 70.0f + 80.0f * r();
        if (sylLeft <= 1) len *= 1.35f;
        ticksLeft = ms(len);
    }

    // once per control step
    void tick()
    {
        if (--ticksLeft <= 0)
        {
            if (phase == 0) { sylTotal = sylLeft = 3 + (int)(rng.next() % 8u); beginConsonant(); }
            else if (phase == 1) beginVowel();
            else if (--sylLeft <= 0) beginPause();
            else beginConsonant();
        }
        if (burstTicks > 0 && --burstTicks == 0) nAmpT = 0.0f;
        for (int i = 0; i < 3; ++i) fc[i] += 0.02f * (ft[i] - fc[i]);
        pitch += 0.012f * (pitchT - pitch);
        vibPh += 5.2f / tickRate; if (vibPh >= 1.0f) vibPh -= 1.0f;
        glot.setFrequency(clamp(pitch * (1.0f + 0.012f * sinT(vibPh)), 60.0f, 400.0f));
        setFilters();
    }

    void render(float* L, float* R, int n)
    {
        const float k1 = 2.0f - 1.98f * 0.93f, k2 = 2.0f - 1.98f * 0.95f;
        for (int i = 0; i < n; ++i)
        {
            vAmp += vSlew * (vAmpT - vAmp);
            nAmp += nSlew * (nAmpT - nAmp);
            const float w = nz.nextBipolar();
            const float src = (tilt.process(glot.process()) + w * 0.02f) * vAmp;
            const float a = f1.process(src, SVF::Mode::Bandpass) * k1;
            const float b = f2.process(src, SVF::Mode::Bandpass) * k2;
            const float cc = f3.process(src, SVF::Mode::Bandpass) * k2 * 0.7f;
            const float fr = fric.process(w, SVF::Mode::Bandpass) * nAmp * 0.22f;
            const float y = (a + b + cc) * 1.9f + fr;
            L[i] += y;
            R[i] += y;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// 99.9 PULSE - four-on-the-floor kick, clap, hats, a squelchy bass that pumps, chord stabs.
// ---------------------------------------------------------------------------------------------
const int8_t kPulseRoot[4] = {0, 0, -4, -2};
const int8_t kPulseOffs[8] = {0, 0, 0, 12, 7, 10, 3, 12};

struct AcidBass
{
    PolyBlepOsc osc;
    SVF filt;
    float hz = 65.0f, hzT = 65.0f, glide = 0.3f, amp = 0.0f, ampT = 0.0f, ampDecay = 0.999f, ampSlew = 0.01f;
    float env = 0.0f, envDecayTick = 0.99f, accent = 0.0f, hold = 0.999f, cut = 0.99f;

    void prepare(const Ctx& c)
    {
        osc.setSampleRate(c.sr); osc.setWave(Wave::Saw); osc.setFrequency(hz);
        filt.setSampleRate(c.sr); filt.set(400.0f, 0.78f);
        envDecayTick = c.t60Tick(0.25f);
        hold = c.t60(0.7f); cut = c.t60(0.05f);
        ampSlew = c.slew2;
    }
    void kill() { amp = ampT = 0.0f; env = 0.0f; filt.reset(); }
    void note(float f, bool slide, bool acc)
    {
        hzT = f;
        glide = slide ? 0.06f : 0.4f;
        if (amp < 1.0e-3f) hz = f;
        if (!slide || ampT < 0.05f) { ampT = acc ? 1.0f : 0.72f; env = 1.0f; }
        ampDecay = hold;
        accent = acc ? 1.0f : 0.0f;
    }
    void off() { ampDecay = cut; }
    void tick(float sweep01)
    {
        hz += glide * (hzT - hz);
        osc.setFrequency(hz);
        env *= envDecayTick;
        filt.set(170.0f + 500.0f * sweep01 + (650.0f + 1500.0f * sweep01 + 900.0f * accent) * env, 0.78f);
    }
    bool active() const { return amp > 1.0e-5f || ampT > 1.0e-5f; }
    float next()
    {
        ampT *= ampDecay;
        amp += ampSlew * (ampT - amp);
        return filt.process(osc.process(), SVF::Mode::Lowpass) * amp;
    }
};

struct PulseStation
{
    DrumKit kit;
    AcidBass bass;
    FmVoice stab[3];
    float duck = 0.0f, duckSm = 0.0f, duckDecay = 0.999f, duckSlew = 0.01f, sweepPh = 0.0f, tickRate = 2756.0f;

    void prepare(const Ctx& c)
    {
        kit.prepare(c, true);
        bass.prepare(c);
        duckDecay = c.t60(0.45f);
        duckSlew = c.slew2;
        tickRate = c.tickRate;
        static const float pans[3] = {0.3f, 0.5f, 0.7f};
        for (int k = 0; k < 3; ++k) stab[k].setPan(0.085f, pans[k]);
    }
    void kill() { kit.kill(); bass.kill(); for (FmVoice& x : stab) x.kill(); duck = duckSm = 0.0f; }

    void tick()
    {
        sweepPh += 0.033f / tickRate; if (sweepPh >= 1.0f) sweepPh -= 1.0f;   // one sweep every 30 seconds
        bass.tick(0.5f + 0.5f * sinT(sweepPh));
    }

    void onStep(const Ctx& c, int step)
    {
        const int s = imod(step, 16), bar = fdiv(step, 16);
        const int rootOff = kPulseRoot[imod(bar, 4)];
        if ((s & 3) == 0) { kit.kick(1.0f); duck = 1.0f; }
        if (s == 4 || s == 12) kit.snare(0.9f);
        if ((s & 3) == 2) kit.hat(0.8f, true);
        else if (s & 1) kit.hat(0.3f, false);

        Dice d(hash3(41u, (uint32_t)fdiv(bar, 4), (uint32_t)s));
        const bool on = d.next() < 0.72f || s == 0;
        const int note = 36 + rootOff + kPulseOffs[d.bits() % 8u];
        const bool acc = d.next() < 0.25f;
        const bool slide = d.next() < 0.2f;
        if (on) bass.note(noteHz(note) * c.keyRatio, slide, acc);
        else bass.off();

        if ((s == 3 || s == 6 || s == 11) && (hash3(42u, (uint32_t)fdiv(bar, 2), (uint32_t)s) % 3u) != 0u)
        {
            const int root = 60 + rootOff;
            const int third = rootOff == 0 ? 3 : 4;
            const int notes[3] = {root, root + third, root + 7};
            for (int k = 0; k < 3; ++k)
                stab[k].hit(c, noteHz(notes[k]) * c.keyRatio, 2.0f, 0.9f, 0.3f, 0.35f, 0.12f, 0.02f, 1.5f);
        }
    }

    void render(float* L, float* R, int n)
    {
        float tL[kCtl], tR[kCtl];
        for (int i = 0; i < n; ++i) { tL[i] = 0.0f; tR[i] = 0.0f; }
        for (FmVoice& x : stab) x.render(tL, tR, n);
        const bool bassOn = bass.active();
        for (int i = 0; i < n; ++i)
        {
            duck *= duckDecay;
            duckSm += duckSlew * (duck - duckSm);
            const float g = 1.0f - 0.7f * duckSm;      // everything but the drums ducks after each kick
            const float b = bassOn ? bass.next() * 0.3f : 0.0f;
            L[i] += (tL[i] + b) * g;
            R[i] += (tR[i] + b) * g;
        }
        kit.render(L, R, n);
    }
};

// ---------------------------------------------------------------------------------------------
// 102.9 DRIFT - slow pad chords that brighten and dim, a low drone, high glass pings.
// ---------------------------------------------------------------------------------------------
const int8_t  kDriftChord[6][4] = {{48, 55, 62, 63}, {44, 51, 55, 60}, {51, 58, 65, 67},
                                   {46, 53, 60, 62}, {53, 56, 60, 67}, {55, 58, 62, 65}};
const int8_t  kDriftRoot[6]     = {36, 32, 39, 34, 41, 43};
const uint8_t kDriftSeq[3][4]   = {{0, 1, 2, 3}, {0, 4, 1, 3}, {0, 2, 4, 5}};

struct DriftStation
{
    FmVoice pad[8];      // two banks of four, so one chord can fade while the next one rises
    FmVoice drone[2];
    FmVoice ping[3];
    float lfoPh[8] = {0.0f, 0.13f, 0.31f, 0.47f, 0.59f, 0.71f, 0.83f, 0.97f};
    int chord = 0, nextPing = 0;
    float tickRate = 2756.0f;

    void prepare(const Ctx& c)
    {
        tickRate = c.tickRate;
        static const float pans[4] = {0.25f, 0.75f, 0.4f, 0.6f};
        for (int k = 0; k < 8; ++k) pad[k].setPan(0.085f, pans[k & 3]);
        drone[0].setPan(0.13f, 0.5f); drone[1].setPan(0.13f, 0.5f);
    }
    void kill() { for (FmVoice& x : pad) x.kill(); for (FmVoice& x : drone) x.kill(); for (FmVoice& x : ping) x.kill(); }

    static int chordAt(int slot)
    {
        const int seq = (int)(hash3(51u, (uint32_t)fdiv(slot, 4), 0u) % 3u);
        return kDriftSeq[seq][imod(slot, 4)];
    }

    void startChord(const Ctx& c, int slot, float attackSeconds)
    {
        chord = chordAt(slot);
        const int bank = imod(slot, 2);
        const float rise = clamp(1.0f / (attackSeconds * c.sr), 0.0f, 1.0f);
        const float fall = clamp(1.0f / (0.9f * c.sr), 0.0f, 1.0f);
        for (int k = 0; k < 4; ++k)
        {
            FmVoice& x = pad[bank * 4 + k];
            const float detune = 1.0f + 0.004f * ((float)(hash3(52u, (uint32_t)slot, (uint32_t)k) & 255u) / 255.0f - 0.5f);
            x.incC = clamp(noteHz(kDriftChord[chord][k]) * c.keyRatio * detune * c.invSr, 0.0f, 0.2f);
            x.incM = x.incC;
            x.ampT = 1.0f; x.ampDecay = 1.0f; x.ampSlew = rise;
            x.idxDecay = 1.0f; x.idxFloor = 0.0f; x.idxSlew = c.slew12;
            FmVoice& old = pad[(1 - bank) * 4 + k];
            old.ampT = 0.0f; old.ampSlew = fall;
        }
        FmVoice& dr = drone[bank];
        dr.incC = clamp(noteHz(kDriftRoot[chord]) * c.keyRatio * c.invSr, 0.0f, 0.2f);
        dr.incM = dr.incC;
        dr.ampT = 1.0f; dr.ampDecay = 1.0f; dr.ampSlew = rise;
        dr.idxT = 0.05f; dr.idxDecay = 1.0f; dr.idxFloor = 0.0f; dr.idxSlew = c.slew12;
        drone[1 - bank].ampT = 0.0f; drone[1 - bank].ampSlew = fall;
    }

    void activate(const Ctx& c, int step) { startChord(c, fdiv(step, 32), 0.3f); }

    void onStep(const Ctx& c, int step)
    {
        if (imod(step, 32) == 0) startChord(c, fdiv(step, 32), 1.3f);
        if ((step & 1) == 0)
        {
            Dice d(hash3(53u, (uint32_t)step, 0u));
            if (d.next() < 0.26f)
            {
                FmVoice& x = ping[nextPing];
                nextPing = (nextPing + 1) % 3;
                const int note = kDriftChord[chord][d.bits() % 4u] + (d.next() < 0.5f ? 12 : 24);
                x.setPan(0.075f, 0.15f + 0.7f * d.next());
                x.hit(c, noteHz(note) * c.keyRatio, 2.0f, 0.5f + 0.5f * d.next(), 0.1f, 3.2f, 0.7f, 0.0f, 4.0f);
            }
        }
    }

    // once per control step: each pad voice brightens and dims at its own slow rate
    void tick()
    {
        for (int k = 0; k < 8; ++k)
        {
            lfoPh[k] += (0.05f + 0.011f * (float)k) / tickRate; if (lfoPh[k] >= 1.0f) lfoPh[k] -= 1.0f;
            pad[k].idxT = 0.06f + 0.13f * (0.5f + 0.5f * sinT(lfoPh[k]));
        }
    }

    void render(float* L, float* R, int n)
    {
        for (FmVoice& x : pad) x.render(L, R, n);
        for (FmVoice& x : drone) x.render(L, R, n);
        for (FmVoice& x : ping) x.render(L, R, n);
    }
};

// ---------------------------------------------------------------------------------------------
// 105.9 BELLS - a kalimba pattern that answers itself three steps later, and a wooden bass note.
// ---------------------------------------------------------------------------------------------
const int8_t kPent[5] = {0, 3, 5, 7, 10};

struct BellsStation
{
    FmVoice v[8];
    FmVoice bass;
    int next = 0;

    void prepare(const Ctx&) { bass.setPan(0.26f, 0.5f); }
    void kill() { for (FmVoice& x : v) x.kill(); bass.kill(); }

    // The note this step plays, or -1. `pan` gets 0 or 1.
    static int noteAt(int step, int& pan)
    {
        const int s = imod(step, 16), bar = fdiv(step, 16), m = s & 7;
        uint32_t h = hash3(61u, (uint32_t)fdiv(bar, 4), (uint32_t)m);
        if (s >= 8 && (hash3(62u, (uint32_t)bar, (uint32_t)m) & 3u) == 0u) h = hash3(63u, (uint32_t)bar, (uint32_t)m);
        const bool on = m == 0 || (h % 100u) < 58u;
        const int di = (int)((h >> 8) % 11u);
        pan = di & 1;
        return on ? 60 + 12 * (di / 5) + kPent[di % 5] : -1;
    }

    void pluck(const Ctx& c, int note, float vel, int pan)
    {
        FmVoice& x = v[next];
        next = (next + 1) & 7;
        x.setPan(0.2f, pan ? 0.72f : 0.28f);
        x.hit(c, noteHz(note) * c.keyRatio, 3.0f, vel, 0.05f + 0.26f * vel, 1.2f, 0.11f, 0.0f, 1.0f);
    }

    void onStep(const Ctx& c, int step)
    {
        const int s = imod(step, 16), bar = fdiv(step, 16);
        int pan = 0;
        const int note = noteAt(step, pan);
        if (note >= 0)
        {
            Dice d(hash3(64u, (uint32_t)step, 0u));
            pluck(c, note, 0.62f + 0.36f * d.next(), pan);
        }
        else
        {
            const int echo = noteAt(step - 3, pan);
            if (echo >= 0) pluck(c, echo, 0.26f, 1 - pan);
        }
        const int root = 48 + kPulseRoot[imod(bar, 4)];
        if (s == 0) bass.hit(c, noteHz(root) * c.keyRatio, 1.0f, 0.95f, 0.14f, 1.0f, 0.12f, 0.01f, 1.5f);
        else if (s == 10 && (hash3(65u, (uint32_t)bar, 0u) & 1u))
            bass.hit(c, noteHz(root + 7) * c.keyRatio, 1.0f, 0.7f, 0.14f, 0.8f, 0.12f, 0.01f, 1.5f);
    }

    void render(float* L, float* R, int n)
    {
        for (FmVoice& x : v) x.render(L, R, n);
        bass.render(L, R, n);
    }
};

// ---------------------------------------------------------------------------------------------
// A sine that glides: the whistles and the utility signals between stations.
// ---------------------------------------------------------------------------------------------
struct Tone
{
    float ph = 0.0f, hz = 800.0f, hzT = 800.0f, amp = 0.0f, ampT = 0.0f, glide = 0.002f, ampSlew = 0.002f;
    bool live() const { return amp > 1.0e-5f || ampT > 1.0e-5f; }
    float next(float invSr)
    {
        hz += glide * (hzT - hz);
        amp += ampSlew * (ampT - amp);
        ph += hz * invSr; if (ph >= 1.0f) ph -= 1.0f;
        return sinT(ph) * amp;
    }
};

// ---------------------------------------------------------------------------------------------
// A small room: four delay lines feeding each other, two all-pass diffusers in front.
// ---------------------------------------------------------------------------------------------
struct Room
{
    DelayLine d[4], ap[2];
    int len[4] = {1021, 1249, 1487, 1733}, apLen[2] = {149, 211};
    float lp[4] = {0.0f, 0.0f, 0.0f, 0.0f}, damp = 0.5f;

    void prepare(float sr)
    {
        static const int base[4] = {1021, 1249, 1487, 1733};
        static const int apBase[2] = {149, 211};
        const float k = sr / 44100.0f;
        for (int i = 0; i < 4; ++i) { len[i] = (int)((float)base[i] * k) | 1; d[i].prepare(len[i] + 4); }
        for (int i = 0; i < 2; ++i) { apLen[i] = (int)((float)apBase[i] * k) | 1; ap[i].prepare(apLen[i] + 4); }
        damp = clamp(1.0f - std::exp(-6.2832f * 5200.0f / sr), 0.0f, 1.0f);
    }
    void reset()
    {
        for (DelayLine& x : d) x.reset();
        for (DelayLine& x : ap) x.reset();
        for (float& x : lp) x = 0.0f;
    }
    void process(float in, float& outL, float& outR)
    {
        for (int i = 0; i < 2; ++i)
        {
            const float delayed = ap[i].readInt(apLen[i]);
            const float w = in + 0.6f * delayed;
            ap[i].write(w);
            in = delayed - 0.6f * w;
        }
        float s[4];
        for (int i = 0; i < 4; ++i) { lp[i] += damp * (d[i].readInt(len[i]) - lp[i]); s[i] = lp[i]; }
        const float half = 0.5f * (s[0] + s[1] + s[2] + s[3]);
        for (int i = 0; i < 4; ++i) d[i].write(in + (s[i] - half) * 0.8f);
        outL = s[0] + s[2];
        outR = s[1] + s[3];
    }
};

// A straight wire below 0.9; only the rare peak above that is rounded off, and never past 1.
inline float safeClip(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float y = 0.9f + 0.1f * fastTanh((a - 0.9f) * 10.0f);
    return x < 0.0f ? -y : y;
}

} // namespace

// =============================================================================================
// The plugin
// =============================================================================================
class MPCRadio : public mpc::Plugin
{
public:
    enum Params
    {
        kVolume = 0, kTune, kStatic, kSqueal, kGhosts, kKey, kSync, kMix,
        kSpeaker, kDrive, kWow, kRoom, kEcho, kEchoTime, kRepeats, kEchoTone,
        kLive, kSignal, kSpkType,
        kNumParams
    };

    MPCRadio() : mpc::Plugin(makeInfo())
    {
        buildTables();

        // The first parameter is nudged by the MPC every time the plugin loads: a plain volume.
        // The unit is written into each value's text by formatValue(), so the unit field stays empty
        // and no screen can show it twice.
        addParam(mpc::floatParam("volume",   "Volume",   -40.0f, 6.0f, 0.0f, "", 1));
        addParam(mpc::floatParam("tune",     "Tune",      87.5f, 108.0f, 95.8f, "", 1));
        addParam(mpc::floatParam("static",   "Static",    0.0f, 100.0f, 50.0f, "", 0));
        addParam(mpc::floatParam("squeal",   "Squeal",    0.0f, 100.0f, 40.0f, "", 0));
        addParam(mpc::floatParam("ghosts",   "Ghosts",    0.0f, 100.0f, 35.0f, "", 0));
        addParam(mpc::choiceParam("key",     "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        addParam(mpc::boolParam ("sync",     "Sync", true));
        addParam(mpc::floatParam("mix",      "Mix",       0.0f, 100.0f, 100.0f, "", 0));
        addParam(mpc::floatParam("speaker",  "Speaker",   0.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::floatParam("drive",    "Drive",     0.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::floatParam("wow",      "Wow",       0.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::floatParam("room",     "Room",      0.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::floatParam("echo",     "Echo",      0.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::logParam  ("echotime", "Echo Time", 40.0f, 1500.0f, 380.0f, "", 0));
        addParam(mpc::floatParam("repeats",  "Repeats",   0.0f, 95.0f, 35.0f, "", 0));
        addParam(mpc::logParam  ("echotone", "Echo Tone", 500.0f, 12000.0f, 4000.0f, "", 0));
        // Added after the original sixteen, so saved projects still find theirs.
        addParam(mpc::boolParam ("live",     "Live", true));   // kept for saved projects; the radio is always live
        {
            // On no page: the plugin sets it itself, and the signal lamps in the tuning window follow it.
            mpc::Param signal = mpc::floatParam("signal", "Signal", 0.0f, 1.0f, 1.0f, "", 2);
            signal.automatable = false;
            addParam(signal);

        // Added after every older control, so saved projects still find those in their places.
        // Which speaker the Speaker knob turns the radio into.
        addParam(mpc::choiceParam("spktype", "Speaker Type", {"Pocket", "Boombox", "Car", "Phone", "Wood", "Horn"}, 0));
        }

        //                         Vol   Tune   Stat  Sque  Ghos  Key Sync Mix   Spkr  Driv  Wow   Room  Echo  Time    Rep   Tone
        // Every preset is on the live dial now. The last three values are Live (kept, always on), the hidden Signal
        // and the speaker type (0 Pocket, 1 Boombox, 2 Car, 3 Phone, 4 Wood, 5 Horn).
        addProgram("Late Night Jazz",   {0.0f, 95.8f,  50.0f, 40.0f, 35.0f, 0, 1, 100.0f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  380.0f, 35.0f, 4000.0f, 1, 1.0f, 0});
        addProgram("Kitchen Hip Hop",   {0.0f, 90.4f,  40.0f, 30.0f, 30.0f, 0, 1, 100.0f, 65.0f, 20.0f, 0.0f,  35.0f, 0.0f,  380.0f, 35.0f, 4000.0f, 1, 0.0f, 0});
        addProgram("Between Stations",  {0.0f, 97.0f,  80.0f, 100.0f, 80.0f, 0, 1, 100.0f, 30.0f, 0.0f,  15.0f, 20.0f, 35.0f, 520.0f, 55.0f, 2500.0f, 1, 0.0f, 4});
        addProgram("Soul On The Phone", {0.0f, 88.6f,  60.0f, 40.0f, 40.0f, 0, 1, 100.0f, 85.0f, 15.0f, 10.0f, 0.0f,  0.0f,  380.0f, 35.0f, 4000.0f, 1, 0.0f, 3});
        addProgram("Dub Talk",          {0.0f, 104.8f, 30.0f, 30.0f, 20.0f, 0, 1, 100.0f, 55.0f, 0.0f,  0.0f,  15.0f, 55.0f, 450.0f, 70.0f, 1800.0f, 1, 0.0f, 5});
        addProgram("Warped Classic",    {0.0f, 106.6f, 40.0f, 30.0f, 30.0f, 0, 1, 100.0f, 0.0f,  0.0f,  70.0f, 30.0f, 30.0f, 290.0f, 45.0f, 5000.0f, 1, 0.0f, 0});
        addProgram("Clean House",       {0.0f, 101.2f, 0.0f,  0.0f,  0.0f,  0, 1, 100.0f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  380.0f, 35.0f, 4000.0f, 1, 0.0f, 0});
        addProgram("Drift Off",         {0.0f, 97.6f,  50.0f, 40.0f, 60.0f, 0, 0, 100.0f, 0.0f,  0.0f,  25.0f, 45.0f, 25.0f, 700.0f, 50.0f, 3000.0f, 1, 0.0f, 0});
        // The live band (the last two values are Live and the hidden Signal). Added after the original eight.
        addProgram("Live Soul 45s",     {0.0f, 88.6f,  50.0f, 40.0f, 35.0f, 0, 1, 100.0f, 60.0f, 15.0f, 12.0f, 0.0f,  0.0f,  380.0f, 35.0f, 4000.0f, 1, 0.0f, 4});
        addProgram("Live Dub Reggae",   {0.0f, 94.0f,  40.0f, 30.0f, 30.0f, 0, 1, 100.0f, 70.0f, 0.0f,  0.0f,  20.0f, 45.0f, 430.0f, 65.0f, 2200.0f, 1, 0.0f, 1});
        addProgram("Live Kitchen Talk", {0.0f, 104.8f, 45.0f, 30.0f, 25.0f, 0, 1, 100.0f, 70.0f, 20.0f, 0.0f,  35.0f, 0.0f,  380.0f, 35.0f, 4000.0f, 1, 0.0f, 0});
        addProgram("Car Radio Rock",    {0.0f, 99.4f,  30.0f, 20.0f, 25.0f, 0, 1, 100.0f, 75.0f, 0.0f,  0.0f,  10.0f, 0.0f,  380.0f, 35.0f, 4000.0f, 1, 0.0f, 2});
    }

    // Values are shown with their units.
    bool formatValue(int index, float v, std::string& out) override
    {
        char buf[32];
        switch (index)
        {
            case kVolume:   std::snprintf(buf, sizeof(buf), "%.1f dB", (double)(std::fabs(v) < 0.05f ? 0.0f : v)); break;
            case kTune:     std::snprintf(buf, sizeof(buf), "%.1f MHz", (double)(std::floor(v * 10.0f + 0.5f) * 0.1f)); break;
            case kEchoTime: std::snprintf(buf, sizeof(buf), "%d ms", (int)std::lround(v)); break;
            case kEchoTone:
                if (v >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.1f kHz", (double)(v * 0.001f));
                else              std::snprintf(buf, sizeof(buf), "%d Hz", (int)std::lround(v));
                break;
            case kKey: case kSync: case kLive: case kSpkType: return false;
            default:        std::snprintf(buf, sizeof(buf), "%d %%", (int)std::lround(v)); break;
        }
        out = buf;
        return true;
    }

    // All memory is allocated here.
    void prepare(double sampleRate, int maxBlockSize) override
    {
        cx_.setSampleRate((float)sampleRate);
        const float sr = cx_.sr;

        keys_.prepare(cx_); breaks_.prepare(cx_); talk_.prepare(cx_);
        pulse_.prepare(cx_); drift_.prepare(cx_); bells_.prepare(cx_);

        wowL_.prepare((int)(sr * 0.03f) + 8);
        wowR_.prepare((int)(sr * 0.03f) + 8);
        echoL_.prepare((int)(sr * 1.6f) + 8);
        echoR_.prepare((int)(sr * 1.6f) + 8);
        room_.prepare(sr);

        // the live band: the stream is only started here; nothing is fetched until a station is tuned in
        stream_.start(sampleRate);
        {
            const size_t cap = (size_t)(maxBlockSize > 8192 ? maxBlockSize : 8192);
            netL_.assign(cap, 0.0f);
            netR_.assign(cap, 0.0f);
        }
        liveCoef_ = 1.0f - std::exp(-kCtlF / (0.2f * sr));

        spkHpL_.setSampleRate(sr); spkHpR_.setSampleRate(sr);
        spkPkL_.setSampleRate(sr); spkPkR_.setSampleRate(sr);
        spkLpL_.setSampleRate(sr); spkLpR_.setSampleRate(sr);

        ghostLp_.setCutoff(2800.0f, sr);
        ghostHp_.setCutoff(500.0f, sr);
        hissLpL_.setCutoff(6500.0f, sr);
        hissLpR_.setCutoff(6500.0f, sr);
        hissHpL_.setCutoff(250.0f, sr);
        hissHpR_.setCutoff(250.0f, sr);
        crkLp_.setCutoff(4500.0f, sr);
        echoHpL_.setCutoff(120.0f, sr);
        echoHpR_.setCutoff(120.0f, sr);
        crkDecay_ = cx_.t60(0.004f);

        vol_.setTime(20.0f, sr);      mix_.setTime(20.0f, sr);
        wow_.setTime(60.0f, sr);      drive_.setTime(30.0f, sr);
        spk_.setTime(30.0f, sr);      echoSend_.setTime(30.0f, sr);
        echoTime_.setTime(120.0f, sr); echoFb_.setTime(30.0f, sr);
        roomAmt_.setTime(30.0f, sr);

        tuneCoef_ = 1.0f - std::exp(-kCtlF / (0.06f * sr));
        slowCoef_ = 1.0f - std::exp(-kCtlF / (0.03f * sr));

        for (Tone& t : whistle_) { t.glide = cx_.slew(8.0f); t.ampSlew = cx_.slew12; }
        alien_[0].glide = cx_.slew1;       alien_[0].ampSlew = cx_.slew12;
        alien_[1].glide = cx_.slew5;       alien_[1].ampSlew = cx_.slew5;
        alien_[2].glide = cx_.slew1;       alien_[2].ampSlew = cx_.slew(3.0f);
        alien_[3].glide = cx_.slew(4.0f);  alien_[3].ampSlew = cx_.slew5;
        alien_[2].hz = alien_[2].hzT = 780.0f;

        readParams();
        reset();
    }

    // Clears every memory of sound. Runs on the audio thread after a preset or project load.
    void reset() override
    {
        readParams();
        keys_.kill(); breaks_.kill(); talk_.kill(); pulse_.kill(); drift_.kill(); bells_.kill();
        for (Slot& s : slot_) { s.clean.snap(0.0f); s.ghost.snap(0.0f); s.active = false; s.lastStep = INT_MIN; }
        for (Tone& t : whistle_) { t.amp = t.ampT = 0.0f; }
        for (Tone& t : alien_) { t.amp = t.ampT = 0.0f; }
        numLive_ = 0;
        hiss_.snap(0.0f); wowMod_.snap(cx_.sr * 0.006f);
        crk_ = 0.0f; crackleLevel_ = 0.0f; ghostOn_ = false; noiseOn_ = false;

        wowL_.reset(); wowR_.reset(); echoL_.reset(); echoR_.reset(); room_.reset();
        spkHpL_.reset(); spkHpR_.reset(); spkLpL_.reset(); spkLpR_.reset();
        ghostLp_.reset(); ghostHp_.reset(); hissLpL_.reset(); hissLpR_.reset(); hissHpL_.reset(); hissHpR_.reset();
        crkLp_.reset(); echoLpL_.reset(); echoLpR_.reset(); echoHpL_.reset(); echoHpR_.reset();

        vol_.snap(volT_);           mix_.snap(mixT_);
        wow_.snap(wowT_);           drive_.snap(driveT_);
        spk_.snap(spkT_);           echoSend_.snap(echoT_);
        echoTime_.snap(echoTimeT_); echoFb_.snap(echoFbT_);
        roomAmt_.snap(roomT_);
        tunePos_ = tuneT_;
        spkLast_ = -1.0f; echoToneLast_ = -1.0f;
        spkPkL_.reset(); spkPkR_.reset();
        for (int k = 0; k < kSpkVals; ++k) spkCur_[k] = kSpk[spkType_][k];
        bandMix_ = liveT_ ? 1.0f : 0.0f;
        netClean_.snap(0.0f); netGhost_.snap(0.0f);
        netLevel_ = 0.0f; netOn_ = false; netDwell_ = 0; netAt_ = 0; netHave_ = 0;
        lampsHold_ = (int)(cx_.tickRate * 0.5f) + 1;
        // A preset has just moved the controls: in a moment, show the MPC's screen where they stand now.
        showHold_ = (int)(cx_.tickRate * 0.05f) + 1;
        showNext_ = 0;
        ctlLeft_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int numFrames = ctx.numFrames;
        if (numFrames <= 0 || ctx.outputs == nullptr || ctx.numOutputs < 1 || ctx.outputs[0] == nullptr) return;

        const float* inL = (ctx.inputs != nullptr && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs != nullptr && ctx.numInputs > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = (ctx.numOutputs > 1 && ctx.outputs[1] != nullptr) ? ctx.outputs[1] : nullptr;

        readParams();
        vol_.setTarget(volT_);           mix_.setTarget(mixT_);
        wow_.setTarget(wowT_);           drive_.setTarget(driveT_);
        spk_.setTarget(spkT_);           echoSend_.setTarget(echoT_);
        echoTime_.setTarget(echoTimeT_); echoFb_.setTarget(echoFbT_);
        roomAmt_.setTarget(roomT_);

        // The song's clock. While the song plays, the stations sit on its bars.
        // The live band's stream is read for the whole block on every call, even while it is turned down,
        // so that it never falls behind what is being sent. read() never waits.
        stream_.open(streamWanted_);
        netHave_ = numFrames < (int)netL_.size() ? numFrames : (int)netL_.size();
        if (netHave_ > 0) stream_.read(netL_.data(), netR_.data(), netHave_);

        const mpc::Transport& tr = ctx.transport;
        const double bpm = (tr.bpm > 20.0 && tr.bpm < 400.0) ? tr.bpm : 120.0;
        const double beatsPerSample = bpm / 60.0 / (double)cx_.sr;
        if (tr.valid && tr.playing && tr.ppqValid && std::isfinite(tr.ppqPos)) songBeat_ = tr.ppqPos;

        int i = 0;
        while (i < numFrames)
        {
            if (ctlLeft_ <= 0) { controlTick(songBeat_ + (double)i * beatsPerSample); ctlLeft_ = kCtl; }
            const int n = (numFrames - i) < ctlLeft_ ? (numFrames - i) : ctlLeft_;
            netAt_ = i;
            renderChunk(inL != nullptr ? inL + i : nullptr, inR != nullptr ? inR + i : nullptr,
                        outL + i, outR != nullptr ? outR + i : nullptr, n);
            ctlLeft_ -= n;
            i += n;
        }
        // a block longer than was promised: keep the stream in step all the same
        for (int extra = numFrames - netHave_; extra > 0; )
        {
            const int m = extra < (int)netL_.size() ? extra : (int)netL_.size();
            if (m <= 0) break;
            stream_.read(netL_.data(), netR_.data(), m);
            extra -= m;
        }
        songBeat_ += (double)numFrames * beatsPerSample;
        if (songBeat_ > 1.0e7 || songBeat_ < -1.0e7) songBeat_ = 0.0;
    }

private:
    struct Slot
    {
        Ramp clean, ghost;
        double beat = 0.0;
        int lastStep = INT_MIN;
        bool active = false;
        float flutPh = 0.0f;
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "MPC Radio";
        info.vendor       = "Steph";     // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('S', 'O', 'o', '3');   // must match plugin.json "uid"
        info.version      = 1000;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;           // the radio plays on by itself, so the tail has no end
        return info;
    }

    // Parameters are read once per block into plain targets.
    void readParams()
    {
        volT_   = dbToGain(get(kVolume));
        tuneT_  = clamp(std::floor(get(kTune) * 10.0f + 0.5f) * 0.1f, 87.5f, 108.0f);
        static_ = get(kStatic) * 0.01f;
        squeal_ = get(kSqueal) * 0.01f;
        ghosts_ = get(kGhosts) * 0.01f;
        const int key = clamp(getInt(kKey), 0, 11);
        if (key != key_)
        {
            key_ = key;
            cx_.keyRatio = std::pow(2.0f, (float)(key > 6 ? key - 12 : key) / 12.0f);
        }
        songTempo_ = getBool(kSync);
        liveT_ = true;   // the radio is always live now. Live, Key and Sync are kept only so saved projects load.
        spkType_ = clamp(getInt(kSpkType), 0, kNumSpk - 1);
        mixT_      = 1.0f;   // Mix is off the screen: always all radio. The control is kept, unread, for saved projects.
        spkT_      = get(kSpeaker) * 0.01f;
        driveT_    = get(kDrive) * 0.01f;
        wowT_      = get(kWow) * 0.01f;
        roomT_     = get(kRoom) * 0.01f;
        echoT_     = get(kEcho) * 0.01f;
        echoTimeT_ = clamp(get(kEchoTime) * 0.001f * cx_.sr, 8.0f, (float)echoL_.maxDelay() - 2.0f);
        echoFbT_   = clamp(get(kRepeats) * 0.01f, 0.0f, 0.95f);
        echoToneT_ = clamp(get(kEchoTone), 300.0f, 16000.0f);
    }

    static int stepFromBeat(double beat, float swing)
    {
        const double pos = beat * 2.0;
        const double fl = std::floor(pos);
        return (int)fl * 2 + ((pos - fl) >= (double)swing ? 1 : 0);
    }

    void activateStation(int s, int step)
    {
        switch (s)
        {
            case 1: keys_.activate(cx_, step); break;
            case 5: drift_.activate(cx_, step); break;
            default: break;
        }
    }
    void stepStation(int s, int step)
    {
        switch (s)
        {
            case 1: keys_.onStep(cx_, step); break;
            case 2: breaks_.onStep(cx_, step); break;
            case 4: pulse_.onStep(cx_, step); break;
            case 5: drift_.onStep(cx_, step); break;
            case 6: bells_.onStep(cx_, step); break;
            default: break;
        }
    }
    void killStation(int s)
    {
        switch (s)
        {
            case 1: keys_.kill(); break;
            case 2: breaks_.kill(); break;
            case 3: talk_.kill(); break;
            case 4: pulse_.kill(); break;
            case 5: drift_.kill(); break;
            case 6: bells_.kill(); break;
            default: break;
        }
    }

    // Everything slow, once every 16 samples.
    void controlTick(double songBeat)
    {
        const float tickSec = kCtlF * cx_.invSr;

        tunePos_ += tuneCoef_ * (tuneT_ - tunePos_);
        if (std::fabs(tuneT_ - tunePos_) < 0.002f) tunePos_ = tuneT_;

        // Which band the dial is on: 0 is the radio's own stations, 1 is the live ones.
        // Glided, so that flipping the switch fades one band out and the other in.
        bandMix_ += slowCoef_ * ((liveT_ ? 1.0f : 0.0f) - bandMix_);
        if (bandMix_ < 1.0e-4f) bandMix_ = 0.0f; else if (bandMix_ > 0.9999f) bandMix_ = 1.0f;
        const float fmAmt = 1.0f - bandMix_;

        whWob_ += 5.5f * tickSec; if (whWob_ >= 1.0f) whWob_ -= 1.0f;

        float maxClean = 0.0f;
        bool anyGhost = false;
        for (int s = 0; s < kNumStations; ++s)
        {
            Slot& sl = slot_[s];
            const float d = std::fabs(tunePos_ - kStationMHz[s]);

            // how the station is heard at this distance from its frequency
            const float off = smooth01(0.12f, 0.45f, d);
            const float clean = (1.0f - off) * fmAmt;
            const float fringe = off * (1.0f - smooth01(0.45f, 0.95f, d));
            sl.flutPh += (0.11f + 0.05f * (float)s) * tickSec; if (sl.flutPh >= 1.0f) sl.flutPh -= 1.0f;
            float far = clamp(1.0f - (d - 0.3f) / 2.2f, 0.0f, 1.0f);
            far = far * far * off * (0.35f + 0.65f * (0.5f + 0.5f * sinT(sl.flutPh)));
            const float ghost = std::fmax(fringe * 0.7f, ghosts_ * 0.8f * far) * fmAmt;

            sl.clean.to(clean);
            sl.ghost.to(ghost);
            const bool active = sl.clean.live() || sl.ghost.live();
            if (sl.ghost.live()) anyGhost = true;
            if (clean > maxClean) maxClean = clean;

            // the whistle that slides as the dial nears this station
            const float w = smooth01(0.13f, 0.5f, d) * (1.0f - smooth01(0.9f, 1.7f, d));
            whistle_[s].ampT = 0.13f * squeal_ * w * fmAmt;
            whistle_[s].hzT = (160.0f + 2300.0f * d) * (1.0f + 0.025f * sinT(whWob_ + 0.37f * (float)s));

            // the station's own clock
            if (songTempo_) sl.beat = songBeat;
            else            sl.beat += (double)(kOwnBpm[s] * (1.0f / 60.0f) * tickSec);
            if (sl.beat > 1.0e7) sl.beat = 0.0;
            const int step = stepFromBeat(sl.beat, kSwing[s]);
            // a clock that slips back by a single step is jitter, not a jump, and is ignored
            const bool stepped = sl.lastStep == INT_MIN || step > sl.lastStep || step < sl.lastStep - 1;
            if (active && !sl.active) activateStation(s, step);
            if (stepped)
            {
                sl.lastStep = step;
                if (active) stepStation(s, step);
            }
            if (!active && sl.active) killStation(s);
            sl.active = active;
        }
        // The live band: the one internet station nearest the dial. Its gains follow the station that is
        // open, and a station only changes once that one has faded to nothing, so a change never clicks.
        {
            int nearest = 0; float nd = 99.0f;
            for (int k = 0; k < kNumLive; ++k)
            {
                const float dk = std::fabs(tunePos_ - kLiveMHz[k]);
                if (dk < nd) { nd = dk; nearest = k; }
            }
            const int want = (liveT_ && nearest < mpforge::AudioStream::streamCount())
                                 ? nearest : (int)mpforge::AudioStream::kNoStream;
            const int cur = streamWanted_;

            float clean = 0.0f, ghost = 0.0f;
            if (cur >= 0 && cur < kNumLive)
            {
                const float d = std::fabs(tunePos_ - kLiveMHz[cur]);
                const float off = smooth01(0.12f, 0.45f, d);
                const float fringe = off * (1.0f - smooth01(0.45f, 0.85f, d));
                netFlut_ += 0.13f * tickSec; if (netFlut_ >= 1.0f) netFlut_ -= 1.0f;
                float far = clamp(1.0f - (d - 0.3f) / 0.55f, 0.0f, 1.0f);
                far = far * far * off * (0.35f + 0.65f * (0.5f + 0.5f * sinT(netFlut_)));
                clean = 1.0f - off;
                ghost = std::fmax(fringe * 0.7f, ghosts_ * 0.8f * far);
            }

            // the stream fades in once its sound is really arriving, and static fills the wait
            const bool playing = cur >= 0 && stream_.opened() == cur
                                 && stream_.state() == mpforge::AudioStream::kPlaying;
            netLevel_ += (playing ? liveCoef_ : slowCoef_) * ((playing ? 1.0f : 0.0f) - netLevel_);
            if (netLevel_ < 1.0e-4f) netLevel_ = 0.0f;

            if (want != netPending_) { netPending_ = want; netDwell_ = 0; }
            else if (netDwell_ < 1000000) ++netDwell_;

            const float gC = clean * netLevel_ * bandMix_;
            const float gG = ghost * netLevel_ * bandMix_;
            if (want != cur && netDwell_ > (int)(cx_.tickRate * 0.25f) && gC < 1.0e-3f && gG < 1.0e-3f)
            {
                streamWanted_ = want;          // process() hands this to the stream at the next block
                netLevel_ = 0.0f;
                netClean_.to(0.0f); netGhost_.to(0.0f);
            }
            else
            {
                netClean_.to(gC); netGhost_.to(gG);
                if (gC > maxClean) maxClean = gC;
            }
            netOn_ = netClean_.live() || netGhost_.live();
            if (netGhost_.live()) anyGhost = true;

            // the whistle that slides as the dial nears a live station
            if (bandMix_ > 0.5f)
            {
                const float w = smooth01(0.13f, 0.5f, nd) * (1.0f - smooth01(0.6f, 0.9f, nd));
                whistle_[0].ampT = 0.13f * squeal_ * w * bandMix_;
                whistle_[0].hzT = (160.0f + 2300.0f * nd) * (1.0f + 0.025f * sinT(whWob_));
            }

            // Signal lamps: how well the dial is locked, on either band. A live station that is tuned in
            // but still arriving blinks one lamp. The hidden Signal control is only set when this changes.
            int lamps = (int)(clamp(maxClean, 0.0f, 1.0f) * 5.0f + 0.5f);
            if (bandMix_ > 0.5f && cur >= 0 && clean > 0.5f && netLevel_ < 0.5f
                && stream_.opened() == cur && stream_.state() == mpforge::AudioStream::kOpening)
            {
                blink_ += 2.5f * tickSec; if (blink_ >= 1.0f) blink_ -= 1.0f;
                lamps = blink_ < 0.5f ? 1 : 0;
            }
            // After a project or preset has loaded (reset), the control stays as the MPC put it back: the
            // plugin only takes note of where the signal stands, and writes again when it next changes.
            {
            // Choosing a preset moves the controls inside the plugin, but the MPC's screen is not told, so
            // the Tune dial and needle stayed on the old station while the new one played. Shortly after a
            // preset or project has loaded (reset), tell the MPC where each control on the screen stands,
            // one per tick, Tune first. Nothing is changed here: each control is only read and reported.
            static const int kShown[] = { kTune, kStatic, kSqueal, kGhosts, kVolume, kSpeaker, kDrive, kWow,
                                          kRoom, kEcho, kEchoTime, kRepeats, kEchoTone, kSpkType };
            static const int kNumShown = (int)(sizeof(kShown) / sizeof(kShown[0]));
            if (showHold_ > 0) --showHold_;
            else if (showNext_ >= 0 && showNext_ < kNumShown) notifyHostParameterChanged(kShown[showNext_++]);
        }
        if (lampsHold_ > 0) { if (--lampsHold_ == 0) lampsShown_ = lamps; }
            else if (lamps != lampsShown_)
            {
                lampsShown_ = lamps;
                setNormalised(kSignal, (float)lamps * 0.2f);
                notifyHostParameterChanged(kSignal);
            }
        }
        ghostOn_ = anyGhost;

        if (slot_[3].active) talk_.tick();
        if (slot_[4].active) pulse_.tick();
        if (slot_[5].active) drift_.tick();

        // air: only heard as far as no station is locked
        const float open = 1.0f - maxClean;
        hiss_.to(static_ * static_ * 0.5f * open);
        crackleLevel_ += slowCoef_ * (static_ * open - crackleLevel_);
        if (--crkTicks_ <= 0) { crkTicks_ = (int)(cx_.tickRate * 0.4f) + 1; crkDensT_ = airRng_.nextFloat(); }
        crkDens_ += 0.004f * (crkDensT_ - crkDens_);
        crackleProb_ = (2.0f + 26.0f * crkDens_ * crkDens_) * cx_.invSr;
        noiseOn_ = hiss_.live() || crackleLevel_ > 1.0e-4f || std::fabs(crk_) > 1.0e-5f;

        tickAliens(tickSec);

        // which tones need rendering
        numLive_ = 0;
        for (int s = 0; s < kNumStations; ++s) if (whistle_[s].live()) live_[numLive_++] = &whistle_[s];
        for (int a = 0; a < kNumAliens; ++a) if (alien_[a].live()) live_[numLive_++] = &alien_[a];

        // Wow: two slow waves and one fast one move a short delay
        wowP1_ += 0.55f * tickSec; if (wowP1_ >= 1.0f) wowP1_ -= 1.0f;
        wowP2_ += 0.93f * tickSec; if (wowP2_ >= 1.0f) wowP2_ -= 1.0f;
        wowP3_ += 6.3f * tickSec;  if (wowP3_ >= 1.0f) wowP3_ -= 1.0f;
        wowMod_.to(cx_.sr * (0.006f + 0.0045f * (0.6f * sinT(wowP1_) + 0.4f * sinT(wowP2_)) + 0.00015f * sinT(wowP3_)));

        // Drive: more gain in, less level out
        const float dr = drive_.value();
        driveG_ = 1.0f + 2.0f * dr + 17.0f * dr * dr;
        driveComp_ = 0.16f / fastTanh(0.16f * driveG_);
        driveMix_ = dr * 5.0f > 1.0f ? 1.0f : dr * 5.0f;

        // Speaker: the two corners close in
        const float sp = spk_.value();
        // Glide towards the chosen speaker's numbers, so that changing the type reshapes the sound
        // over about a tenth of a second and cannot click.
        bool spkMoving = false;
        {
            const float* want = kSpk[spkType_];
            for (int k = 0; k < kSpkVals; ++k)
            {
                const float d = want[k] - spkCur_[k];
                if (std::fabs(d) > 1.0e-4f * (std::fabs(want[k]) + 1.0e-3f)) { spkCur_[k] += slowCoef_ * d; spkMoving = true; }
                else spkCur_[k] = want[k];
            }
        }
        if (spkMoving || std::fabs(sp - spkLast_) > 5.0e-4f)
        {
            spkLast_ = sp;
            const float hp = 20.0f * std::pow(clamp(spkCur_[kSpkHp] * 0.05f, 1.0f, 100.0f), sp);
            spkPkL_.set(clamp(spkCur_[kSpkPkHz], 40.0f, 6000.0f), clamp(spkCur_[kSpkPkRes], 0.0f, 0.9f));
            spkPkR_.set(clamp(spkCur_[kSpkPkHz], 40.0f, 6000.0f), clamp(spkCur_[kSpkPkRes], 0.0f, 0.9f));
            const float lp = 20000.0f * std::pow(clamp(spkCur_[kSpkLp] * 5.0e-5f, 0.02f, 1.0f), sp);
            const float hpRes = clamp(0.15f + (spkCur_[kSpkHpRes] - 0.15f) * sp, 0.0f, 0.8f);
            const float lpRes = clamp(0.10f + (spkCur_[kSpkLpRes] - 0.10f) * sp, 0.0f, 0.8f);
            spkHpL_.set(hp, hpRes); spkHpR_.set(hp, hpRes);
            spkLpL_.set(lp, lpRes); spkLpR_.set(lp, lpRes);
        }
        spkMix_ = sp * 6.0f > 1.0f ? 1.0f : sp * 6.0f;
        spkGain_ = 1.0f + (spkCur_[kSpkGain] - 1.0f) * sp;
        // the body resonance (a band-pass added back in, level at its centre = 1 + amount), how far the
        // two sides fold to one, and the cone's grit: a gentle tanh with the level held
        spkPk_   = clamp(spkCur_[kSpkPkAmt], 0.0f, 2.0f) * sp * (2.0f - 1.98f * clamp(spkCur_[kSpkPkRes], 0.0f, 0.9f));
        spkMono_ = clamp(spkCur_[kSpkMono] * sp, 0.0f, 1.0f);
        spkDrv_  = 1.0f + 3.0f * clamp(spkCur_[kSpkGrit], 0.0f, 1.0f) * sp;
        spkComp_ = 1.0f / (1.0f + 0.6f * (spkDrv_ - 1.0f));

        if (std::fabs(echoToneT_ - echoToneLast_) > 1.0f)
        {
            echoToneLast_ += 0.2f * (echoToneT_ - echoToneLast_);
            if (echoToneLast_ < 300.0f || std::fabs(echoToneT_ - echoToneLast_) <= 1.0f) echoToneLast_ = echoToneT_;
            echoLpL_.setCutoff(echoToneLast_, cx_.sr);
            echoLpR_.setCutoff(echoToneLast_, cx_.sr);
        }
    }

    // The four utility signals. Their little programmes run all the time; they cost nothing unheard.
    void tickAliens(float tickSec)
    {
        float win[kNumAliens];
        for (int a = 0; a < kNumAliens; ++a)
            win[a] = (1.0f - smooth01(0.15f, 0.85f, std::fabs(tunePos_ - kAlienMHz[a]))) * squeal_ * (1.0f - bandMix_);

        // 89.4 teletype: two tones swapped at random
        if (--ttyTicks_ <= 0)
        {
            ttyTicks_ = (int)(cx_.tickRate * 0.022f) + 1;
            const uint32_t b = airRng_.next();
            if ((b & 31u) == 0u) ttyIdle_ = 6 + (int)((b >> 8) & 15u);
            if (ttyIdle_ > 0) { --ttyIdle_; alien_[0].hzT = 1445.0f; }
            else alien_[0].hzT = (b & 64u) ? 1445.0f : 1275.0f;
        }
        alien_[0].ampT = 0.2f * win[0];

        // 95.4 swooper: up and down over two seconds, shivering
        swPh_ += tickSec / 2.2f; if (swPh_ >= 1.0f) swPh_ -= 1.0f;
        if (win[1] > 0.0f)
        {
            const float tri = 1.0f - std::fabs(2.0f * swPh_ - 1.0f);
            alien_[1].hzT = 260.0f * std::pow(2.0f, 3.3f * tri * tri);
            alien_[1].ampT = 0.22f * win[1] * (0.6f + 0.4f * sinT(swPh_ * 23.0f));
        }
        else alien_[1].ampT = 0.0f;

        // 101.4 beeper: dots, dashes and gaps
        if (--morseTicks_ <= 0)
        {
            morseKey_ = !morseKey_;
            const uint32_t b = airRng_.next() % 10u;
            const float unit = 0.055f;
            const float len = morseKey_ ? (b < 6u ? unit : 3.0f * unit) : (b < 6u ? unit : (b < 9u ? 3.0f * unit : 7.0f * unit));
            morseTicks_ = (int)(cx_.tickRate * len) + 1;
        }
        alien_[2].hzT = 780.0f;
        alien_[2].ampT = morseKey_ ? 0.2f * win[2] : 0.0f;

        // 107.4 pulsar: a tone that steps about a strange scale
        if (--pulsarTicks_ <= 0)
        {
            static const float ratios[9] = {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f};
            const uint32_t b = airRng_.next();
            pulsarTicks_ = (int)(cx_.tickRate * (0.09f + 0.045f * (float)(b & 3u))) + 1;
            alien_[3].hzT = 350.0f * ratios[(b >> 4) % 9u];
            pulsarOn_ = ((b >> 12) % 5u) != 0u;
        }
        alien_[3].ampT = pulsarOn_ ? 0.2f * win[3] : 0.0f;
    }

    // Up to 16 samples of sound. Inputs and outputs may be the same memory: the input is read first.
    void renderChunk(const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        float dryL[kCtl], dryR[kCtl], stL[kCtl], stR[kCtl], gh[kCtl], bL[kCtl], bR[kCtl];
        for (int j = 0; j < n; ++j)
        {
            dryL[j] = inL != nullptr ? inL[j] : 0.0f;
            dryR[j] = inR != nullptr ? inR[j] : 0.0f;
            if (!std::isfinite(dryL[j])) dryL[j] = 0.0f;
            if (!std::isfinite(dryR[j])) dryR[j] = 0.0f;
            stL[j] = stR[j] = gh[j] = 0.0f;
        }

        // ---- the stations that can be heard ----
        for (int s = 0; s < kNumStations; ++s)
        {
            Slot& sl = slot_[s];
            if (!sl.active) continue;
            if (s == 0) { for (int j = 0; j < n; ++j) { bL[j] = dryL[j]; bR[j] = dryR[j]; } }
            else
            {
                for (int j = 0; j < n; ++j) { bL[j] = 0.0f; bR[j] = 0.0f; }
                switch (s)
                {
                    case 1: keys_.render(bL, bR, n); break;
                    case 2: breaks_.render(bL, bR, n); break;
                    case 3: talk_.render(bL, bR, n); break;
                    case 4: pulse_.render(bL, bR, n); break;
                    case 5: drift_.render(bL, bR, n); break;
                    default: bells_.render(bL, bR, n); break;
                }
            }
            for (int j = 0; j < n; ++j)
            {
                const float c = sl.clean.next();
                const float g = sl.ghost.next();
                stL[j] += bL[j] * c;
                stR[j] += bR[j] * c;
                gh[j] += (bL[j] + bR[j]) * 0.5f * g;
            }
        }

        const float invSr = cx_.invSr;
        for (int j = 0; j < n; ++j)
        {
            if (netOn_)
            {
                // the live station: clean in stereo, and its fringe on the ghost bus like any other
                const float c = netClean_.next() * kLiveGain;
                const float g = netGhost_.next() * kLiveGain;
                const int k = netAt_ + j;
                const float a = k < netHave_ ? netL_[(size_t)k] : 0.0f;
                const float b = k < netHave_ ? netR_[(size_t)k] : 0.0f;
                stL[j] += a * c;
                stR[j] += b * c;
                gh[j] += (a + b) * 0.5f * g;
            }
            float xL = stL[j], xR = stR[j];

            // ---- ghosts: thin and bent ----
            if (ghostOn_)
            {
                const float g = fastTanh(ghostHp_.process(ghostLp_.process(gh[j])) * 3.0f) * 0.45f;
                xL += g; xR += g;
            }

            // ---- air ----
            const float hissGain = hiss_.next();
            if (noiseOn_)
            {
                const float wc = airA_.nextBipolar(), wl = airB_.nextBipolar(), wr = airC_.nextBipolar();
                const float hl = hissHpL_.process(hissLpL_.process(wc * 0.7f + wl * 0.5f));
                const float hr = hissHpR_.process(hissLpR_.process(wc * 0.7f + wr * 0.5f));
                if (airA_.nextFloat() < crackleProb_)
                {
                    const float a = airB_.nextFloat();
                    crk_ += (0.1f + 1.5f * a * a * a) * ((airC_.next() & 1u) ? 1.0f : -1.0f);
                    crkPan_ = 0.3f + 0.4f * airA_.nextFloat();
                }
                crk_ *= crkDecay_;
                const float ck = crkLp_.process(crk_) * crackleLevel_ * 0.35f;
                xL += hl * hissGain + ck * (1.0f - crkPan_) * 2.0f;
                xR += hr * hissGain + ck * crkPan_ * 2.0f;
            }
            if (numLive_ > 0)
            {
                float t = 0.0f;
                for (int k = 0; k < numLive_; ++k) t += live_[k]->next(invSr);
                xL += t; xR += t;
            }

            // ---- Wow ----
            const float wowAmt = wow_.process();
            const float wowDelay = wowMod_.next();
            wowL_.write(xL); wowR_.write(xR);
            if (wowAmt > 1.0e-4f)
            {
                const float dly = 1.0f + wowAmt * wowDelay;
                xL = wowL_.readLinear(dly);
                xR = wowR_.readLinear(dly);
            }

            // ---- Drive ----
            const float dr = drive_.process();
            if (dr > 1.0e-4f)
            {
                xL += (fastTanh(xL * driveG_) * driveComp_ - xL) * driveMix_;
                xR += (fastTanh(xR * driveG_) * driveComp_ - xR) * driveMix_;
            }

            // ---- Speaker ----
            const float sp = spk_.process();
            if (sp > 1.0e-4f)
            {
                // low cut -> cone grit -> high cut (which also takes the grit's harsh top off) -> body resonance
                float hL = spkHpL_.process(xL, SVF::Mode::Highpass);
                float hR = spkHpR_.process(xR, SVF::Mode::Highpass);
                if (spkDrv_ > 1.001f)
                {
                    hL = fastTanh(hL * spkDrv_) * spkComp_;
                    hR = fastTanh(hR * spkDrv_) * spkComp_;
                }
                float fL = spkLpL_.process(hL, SVF::Mode::Lowpass);
                float fR = spkLpR_.process(hR, SVF::Mode::Lowpass);
                fL = (fL + spkPk_ * spkPkL_.process(fL, SVF::Mode::Bandpass)) * spkGain_;
                fR = (fR + spkPk_ * spkPkR_.process(fR, SVF::Mode::Bandpass)) * spkGain_;
                const float spkMid = 0.5f * (fL + fR);
                fL += (spkMid - fL) * spkMono_;
                fR += (spkMid - fR) * spkMono_;
                
                xL += (fL - xL) * spkMix_;
                xR += (fR - xR) * spkMix_;
            }

            // ---- Echo ----
            {
                const float send = echoSend_.process();
                const float fb = echoFb_.process();
                const float dt = echoTime_.process();
                float eL = echoL_.readLinear(dt), eR = echoR_.readLinear(dt);
                eL = softClip(echoHpL_.process(echoLpL_.process(eL)));
                eR = softClip(echoHpR_.process(echoLpR_.process(eR)));
                echoL_.write(xL * send + (eL * 0.7f + eR * 0.3f) * fb);
                echoR_.write(xR * send + (eR * 0.7f + eL * 0.3f) * fb);
                const float keep = 1.0f - 0.2f * send;
                xL = xL * keep + eL;
                xR = xR * keep + eR;
            }

            // ---- Room ----
            {
                const float rm = roomAmt_.process();
                float rL, rR;
                room_.process((xL + xR) * 0.5f * rm, rL, rR);
                const float keep = 1.0f - 0.2f * rm;
                xL = xL * keep + rL * 0.5f;
                xR = xR * keep + rR * 0.5f;
            }

            // ---- Mix, Volume, safety ----
            const float mx = mix_.process();
            const float vol = vol_.process();
            float oL = safeClip((dryL[j] + (xL - dryL[j]) * mx) * vol);
            float oR = safeClip((dryR[j] + (xR - dryR[j]) * mx) * vol);
            if (!std::isfinite(oL)) oL = 0.0f;
            if (!std::isfinite(oR)) oR = 0.0f;
            outL[j] = oL;
            if (outR != nullptr) outR[j] = oR;
        }
    }

    // ---- state ----
    Ctx cx_;
    Slot slot_[kNumStations];
    KeysStation   keys_;
    BreaksStation breaks_;
    TalkStation   talk_;
    PulseStation  pulse_;
    DriftStation  drift_;
    BellsStation  bells_;

    Tone  whistle_[kNumStations];
    Tone  alien_[kNumAliens];
    Tone* live_[kNumStations + kNumAliens] = {};
    int   numLive_ = 0;

    // parameter targets
    float volT_ = 1.0f, tuneT_ = 90.9f, static_ = 0.5f, squeal_ = 0.4f, ghosts_ = 0.35f;
    float mixT_ = 1.0f, spkT_ = 0.0f, driveT_ = 0.0f, wowT_ = 0.0f, roomT_ = 0.0f;
    float echoT_ = 0.0f, echoTimeT_ = 16000.0f, echoFbT_ = 0.35f, echoToneT_ = 4000.0f;
    int   key_ = -1;
    bool  songTempo_ = true;

    Smoother vol_, mix_, wow_, drive_, spk_, echoSend_, echoTime_, echoFb_, roomAmt_;

    // tuner and air
    float tunePos_ = 90.9f, tuneCoef_ = 0.006f, slowCoef_ = 0.012f, whWob_ = 0.0f;
    Ramp  hiss_, wowMod_;
    bool  ghostOn_ = false, noiseOn_ = false;
    Xorshift32 airA_{0xA341316Cu}, airB_{0xC8013EA4u}, airC_{0xAD90777Du}, airRng_{0x7E95761Eu};
    OnePoleLP ghostLp_, hissLpL_, hissLpR_, crkLp_, echoLpL_, echoLpR_;
    OnePoleHP ghostHp_, hissHpL_, hissHpR_, echoHpL_, echoHpR_;
    float crk_ = 0.0f, crkDecay_ = 0.9f, crkPan_ = 0.5f, crackleLevel_ = 0.0f, crackleProb_ = 0.0f;
    float crkDens_ = 0.3f, crkDensT_ = 0.3f;
    int   crkTicks_ = 0;

    // utility signals
    int   ttyTicks_ = 0, ttyIdle_ = 0, morseTicks_ = 0, pulsarTicks_ = 0;
    bool  morseKey_ = false, pulsarOn_ = true;
    float swPh_ = 0.0f;

    // effects
    DelayLine wowL_, wowR_, echoL_, echoR_;
    float wowP1_ = 0.0f, wowP2_ = 0.3f, wowP3_ = 0.6f;
    float driveG_ = 1.0f, driveComp_ = 1.0f, driveMix_ = 0.0f;
    SVF   spkHpL_, spkHpR_, spkLpL_, spkLpR_, spkPkL_, spkPkR_;
    int   spkType_ = 0;
    float spkCur_[kSpkVals] = {380.0f, 0.40f, 3200.0f, 0.40f, 1.80f, 1000.0f, 0.50f, 0.0f, 0.0f, 0.0f};
    float spkPk_ = 0.0f, spkMono_ = 0.0f, spkDrv_ = 1.0f, spkComp_ = 1.0f;
    float spkLast_ = -1.0f, spkMix_ = 0.0f, spkGain_ = 1.0f, echoToneLast_ = -1.0f;
    Room  room_;

    // the live band
    mpforge::AudioStream stream_;
    std::vector<float>   netL_, netR_;
    Ramp  netClean_, netGhost_;
    int   streamWanted_ = (int)mpforge::AudioStream::kNoStream, netPending_ = (int)mpforge::AudioStream::kNoStream;
    int   netDwell_ = 0, netAt_ = 0, netHave_ = 0, lampsShown_ = 5, lampsHold_ = 0;
    float bandMix_ = 1.0f, netLevel_ = 0.0f, netFlut_ = 0.0f, liveCoef_ = 0.01f, blink_ = 0.0f;
    bool  liveT_ = true, netOn_ = false;

    // clocks
    double songBeat_ = 0.0;
    int    ctlLeft_ = 0;
    int    showHold_ = 0, showNext_ = 1000;   // telling the screen where the controls stand after a preset
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(MPCRadio)
