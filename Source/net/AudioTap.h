#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

namespace mlm
{
/**
    Lock-free single-producer / single-consumer stereo FIFO.

    The audio thread pushes a copy of the master bus (never blocks, never allocates);
    the streaming thread pops it. If the consumer falls behind, new audio is dropped
    (the DAW output is never affected).
*/
class AudioTap
{
public:
    static constexpr int capacityFrames = 1 << 19; // ~10.9 s at 48 kHz, ~2.7 s at 192 kHz

    AudioTap() : fifo (capacityFrames), storage ((size_t) capacityFrames * 2, 0.0f) {}

    void setSampleRate (double sr) noexcept { sampleRate.store ((int) sr); }
    int getSampleRate() const noexcept      { return sampleRate.load(); }

    /** Audio thread. 'right' may equal 'left' for mono. */
    void push (const float* left, const float* right, int numFrames) noexcept
    {
        if (! enabled.load (std::memory_order_relaxed))
            return;

        int s1, n1, s2, n2;
        fifo.prepareToWrite (numFrames, s1, n1, s2, n2);
        if (n1 + n2 < numFrames)
            dropped.fetch_add ((juce::uint32) (numFrames - n1 - n2), std::memory_order_relaxed);

        auto copy = [&] (int start, int count, int offset)
        {
            float* d = storage.data() + (size_t) start * 2;
            for (int i = 0; i < count; ++i)
            {
                d[2 * i]     = left[offset + i];
                d[2 * i + 1] = right[offset + i];
            }
        };
        copy (s1, n1, 0);
        copy (s2, n2, n1);
        fifo.finishedWrite (n1 + n2);
        written.fetch_add ((juce::uint64) (n1 + n2), std::memory_order_relaxed);
    }

    /** Streaming thread: pops up to maxFrames interleaved stereo frames. Returns frames read. */
    int pop (float* interleaved, int maxFrames) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (maxFrames, s1, n1, s2, n2);
        std::copy (storage.data() + (size_t) s1 * 2, storage.data() + (size_t) (s1 + n1) * 2, interleaved);
        std::copy (storage.data() + (size_t) s2 * 2, storage.data() + (size_t) (s2 + n2) * 2, interleaved + (size_t) n1 * 2);
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

    int getNumReady() const noexcept { return fifo.getNumReady(); }

    void setEnabled (bool on) noexcept { enabled.store (on); }
    bool isEnabled() const noexcept    { return enabled.load(); }

    juce::uint64 getFramesWritten() const noexcept { return written.load(); }
    juce::uint32 getFramesDropped() const noexcept { return dropped.load(); }

private:
    juce::AbstractFifo fifo;
    std::vector<float> storage;
    std::atomic<int> sampleRate { 48000 };
    std::atomic<bool> enabled { true };
    std::atomic<juce::uint64> written { 0 };
    std::atomic<juce::uint32> dropped { 0 };
};
}
