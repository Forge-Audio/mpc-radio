// MP Forge's audio stream: brings sound in from the internet for a plugin to use.
//
// SPDX-License-Identifier: MIT
//
// This file and mp_forge_stream.cpp are MP Forge's own. MP Forge writes them into the plugin afresh before every
// build, so a change made to either is lost. They are the only code in a plugin that may use the network.
//
// A stream fetches MP3 sound from one of the addresses the plugin lists in online.json, turns it into samples
// and hands them to the plugin, which does with them whatever it likes: plays them as a radio, runs them
// through its own effects, or chops them up. There is no way to hand a stream an address that is not listed.
// It sends an address one request and nothing else.
//
// All the waiting and decoding happen on a thread of the stream's own. The audio thread only ever takes
// sound that has already arrived, so a slow network gives silence and never a freeze.
//
// How to use it:
//
//     mpforge::AudioStream stream_;                      // a member of the plugin
//     stream_.start(sampleRate);                         // in prepare()
//     stream_.open(0);                                   // the first address in online.json. kNoStream closes it
//     stream_.read(left, right, ctx.numFrames);          // in process(): fills both with sound, or with silence
//
// open() only leaves a number for the stream's thread, so it is safe to call from process().
// read() gives the sound at the plugin's own sample rate, whatever rate it was sent at.

#pragma once

namespace mpforge {

class AudioStream
{
public:
    enum State
    {
        kClosed = 0,         // no address is open
        kOpening,            // reaching the address, or filling up before the sound starts
        kPlaying,            // sound is being read
        kNotFound,           // the address's name could not be looked up
        kNotReached,         // no connection could be made
        kRefused,            // the address answered, but not with a stream
        kLost,               // sound was coming and stopped. The stream tries again by itself
        kNotOnThisComputer   // a stream works on an MPC, a Mac and Linux. Elsewhere it gives silence
    };

    static const int kNoStream = -1;

    AudioStream();
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    // The addresses listed in online.json, in that order, each by the name given to it there.
    static int         streamCount();
    static const char* streamName(int stream);

    // Call from prepare(). The stream's thread is started the first time.
    void start(double sampleRate);

    // Which of the listed addresses to bring in. Safe from any thread.
    void open(int stream);
    int  opened() const;

    // For the audio thread: fills left and right with the next sound, and with silence where there is none.
    // Returns how many frames of sound it gave. `right` may be null when only one channel is wanted.
    int read(float* left, float* right, int numFrames);

    State state() const;

    // How full the store of sound waiting to be read is, from 0 to 1.
    float waiting() const;

private:
    struct Inner;
    Inner* inner_;
};

} // namespace mpforge
