// MP Forge's audio stream: the part that goes online. How to use it is in mp_forge_stream.h.
//
// SPDX-License-Identifier: MIT
//
// This file is MP Forge's own. MP Forge writes it into the plugin afresh before every build, so a change made
// here is lost. Together with the files in mp-forge-online/ it is the only code in a plugin that may use the
// network or keep a thread.
//
// The stream's thread looks an address up, connects, asks for its sound once, and turns what arrives into
// samples with minimp3 (CC0). They wait in memory until the audio thread reads them. The thread never waits
// longer than a tenth of a second without looking whether the plugin is leaving or has opened another address.

#include "mp_forge_stream.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mp_forge_online {

struct Stream
{
    const char* name;
    const char* host;
    int         port;
    const char* path;
};

// Written by MP Forge from the plugin's online.json: kStreams and kStreamCount.
#include "mp-forge-online/streams.inc"

inline bool isAStream(int stream) { return stream >= 0 && stream < kStreamCount; }

} // namespace mp_forge_online

namespace mpforge {

int AudioStream::streamCount() { return mp_forge_online::kStreamCount; }

const char* AudioStream::streamName(int stream)
{
    return mp_forge_online::isAStream(stream) ? mp_forge_online::kStreams[stream].name : "";
}

} // namespace mpforge

#if defined(_WIN32)

// On Windows a stream gives silence: a plugin that uses one is built and checked there, and works on the MPC.
namespace mpforge {

struct AudioStream::Inner {};

AudioStream::AudioStream() : inner_(nullptr) {}
AudioStream::~AudioStream() {}
void               AudioStream::start(double) {}
void               AudioStream::open(int) {}
int                AudioStream::opened() const { return kNoStream; }
AudioStream::State AudioStream::state() const { return kNotOnThisComputer; }
float              AudioStream::waiting() const { return 0.0f; }

int AudioStream::read(float* left, float* right, int numFrames)
{
    std::fill(left, left + numFrames, 0.0f);
    if (right != nullptr)
        std::fill(right, right + numFrames, 0.0f);
    return 0;
}

} // namespace mpforge

#else

#include "mp-forge-online/small_sockets.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_SIMD
#include "mp-forge-online/minimp3.h"

#include <chrono>
#include <thread>

namespace mpforge {

namespace net = mp_forge_net;
using mp_forge_online::Stream;

namespace {

const size_t kFramesKept           = 6 * 48000;   // about six seconds
const double kSecondsBeforeReading = 1.5;         // so a short hitch on the network is not heard
const int    kSilentForTooLongMs   = 5000;
const int    kSecondsBeforeRetry   = 5;
const int    kLookEveryMs          = 100;
const int    kHereItIs             = 200;
const int    kLongestAnswer        = 4096;
const int    kMostHeld             = 16 * 1024;   // room for several pieces of sound not yet decoded
const int    kKeptBack             = 4096;        // more than two of the largest pieces an address sends
const int    kUsualRate            = 44100;
// The safety check and the build run a plugin on the computer without any wish to hear it. They say so.
const char*  kStaysOffline         = "MP_FORGE_STAYS_OFFLINE";

// Stereo sound that has arrived and waits to be read. One thread adds to it and one other takes from it,
// and neither ever waits for the other.
class SoundWaiting
{
public:
    explicit SoundWaiting(size_t frames) : frames_(frames), samples_(frames * 2, 0.0f) {}

    size_t waiting() const { return added_.load(std::memory_order_acquire) - taken_.load(std::memory_order_acquire); }
    size_t room() const { return frames_ - waiting(); }
    size_t holds() const { return frames_; }

    // For the thread that receives. The caller has made sure there is room.
    void add(const float* sound, size_t frames, int channels)
    {
        const size_t added = added_.load(std::memory_order_relaxed);
        for (size_t frame = 0; frame < frames; ++frame)
        {
            const size_t at = ((added + frame) % frames_) * 2;
            samples_[at]     = sound[frame * (size_t)channels];
            samples_[at + 1] = sound[frame * (size_t)channels + (size_t)(channels - 1)];
        }
        added_.store(added + frames, std::memory_order_release);
    }

    // For the audio thread: one sample of the sound that is waiting, counted from the next frame to be read.
    float sample(size_t framesAhead, int channel) const
    {
        const size_t taken = taken_.load(std::memory_order_relaxed);
        return samples_[((taken + framesAhead) % frames_) * 2 + (size_t)channel];
    }

