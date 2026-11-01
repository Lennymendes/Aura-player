// NX Player — MIT License
#include <algorithm>
#include "DsdDecoders.h"
#include "Dst.h"
#include <limits>
#include <cstring>

namespace nx
{

//==============================================================================
std::unique_ptr<AudioDecoder> DsfDecoder::open (std::unique_ptr<juce::InputStream>& s, juce::String& error)
{
    uint8_t h[28];
    s->setPosition (0);
    if (! bytes::readExactly (*s, h, 28) || std::memcmp (h, "DSD ", 4) != 0)
        return {};

    std::unique_ptr<DsfDecoder> d (new DsfDecoder());
    d->metadataOffset = (juce::int64) bytes::le64 (h + 20);

    uint8_t f[52];
    s->setPosition ((juce::int64) bytes::le64 (h + 4));
    if (! bytes::readExactly (*s, f, 52) || std::memcmp (f, "fmt ", 4) != 0)
    {
        error = "DSF : chunk fmt manquant";
        return {};
    }
    const auto fmtSize = bytes::le64 (f + 4);
    const auto formatId = bytes::le32 (f + 16);
    const int channels = (int) bytes::le32 (f + 24);
    const auto rate = bytes::le32 (f + 28);
    const auto bits = bytes::le32 (f + 32);
    const auto sampleCount = bytes::le64 (f + 36);
    const auto blockSize = bytes::le32 (f + 44);

    if (formatId != 0 || channels < 1 || channels > 6 || (bits != 1 && bits != 8)
        || rate % 44100 != 0 || rate < 2822400 || blockSize == 0 || blockSize > (1u << 20))
    {
        error = "DSF : format non pris en charge";
        return {};
    }

    uint8_t dh[12];
    const auto dataChunkPos = (juce::int64) bytes::le64 (h + 4) + (juce::int64) fmtSize;
    s->setPosition (dataChunkPos);
    if (! bytes::readExactly (*s, dh, 12) || std::memcmp (dh, "data", 4) != 0)
    {
        error = "DSF : chunk data manquant";
        return {};
    }

    d->format.kind = SampleKind::Dsd;
    d->format.sampleRate = (double) rate;
    d->format.bitsPerSample = 1;
    d->format.numChannels = channels;
    d->format.codec = "DSF";
    d->dataStart = dataChunkPos + 12;
    d->blockSize = (int) blockSize;
    d->lsbFirst = (bits == 1);
    d->lengthBytes = (juce::int64) (sampleCount / 8);

    // Never trust sampleCount beyond what the data chunk holds.
    const auto dataBytes = (juce::int64) bytes::le64 (dh + 4) - 12;
    d->lengthBytes = juce::jmin (d->lengthBytes, (dataBytes / (blockSize * (juce::int64) channels)) * blockSize);

    d->group.allocate ((size_t) (channels * (int) blockSize), true);
    d->stream = std::move (s);
    return d;
}

bool DsfDecoder::loadBlockGroup (juce::int64 groupIndex)
{
    if (groupIndex == loadedGroup) return true;
    const auto groupBytes = (juce::int64) blockSize * format.numChannels;
    if (! stream->setPosition (dataStart + groupIndex * groupBytes)) return false;
    if (stream->read (group.get(), (int) groupBytes) != (int) groupBytes) return false;
    if (lsbFirst)
        for (juce::int64 i = 0; i < groupBytes; ++i)
            group[i] = reverseBits (group[i]);
    loadedGroup = groupIndex;
    return true;
}

bool DsfDecoder::seek (juce::int64 bytePos)
{
    position = juce::jlimit<juce::int64> (0, lengthBytes, bytePos);
    return true;
}

int DsfDecoder::readDsd (uint8_t* dest, int maxBytes)
{
    int done = 0;
    const int ch = format.numChannels;
    while (done < maxBytes && position < lengthBytes)
    {
        const auto groupIndex = position / blockSize;
        if (! loadBlockGroup (groupIndex)) break;
        const int offset = (int) (position % blockSize);
        const int n = (int) std::min<juce::int64> ({ (juce::int64) (maxBytes - done), (juce::int64) (blockSize - offset), lengthBytes - position });
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c)
                dest[(size_t) (done + i) * ch + c] = group[c * blockSize + offset + i];
        done += n;
        position += n;
    }
    return done;
}

