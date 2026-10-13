#include "SongPlayer.h"

// 16-bit storage with 6 dB of headroom: rebalanced stems can peak above full scale.
static constexpr float storeScale = 16383.0f;

SongPlayer::SongPlayer()
{
    currentGain.fill (1.0f);
}

juce::String SongPlayer::load (const juce::File& dir, const juce::StringArray& names)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    auto song = std::make_unique<SongData>();
    juce::StringArray loaded;
    for (auto& name : names)
    {
        if (loaded.size() >= maxStems) break;
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (dir.getChildFile (name + ".wav")));
        if (reader == nullptr)
            continue;

        const auto len = (int64_t) reader->lengthInSamples;
        if (song->stems.empty())
        {
            song->sampleRate = reader->sampleRate;
            song->length = len;
        }
        song->length = juce::jmin (song->length, len);

        // Read in chunks and store as interleaved 16-bit (halves memory vs float).
        std::vector<int16_t> pcm ((size_t) len * 2);
        juce::AudioBuffer<float> chunk (2, 65536);
        for (int64_t pos = 0; pos < len; pos += chunk.getNumSamples())
        {
            const int n = (int) juce::jmin<int64_t> (chunk.getNumSamples(), len - pos);
            reader->read (&chunk, 0, n, pos, true, true);
            auto* cl = chunk.getReadPointer (0);
            auto* cr = chunk.getReadPointer (reader->numChannels > 1 ? 1 : 0);
            for (int i = 0; i < n; ++i)
            {
                pcm[(size_t) (pos + i) * 2]     = (int16_t) juce::jlimit (-32767.0f, 32767.0f, cl[i] * storeScale);
                pcm[(size_t) (pos + i) * 2 + 1] = (int16_t) juce::jlimit (-32767.0f, 32767.0f, cr[i] * storeScale);
            }
        }
        song->stems.push_back (std::move (pcm));
        loaded.add (name);
    }
    if (song->stems.empty())
        return "No stems found in " + dir.getFullPathName();

    setPlaying (false);
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::swap (data, song); // old song is freed below, outside the lock
        position = 0;
        seekRequest = -1;
        sourceRate = data->sampleRate;
        lengthSeconds = (double) data->length / data->sampleRate;
        interpL.reset();
        interpR.reset();
    }
    stemNames = loaded;
    for (auto& c : controls)
    {
        c.volume = 1.0f;
        c.mute = false;
        c.solo = false;
    }
    return {};
}

void SongPlayer::unload()
{
    setPlaying (false);
    std::unique_ptr<SongData> old;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::swap (data, old);
        lengthSeconds = 0.0;
        position = 0;
    }
    stemNames.clear();
}

void SongPlayer::prepare (double rate)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    deviceRate = rate;
    interpL.reset();
    interpR.reset();
}

void SongPlayer::setPlaying (bool shouldPlay) noexcept
{
    if (shouldPlay && getPosition() >= getLength() - 0.05)
        seek (0.0); // restart from the top when pressing play at the end
    playing = shouldPlay;
}

void SongPlayer::seek (double seconds) noexcept
{
    seekRequest = (int64_t) (juce::jmax (0.0, seconds) * sourceRate.load());
}

double SongPlayer::getPosition() const noexcept
{
    const auto req = seekRequest.load();
    return (double) (req >= 0 ? req : position.load()) / sourceRate.load();
}

void SongPlayer::render (float* l, float* r, int n) noexcept
{
    juce::FloatVectorOperations::clear (l, n);
    juce::FloatVectorOperations::clear (r, n);

    const juce::SpinLock::ScopedTryLockType tl (lock);
    if (! tl.isLocked() || data == nullptr)
        return;

    if (const auto req = seekRequest.exchange (-1); req >= 0)
    {
        position = juce::jlimit<int64_t> (0, data->length, req);
        interpL.reset();
        interpR.reset();
    }
    if (! playing.load())
        return;

    const double ratio = data->sampleRate / deviceRate;
    const int64_t pos = position.load();
    const int need = juce::jmin (scratch.getNumSamples(), (int) std::ceil (n * ratio) + 4);
    const int avail = (int) juce::jmin<int64_t> (need, data->length - pos);
    if (avail <= 4)
    {
        playing = false; // end of song
        position = data->length;
        return;
    }

    bool anySolo = false;
    const int numStems = (int) data->stems.size();
    for (int s = 0; s < numStems; ++s)
        anySolo |= controls[(size_t) s].solo.load();

    auto* ml = scratch.getWritePointer (0);
    auto* mr = scratch.getWritePointer (1);
    juce::FloatVectorOperations::clear (ml, need);
    juce::FloatVectorOperations::clear (mr, need);

    constexpr float scale = 1.0f / storeScale;
    for (int s = 0; s < numStems; ++s)
    {
        auto& c = controls[(size_t) s];
        const bool audible = ! c.mute.load() && (! anySolo || c.solo.load());
        const float target = audible ? c.volume.load() : 0.0f;
        float g = currentGain[(size_t) s];
        const float step = (target - g) / (float) avail; // per-block ramp: no zipper noise
        const int16_t* src = data->stems[(size_t) s].data() + pos * 2;
        float pk = 0.0f;
        for (int i = 0; i < avail; ++i, g += step)
        {
            const float sl = src[i * 2] * scale, sr = src[i * 2 + 1] * scale;
            ml[i] += sl * g;
            mr[i] += sr * g;
            pk = juce::jmax (pk, std::abs (sl * g), std::abs (sr * g));
        }
        currentGain[(size_t) s] = target;
        c.peak = juce::jmax (pk, c.peak.load() * 0.9f);
    }

    const int used = interpL.process (ratio, ml, l, n);
    interpR.process (ratio, mr, r, n);
    position = pos + used;
}