    // For the audio thread: lets go of sound that has been read.
    void drop(size_t frames) { taken_.store(taken_.load(std::memory_order_relaxed) + frames, std::memory_order_release); }

private:
    const size_t        frames_;
    std::vector<float>  samples_;
    std::atomic<size_t> added_ { 0 };
    std::atomic<size_t> taken_ { 0 };
};

} // namespace

struct AudioStream::Inner
{
    SoundWaiting      sound { kFramesKept };
    std::thread       listening;
    std::atomic<bool> leaving { false };    // the plugin is going away
    std::atomic<bool> hurry { false };      // stop waiting on the network: leaving, or another address was opened
    std::atomic<int>  wanted { kNoStream };
    std::atomic<int>  reached { kClosed };
    std::atomic<int>  sentAtRate { kUsualRate };
    std::atomic<int>  openings { 0 };       // goes up each time the thread turns to an address
    std::atomic<bool> playing { false };

    // Only the audio thread touches these.
    double hostRate     = kUsualRate;
    double position     = 0.0;              // how far into the waiting sound reading has got, in frames
    int    openingsSeen = 0;

    // ---------------------------------------------------------------------------------------
    // The audio thread: reads what is waiting, and never waits itself
    // ---------------------------------------------------------------------------------------

    // Sound left from the address before would be read ahead of the new one's, so it is let go.
    void forgetTheAddressBefore()
    {
        const int now = openings.load(std::memory_order_relaxed);
        if (now == openingsSeen)
            return;
        openingsSeen = now;
        sound.drop(sound.waiting());
        position = 0.0;
        playing.store(false, std::memory_order_relaxed);
    }

    // Reading starts once enough sound is waiting, and stops to wait for more when it runs out. Sound that
    // was not sent at the plugin's own rate is given at the right speed by reading between its samples.
    int read(float* left, float* right, int numFrames)
    {
        forgetTheAddressBefore();
        const size_t have     = sound.waiting();
        const double rate     = (double)sentAtRate.load(std::memory_order_relaxed);
        const bool   isEnough = (double)have >= kSecondsBeforeReading * rate;
        const bool   reads    = playing.load(std::memory_order_relaxed) || isEnough;
        const int    given    = reads ? readInto(left, right, numFrames, have, rate / hostRate) : 0;
        silenceAfter(given, left, right, numFrames);
        letGoOfWhatWasRead(have);
        playing.store(reads && given == numFrames, std::memory_order_relaxed);
        return given;
    }

    // Returns how many frames could be given before the waiting sound ran out.
    int readInto(float* left, float* right, int numFrames, size_t have, double step)
    {
        int frame = 0;
        for (; frame < numFrames; ++frame, position += step)
        {
            const size_t whole = (size_t)position;
            if (whole + 1 >= have)
                break;
            const float towardsNext = (float)(position - (double)whole);
            left[frame] = sound.sample(whole, 0) * (1.0f - towardsNext) + sound.sample(whole + 1, 0) * towardsNext;
            if (right != nullptr)
                right[frame] = sound.sample(whole, 1) * (1.0f - towardsNext) + sound.sample(whole + 1, 1) * towardsNext;
        }
        return frame;
    }

    static void silenceAfter(int given, float* left, float* right, int numFrames)
    {
        std::fill(left + given, left + numFrames, 0.0f);
        if (right != nullptr)
            std::fill(right + given, right + numFrames, 0.0f);
    }

    void letGoOfWhatWasRead(size_t have)
    {
        const size_t read = std::min((size_t)position, have);
        sound.drop(read);
        position -= (double)read;
    }

    // ---------------------------------------------------------------------------------------
    // The stream's own thread
    // ---------------------------------------------------------------------------------------

    bool shouldHurry() const { return hurry.load(std::memory_order_relaxed); }

    void keepListening()
    {
        while (!leaving.load())
        {
            hurry.store(leaving.load());
            const int stream = wanted.load();
            if (!mp_forge_online::isAStream(stream))
            {
                reached.store(kClosed);
                waitMs(kLookEveryMs);
                continue;
            }
            openings.fetch_add(1);
            reached.store(kOpening);
            const State ended = listenUntilItIsLost(mp_forge_online::kStreams[stream]);
            if (wanted.load() == stream)
            {
                reached.store(ended);
                waitMs(kSecondsBeforeRetry * 1000);
            }
        }
    }

    void waitMs(int longest) const
    {
        for (int waited = 0; waited < longest && !shouldHurry(); waited += kLookEveryMs)
            std::this_thread::sleep_for(std::chrono::milliseconds(kLookEveryMs));
    }