//==============================================================================
namespace
{
    /** DSDIFF with DST compression: "DST " chunk = "FRTE" (frame count) + one "DSTF" chunk per 1/75 s frame.
        Frames are found through the "DSTI" index when present, otherwise by walking the chunks (as far as needed). */
    class DffDstDecoder final : public AudioDecoder
    {
    public:
        DffDstDecoder (std::unique_ptr<juce::InputStream> s, StreamFormat f, juce::int64 dstBody, juce::int64 dstEnd,
                       juce::int64 frames, std::vector<std::pair<juce::int64, juce::uint32>> idx)
            : stream (std::move (s)), format (f), scanPos (dstBody), end (dstEnd), numFrames (frames), index (std::move (idx)),
              decoder (f.numChannels, (int) (f.sampleRate / 44100.0))
        {
            frameBytes = decoder.getBytesPerChannel();
            if (! index.empty()) scanPos = index.back().first + index.back().second + (index.back().second & 1);   // walk on after the index
            if (numFrames <= 0) { locate (std::numeric_limits<juce::int64>::max()); numFrames = (juce::int64) index.size(); }
            lengthBytes = numFrames * frameBytes;
            out.resize ((size_t) decoder.getOutputSize());
        }

        StreamFormat getFormat() const override  { return format; }
        juce::int64 getLength() const override    { return lengthBytes; }
        juce::int64 getPosition() const override  { return position; }
        bool seek (juce::int64 p) override        { position = juce::jlimit<juce::int64> (0, lengthBytes, p); return true; }

        int readDsd (uint8_t* dest, int maxBytes) override
        {
            const int ch = format.numChannels;
            int done = 0;
            while (done < maxBytes && position < lengthBytes)
            {
                const auto k = position / frameBytes;
                if (k != outFrame && ! load (k)) break;
                const int offset = (int) (position - k * frameBytes);
                const int n = juce::jmin (maxBytes - done, frameBytes - offset, (int) (lengthBytes - position));
                std::memcpy (dest + (size_t) done * (size_t) ch, out.data() + (size_t) offset * (size_t) ch, (size_t) n * (size_t) ch);
                done += n;
                position += n;
            }
            return done;
        }

    private:
        /** Walks the "DST " chunk until frame k is indexed (or the end). */
        bool locate (juce::int64 k)
        {
            while ((juce::int64) index.size() <= k && scanPos + 12 <= end)
            {
                uint8_t h[12];
                if (! stream->setPosition (scanPos) || ! bytes::readExactly (*stream, h, 12)) return false;
                const auto size = (juce::int64) bytes::be64 (h + 4);
                if (size < 0 || scanPos + 12 + size > end + 1) return false;
                if (std::memcmp (h, "DSTF", 4) == 0) index.emplace_back (scanPos + 12, (juce::uint32) size);
                scanPos += 12 + size + (size & 1);
            }
            return (juce::int64) index.size() > k;
        }

        bool load (juce::int64 k)
        {
            if (! locate (k)) return false;
            const auto [pos, size] = index[(size_t) k];
            frame.resize (size);
            if (size == 0 || ! stream->setPosition (pos) || ! bytes::readExactly (*stream, frame.data(), (int) size)) return false;
            juce::String err;
            if (! decoder.decode (frame.data(), frame.size(), out.data(), err)) lastError = err;   // silence for this frame
            outFrame = k;
            return true;
        }

        std::unique_ptr<juce::InputStream> stream;
        StreamFormat format;
        juce::int64 scanPos, end, numFrames, lengthBytes = 0, position = 0, outFrame = -1;
        std::vector<std::pair<juce::int64, juce::uint32>> index;
        dst::FrameDecoder decoder;
        int frameBytes = 4704;
        std::vector<uint8_t> frame, out;
    };
}

std::unique_ptr<AudioDecoder> DffDecoder::open (std::unique_ptr<juce::InputStream>& s, juce::String& error)
{
    uint8_t h[16];
    s->setPosition (0);
    if (! bytes::readExactly (*s, h, 16) || std::memcmp (h, "FRM8", 4) != 0 || std::memcmp (h + 12, "DSD ", 4) != 0)
        return {};

    std::unique_ptr<DffDecoder> d (new DffDecoder());
    d->format.kind = SampleKind::Dsd;
    d->format.bitsPerSample = 1;
    d->format.codec = "DFF";
    const auto formEnd = 12 + (juce::int64) bytes::be64 (h + 4);
    bool haveRate = false, haveData = false, isDst = false;
    juce::int64 dstBody = 0, dstEnd = 0, dstFrames = 0;
    std::vector<std::pair<juce::int64, juce::uint32>> dstIndex;

    juce::int64 pos = 16;
    while (pos + 12 <= formEnd)
    {
        uint8_t ch[12];
        s->setPosition (pos);
        if (! bytes::readExactly (*s, ch, 12)) break;
        const auto size = (juce::int64) bytes::be64 (ch + 4);
        const auto body = pos + 12;

        if (std::memcmp (ch, "PROP", 4) == 0)
        {
            uint8_t type[4];
            if (! bytes::readExactly (*s, type, 4) || std::memcmp (type, "SND ", 4) != 0) { error = "DFF : PROP invalide"; return {}; }
            juce::int64 p = body + 4;
            while (p + 12 <= body + size)
            {
                uint8_t sc[12];
                s->setPosition (p);
                if (! bytes::readExactly (*s, sc, 12)) break;
                const auto subSize = (juce::int64) bytes::be64 (sc + 4);
                uint8_t v[6] {};
                if (std::memcmp (sc, "FS  ", 4) == 0 && bytes::readExactly (*s, v, 4))
                {
                    d->format.sampleRate = (double) bytes::be32 (v);
                    haveRate = true;
                }
                else if (std::memcmp (sc, "CHNL", 4) == 0 && bytes::readExactly (*s, v, 2))
                {
                    d->format.numChannels = bytes::be16 (v);
                }
                else if (std::memcmp (sc, "CMPR", 4) == 0 && bytes::readExactly (*s, v, 4))
                {
                    isDst = std::memcmp (v, "DST ", 4) == 0;
                    if (! isDst && std::memcmp (v, "DSD ", 4) != 0) { error = "DFF : compression inconnue"_t; return {}; }
                }
                p += 12 + subSize + (subSize & 1);
            }
        }
        else if (std::memcmp (ch, "DSD ", 4) == 0)
        {
            d->dataStart = body;
            d->lengthBytes = size;
            haveData = true;
        }
        else if (std::memcmp (ch, "DST ", 4) == 0)
        {
            dstBody = body;
            dstEnd = body + size;
            haveData = true;
            uint8_t fr[12 + 6];
            if (bytes::readExactly (*s, fr, sizeof (fr)) && std::memcmp (fr, "FRTE", 4) == 0) dstFrames = (juce::int64) bytes::be32 (fr + 12);
        }
        else if (std::memcmp (ch, "DSTI", 4) == 0 && size >= 12 && size < ((juce::int64) 1 << 31))
        {
            // index of the frames: {u64 file offset of the frame data, u32 length}
            juce::MemoryBlock mb;
            if (s->readIntoMemoryBlock (mb, (ssize_t) size) == (size_t) size)
                for (size_t i = 0; i + 12 <= mb.getSize(); i += 12)
                {
                    const auto* e = static_cast<const uint8_t*> (mb.getData()) + i;
                    dstIndex.emplace_back ((juce::int64) bytes::be64 (e), bytes::be32 (e + 8));
                }
        }
        else if (std::memcmp (ch, "ID3 ", 4) == 0)
        {
            d->id3Offset = body;
            d->id3Size = size;
        }
        pos = body + size + (size & 1);
    }

    const int channels = d->format.numChannels;
    if (! haveRate || ! haveData || channels < 1 || channels > 6
        || std::fmod (d->format.sampleRate, 44100.0) != 0.0 || d->format.sampleRate < 2822400.0)
    {
        error = "DFF : en-tête incomplet ou format non pris en charge"_t;
        return {};
    }
    if (isDst)
    {
        if (dstBody == 0 || channels > dst::maxChannels) { error = "DFF DST : données absentes ou trop de canaux"_t; return {}; }
        // an index that does not fit the "DST " chunk is ignored (the chunks are walked instead)
        bool indexOk = ! dstIndex.empty();
        for (auto& [off, len] : dstIndex) indexOk = indexOk && off >= dstBody && off + (juce::int64) len <= dstEnd;
        if (! indexOk) dstIndex.clear();
        auto f = d->format;
        f.codec = "DFF DST";
        return std::make_unique<DffDstDecoder> (std::move (s), f, dstBody, dstEnd, dstFrames, std::move (dstIndex));
    }
    d->lengthBytes /= channels;
    d->stream = std::move (s);
    d->stream->setPosition (d->dataStart);
    return d;
}

bool DffDecoder::seek (juce::int64 bytePos)
{
    position = juce::jlimit<juce::int64> (0, lengthBytes, bytePos);
    return stream->setPosition (dataStart + position * format.numChannels);
}

int DffDecoder::readDsd (uint8_t* dest, int maxBytes)
{
    const auto n = (int) std::min<juce::int64> (maxBytes, lengthBytes - position);
    if (n <= 0) return 0;
    const int got = stream->read (dest, n * format.numChannels) / format.numChannels;
    position += juce::jmax (0, got);
    return juce::jmax (0, got);
}

} // namespace nx