    // Returns how it ended: with the address not found, not reached, refusing, or lost after sound had come.
    // A name that leads to this computer or to the musician's own network is treated as not found.
    State listenUntilItIsLost(const Stream& listed)
    {
        in_addr address {};
        if (!net::addressOf(listed.host, hurry, address) || net::isOnALocalNetwork(address))
            return kNotFound;
        const int connection = net::connectedTo(address, listed.port, hurry);
        if (connection == net::kNoSocket)
            return kNotReached;
        const State ended = saysHereItIs(connection, listed) ? receiveFrom(connection) : kRefused;
        close(connection);
        return ended;
    }

    // Asks for the sound and reads the answer up to the empty line that ends it, one byte at a time so that
    // none of the sound after it is swallowed.
    bool saysHereItIs(int connection, const Stream& listed) const
    {
        char asking[1024];
        char answer[kLongestAnswer] = {};
        const int length = std::snprintf(asking, sizeof(asking), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: mp-forge-stream\r\nConnection: close\r\n\r\n", listed.path, listed.host);
        if (length <= 0 || length >= (int)sizeof(asking) || !net::sent(connection, asking, (size_t)length))
            return false;
        for (int read = 0; read < kLongestAnswer - 1 && std::strstr(answer, "\r\n\r\n") == nullptr; ++read)
            if (!net::ready(connection, POLLIN, hurry) || recv(connection, answer + read, 1, 0) != 1)
                return false;
        return net::codeIn(answer) == kHereItIs;
    }

    // Takes in what the address sends and turns it into sound for as long as it keeps coming.
    State receiveFrom(int connection)
    {
        mp3dec_t      decoder;
        unsigned char arrived[kMostHeld];
        int           held = 0;
        mp3dec_init(&decoder);
        while (net::ready(connection, POLLIN, hurry, kSilentForTooLongMs))
        {
            const ssize_t more = recv(connection, arrived + held, (size_t)(kMostHeld - held), 0);
            if (more <= 0)
                break;
            held = soundOutOf(decoder, arrived, held + (int)more);
        }
        return kLost;
    }

    // Decodes the pieces of sound in what has arrived, and returns how many bytes are left over for the next
    // time. The decoder is only sure of a piece when it can see the start of the one after it. Given the very
    // end of what has arrived, it takes a piece that is cut off for rubbish, throws it away and starts afresh.
    // So the last few thousand bytes are always kept back until more has come in behind them.
    int soundOutOf(mp3dec_t& decoder, unsigned char* arrived, int held)
    {
        mp3d_sample_t       decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t about {};
        int                 used = 0;
        while (held - used > kKeptBack && !shouldHurry())
        {
            const int frames = mp3dec_decode_frame(&decoder, arrived + used, held - used, decoded, &about);
            if (about.frame_bytes == 0)
                break;
            used += about.frame_bytes;
            if (frames > 0)
                keepWaiting(decoded, (size_t)frames, about);
        }
        std::memmove(arrived, arrived + used, (size_t)(held - used));
        return held - used;
    }

    // Leaves the sound waiting for the audio thread. When the store is full the thread holds back, and the
    // address is then simply read more slowly.
    void keepWaiting(const mp3d_sample_t* decoded, size_t frames, const mp3dec_frame_info_t& about)
    {
        sentAtRate.store(about.hz);
        while (sound.room() < frames && !shouldHurry())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!shouldHurry())
            sound.add(decoded, frames, about.channels);
    }
};

AudioStream::AudioStream() : inner_(new Inner()) {}

AudioStream::~AudioStream()
{
    inner_->leaving.store(true);
    inner_->hurry.store(true);
    if (inner_->listening.joinable())
        inner_->listening.join();
    delete inner_;
}

void AudioStream::start(double sampleRate)
{
    inner_->hostRate = sampleRate > 0.0 ? sampleRate : (double)kUsualRate;
    const bool mayGoOnline = std::getenv(kStaysOffline) == nullptr;
    if (mayGoOnline && !inner_->listening.joinable())
        inner_->listening = std::thread([this] { inner_->keepListening(); });
}

void AudioStream::open(int stream)
{
    if (inner_->wanted.exchange(stream) != stream)
        inner_->hurry.store(true);
}

int AudioStream::opened() const { return inner_->wanted.load(std::memory_order_relaxed); }

int AudioStream::read(float* left, float* right, int numFrames) { return inner_->read(left, right, numFrames); }

AudioStream::State AudioStream::state() const
{
    const int reached = inner_->reached.load(std::memory_order_relaxed);
    return reached == kOpening && inner_->playing.load(std::memory_order_relaxed) ? kPlaying : (State)reached;
}

float AudioStream::waiting() const { return (float)inner_->sound.waiting() / (float)inner_->sound.holds(); }

} // namespace mpforge

#endif
