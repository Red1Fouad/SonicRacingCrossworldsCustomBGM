#pragma once
#include <windows.h>
#include <vector>
#include <cstring>
#include <algorithm>
#include <string>

#if __has_include("dr_mp3.h")
#include "dr_mp3.h"
#define ENABLE_MP3
#endif

#if __has_include("stb_vorbis.c")
#include "stb_vorbis.c"
#define ENABLE_OGG
#endif

#if __has_include("dr_flac.h")
#include "dr_flac.h"
#define ENABLE_FLAC
#endif

#if __has_include("aacdec.h")
#include "aacdec.h"
#define ENABLE_AAC
#endif

extern "C" {
#include "libvgmstream.h"
}

struct WavData {
    WAVEFORMATEX wfx;
    std::vector<BYTE> audioData;
    bool hasLoop = false;
    UINT32 loopBegin = 0;
    UINT32 loopLength = 0;
};

struct CompressedAudio {
    std::vector<uint8_t> data;
    std::string extension;
    std::string filename;
    float weight = 1.0f;
    float volumeMul = 1.0f;
    float durationSec = 0.0f;
};

struct MemStreamFile {
    const uint8_t* data;
    int64_t size;
    std::string name;
};

static int MemStreamFile_read(void* user_data, uint8_t* dst, int64_t offset, int length) {
    auto* f = (MemStreamFile*)user_data;
    if (offset < 0 || offset >= f->size) return 0;
    int avail = (int)(f->size - offset);
    if (length > avail) length = avail;
    memcpy(dst, f->data + offset, length);
    return length;
}

static int64_t MemStreamFile_get_size(void* user_data) {
    return ((MemStreamFile*)user_data)->size;
}

static const char* MemStreamFile_get_name(void* user_data) {
    return ((MemStreamFile*)user_data)->name.c_str();
}

static void MemStreamFile_close(libstreamfile_t* libsf) {
    if (libsf) {
        delete (MemStreamFile*)libsf->user_data;
        free(libsf);
    }
}

static libstreamfile_t* MemStreamFile_open(void* user_data, const char*) {
    auto* f = (MemStreamFile*)user_data;
    auto* mf = new MemStreamFile{f->data, f->size, f->name};
    auto* sf = (libstreamfile_t*)calloc(1, sizeof(libstreamfile_t));
    sf->user_data = mf;
    sf->read = MemStreamFile_read;
    sf->get_size = MemStreamFile_get_size;
    sf->get_name = MemStreamFile_get_name;
    sf->open = MemStreamFile_open;
    sf->close = MemStreamFile_close;
    return sf;
}

static libstreamfile_t* CreateMemStreamFile(const uint8_t* data, int64_t size, const std::string& name) {
    auto* mf = new MemStreamFile{data, size, name};
    auto* sf = (libstreamfile_t*)calloc(1, sizeof(libstreamfile_t));
    sf->user_data = mf;
    sf->read = MemStreamFile_read;
    sf->get_size = MemStreamFile_get_size;
    sf->get_name = MemStreamFile_get_name;
    sf->open = MemStreamFile_open;
    sf->close = MemStreamFile_close;
    return sf;
}

class AudioLoader {
public:
    static void NormalizePcm16(WavData& data) {
        if (data.audioData.empty() || data.wfx.wBitsPerSample != 16) return;
        int16_t* samples = reinterpret_cast<int16_t*>(data.audioData.data());
        size_t count = data.audioData.size() / 2;
        int16_t peak = 0;
        for (size_t i = 0; i < count; i++) {
            int16_t s = samples[i];
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        if (peak == 0) return;
        float gain = (0.90f * 32767.0f) / (float)peak;
        for (size_t i = 0; i < count; i++) {
            float scaled = samples[i] * gain;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            samples[i] = (int16_t)scaled;
        }
    }

    static void ScalePcm16(WavData& data, float mul) {
        if (data.audioData.empty() || data.wfx.wBitsPerSample != 16 || mul == 1.0f) return;
        int16_t* s = reinterpret_cast<int16_t*>(data.audioData.data());
        size_t cnt = data.audioData.size() / 2;
        for (size_t i = 0; i < cnt; i++) {
            float v = s[i] * mul;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            s[i] = (int16_t)v;
        }
    }

    static void TrimLeadingSilence(WavData& data, int16_t threshold = 1024) {
        if (data.audioData.empty() || data.wfx.wBitsPerSample != 16) return;
        int channels = data.wfx.nChannels;
        if (channels <= 0) return;
        int16_t* samples = reinterpret_cast<int16_t*>(data.audioData.data());
        size_t totalFrames = data.audioData.size() / (channels * sizeof(int16_t));
        size_t firstNonSilent = totalFrames;
        for (size_t f = 0; f < totalFrames; f++) {
            bool silent = true;
            for (int ch = 0; ch < channels; ch++) {
                int16_t s = samples[f * channels + ch];
                if (s < 0) s = -s;
                if (s > threshold) { silent = false; break; }
            }
            if (!silent) { firstNonSilent = f; break; }
        }
        if (firstNonSilent == 0) return;
        if (firstNonSilent >= totalFrames) { data.audioData.clear(); return; }
        size_t bytesPerFrame = channels * sizeof(int16_t);
        size_t remaining = totalFrames - firstNonSilent;
        memmove(data.audioData.data(), data.audioData.data() + firstNonSilent * bytesPerFrame, remaining * bytesPerFrame);
        data.audioData.resize(remaining * bytesPerFrame);
    }

    static bool LoadCompressed(const std::string& filename, CompressedAudio& out) {
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;
        out.data = std::move(fileData);
        out.filename = filename;
        size_t dot = filename.find_last_of(".");
        if (dot != std::string::npos) {
            out.extension = filename.substr(dot);
            for (auto& c : out.extension) c = (char)tolower(c);
        }
        return true;
    }

    static bool DecodeToPcm(const CompressedAudio& input, WavData& outData) {
        const std::string& ext = input.extension;
        if (ext == ".wav" || ext == ".adx" || ext == ".aax" ||
            ext == ".brstm" || ext == ".bcstm" ||
            ext == ".bwav" || ext == ".bcwav")
            return DecodeVgmstreamMemory(input.data, input.filename, outData);
        if (ext == ".mp3") return DecodeMp3Memory(input.data, outData);
        if (ext == ".ogg") return DecodeOggMemory(input.data, outData);
        if (ext == ".flac") return DecodeFlacMemory(input.data, outData);
        if (ext == ".aac" || ext == ".m4a") return DecodeAacMemory(input.data, outData);
        return false;
    }

    static float EstimateDuration(const CompressedAudio& input) {
        const std::string& ext = input.extension;
        const auto& d = input.data;
        if (ext == ".wav" && d.size() >= 44) {
            if (memcmp(d.data(), "RIFF", 4) != 0) return 0.0f;
            size_t pos = 12;
            uint32_t byteRate = 0, dataSize = 0;
            while (pos + 8 <= d.size()) {
                uint32_t cs; memcpy(&cs, d.data() + pos + 4, 4);
                if (memcmp(d.data() + pos, "fmt ", 4) == 0 && pos + 24 <= d.size())
                    memcpy(&byteRate, d.data() + pos + 8 + 12, 4);
                else if (memcmp(d.data() + pos, "data", 4) == 0)
                    dataSize = cs;
                pos += 8 + cs;
                if (cs % 2 != 0) pos++;
            }
            if (byteRate > 0 && dataSize > 0) return (float)dataSize / (float)byteRate;
        }
        if (ext == ".mp3") {
#ifdef ENABLE_MP3
            drmp3 mp3;
            if (drmp3_init_memory(&mp3, d.data(), d.size(), NULL)) {
                float dur = (float)drmp3_get_pcm_frame_count(&mp3) / (float)mp3.sampleRate;
                drmp3_uninit(&mp3);
                return dur;
            }
#endif
        }
        if (ext == ".ogg") {
#ifdef ENABLE_OGG
            int err; stb_vorbis* v = stb_vorbis_open_memory(d.data(), (int)d.size(), &err, NULL);
            if (v) {
                stb_vorbis_info inf = stb_vorbis_get_info(v);
                int samps = stb_vorbis_stream_length_in_samples(v);
                stb_vorbis_close(v);
                if (inf.sample_rate > 0) return (float)samps / (float)inf.sample_rate;
            }
#endif
        }
        if (ext == ".flac") {
#ifdef ENABLE_FLAC
            drflac* f = drflac_open_memory(d.data(), d.size(), NULL);
            if (f) { float dur = (float)f->totalPCMFrameCount / (float)f->sampleRate; drflac_close(f); return dur; }
#endif
        }
        if (ext == ".adx" && d.size() >= 16) {
            uint32_t sr = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
            uint32_t ts = ((uint32_t)d[12] << 24) | ((uint32_t)d[13] << 16) | ((uint32_t)d[14] << 8) | d[15];
            if (sr > 0) return (float)ts / (float)sr;
        }
        if ((ext == ".brstm" || ext == ".bcstm" ||
             ext == ".bwav" || ext == ".bcwav") && d.size() >= 0x40) {
            const uint8_t* p = d.data();
            size_t sz = d.size();
            auto r32 = [&](size_t off, bool be) -> uint32_t {
                if (off + 4 > sz) return 0;
                if (be) return ((uint32_t)p[off]<<24)|((uint32_t)p[off+1]<<16)|((uint32_t)p[off+2]<<8)|p[off+3];
                return p[off]|((uint32_t)p[off+1]<<8)|((uint32_t)p[off+2]<<16)|((uint32_t)p[off+3]<<24);
            };
            auto r16 = [&](size_t off, bool be) -> uint16_t {
                if (off + 2 > sz) return 0;
                return be ? ((uint16_t)p[off]<<8)|p[off+1] : p[off]|((uint16_t)p[off+1]<<8);
            };
            if (memcmp(p, "RSTM", 4) == 0) {
                bool be = (p[4] == 0xFE && p[5] == 0xFF);
                uint32_t headOff = r32(0x10, be);
                if (headOff + 0x10 <= sz) {
                    uint32_t h1r = r32(headOff + 0x0C, be) + 8;
                    uint32_t sr = r16(headOff + h1r + 0x04, be);
                    uint32_t ts = r32(headOff + h1r + 0x0C, be);
                    if (sr > 0 && ts > 0) return (float)ts / (float)sr;
                }
            } else if (memcmp(p, "CSTM", 4) == 0) {
                uint16_t secCount = r16(0x10, false);
                uint32_t infoOff = 0;
                for (uint32_t i = 0; i < secCount; i++) {
                    size_t e = 0x14 + i * 12;
                    if (e + 12 > sz) break;
                    if (r16(e, false) == 0x4000) { infoOff = r32(e + 4, false); break; }
                }
                if (infoOff + 0x30 <= sz) {
                    uint32_t sr = r32(infoOff + 0x24, false);
                    uint32_t ts = r32(infoOff + 0x2C, false);
                    if (sr > 0 && ts > 0) return (float)ts / (float)sr;
                }
            }
        }
        return 0.0f;
    }

private:
    static std::wstring Utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring wide(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
        wide.resize(len - 1);
        return wide;
    }

    static bool ReadFileIntoMemory(const std::string& utf8Path, std::vector<uint8_t>& outData) {
        std::wstring wpath = Utf8ToWide(utf8Path);
        FILE* f = _wfopen(wpath.c_str(), L"rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return false; }
        outData.resize(sz);
        if (fread(outData.data(), 1, sz, f) != (size_t)sz) { fclose(f); return false; }
        fclose(f);
        return true;
    }

    static bool DecodeMp3Memory(const std::vector<uint8_t>& fileData, WavData& outData) {
#ifdef ENABLE_MP3
        drmp3_config config;
        drmp3_uint64 totalPCMFrameCount;
        drmp3_int16* p = drmp3_open_memory_and_read_pcm_frames_s16(fileData.data(), fileData.size(), &config, &totalPCMFrameCount, NULL);
        if (!p) return false;
        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)config.channels;
        outData.wfx.nSamplesPerSec = config.sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = outData.wfx.nChannels * 2;
        outData.wfx.nAvgBytesPerSec = outData.wfx.nSamplesPerSec * outData.wfx.nBlockAlign;
        outData.wfx.cbSize = 0;
        size_t dataSize = (size_t)totalPCMFrameCount * config.channels * sizeof(drmp3_int16);
        outData.audioData.resize(dataSize);
        memcpy(outData.audioData.data(), p, dataSize);
        drmp3_free(p, NULL);
        return true;
#else
        return false;
#endif
    }

    static bool DecodeOggMemory(const std::vector<uint8_t>& fileData, WavData& outData) {
#ifdef ENABLE_OGG
        int error = 0;
        stb_vorbis* v = stb_vorbis_open_memory(fileData.data(), (int)fileData.size(), &error, NULL);
        if (!v) return false;
        stb_vorbis_info info = stb_vorbis_get_info(v);
        int channels = info.channels;
        int sampleRate = info.sample_rate;
        int totalSamples = stb_vorbis_stream_length_in_samples(v);
        if (totalSamples <= 0) { stb_vorbis_close(v); return false; }
        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)channels;
        outData.wfx.nSamplesPerSec = sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = channels * 2;
        outData.wfx.nAvgBytesPerSec = sampleRate * channels * 2;
        outData.wfx.cbSize = 0;
        size_t dataSize = totalSamples * channels * sizeof(short);
        outData.audioData.resize(dataSize);
        int decoded = stb_vorbis_get_samples_short_interleaved(v, channels, (short*)outData.audioData.data(), (int)(dataSize / sizeof(short)));
        stb_vorbis_close(v);
        if (decoded <= 0) return false;
        outData.audioData.resize(decoded * channels * sizeof(short));
        return true;
#else
        return false;
#endif
    }

    static bool DecodeFlacMemory(const std::vector<uint8_t>& fileData, WavData& outData) {
#ifdef ENABLE_FLAC
        unsigned int channels;
        unsigned int sampleRate;
        drflac_uint64 totalPCMFrameCount;
        drflac_int16* p = drflac_open_memory_and_read_pcm_frames_s16(fileData.data(), fileData.size(), &channels, &sampleRate, &totalPCMFrameCount, NULL);
        if (!p) return false;
        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)channels;
        outData.wfx.nSamplesPerSec = sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = channels * 2;
        outData.wfx.nAvgBytesPerSec = sampleRate * channels * 2;
        outData.wfx.cbSize = 0;
        size_t dataSize = (size_t)totalPCMFrameCount * channels * sizeof(drflac_int16);
        outData.audioData.resize(dataSize);
        memcpy(outData.audioData.data(), p, dataSize);
        drflac_free(p, NULL);
        return true;
#else
        return false;
#endif
    }

    static bool DecodeVgmstreamMemory(const std::vector<uint8_t>& fileData, const std::string& filename, WavData& outData) {
        if (fileData.empty()) return false;

        libvgmstream_t* lib = libvgmstream_init();
        if (!lib) return false;

        std::string ext;
        size_t dot = filename.find_last_of(".");
        if (dot != std::string::npos) {
            ext = filename.substr(dot);
            for (auto& c : ext) c = (char)tolower(c);
        }

        libvgmstream_config_t cfg = {};
        cfg.force_sfmt = LIBVGMSTREAM_SFMT_PCM16;
        cfg.ignore_loop = false;
        if (ext == ".adx" || ext == ".brstm" || ext == ".aax")
            cfg.force_loop = true;
        libvgmstream_setup(lib, &cfg);

        std::string name = filename.empty() ? "audio.bin" : filename;
        libstreamfile_t* sf = CreateMemStreamFile(fileData.data(), fileData.size(), name);
        int err = libvgmstream_open_stream(lib, sf, 0);
        libstreamfile_close(sf);

        if (err < 0) { libvgmstream_free(lib); return false; }

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)lib->format->channels;
        outData.wfx.nSamplesPerSec = lib->format->sample_rate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = lib->format->channels * 2;
        outData.wfx.nAvgBytesPerSec = lib->format->sample_rate * lib->format->channels * 2;
        outData.wfx.cbSize = 0;

        if (lib->format->loop_flag) {
            outData.hasLoop = true;
            outData.loopBegin = (UINT32)lib->format->loop_start;
            outData.loopLength = (UINT32)(lib->format->loop_end - lib->format->loop_start);
        }

        std::vector<uint8_t> pcmBuf;
        while (!lib->decoder->done) {
            err = libvgmstream_render(lib);
            if (err < 0) break;
            if (lib->decoder->buf_bytes > 0) {
                size_t prev = pcmBuf.size();
                pcmBuf.resize(prev + lib->decoder->buf_bytes);
                memcpy(pcmBuf.data() + prev, lib->decoder->buf, lib->decoder->buf_bytes);
            }
        }

        libvgmstream_free(lib);

        if (pcmBuf.empty()) return false;
        outData.audioData = std::move(pcmBuf);
        return true;
    }

    static bool DecodeAacMemory(const std::vector<uint8_t>& fileData, WavData& outData) {
#ifdef ENABLE_AAC
        if (fileData.size() < 16) return false;
        if ((fileData[0] == 0xFF) && ((fileData[1] & 0xF0) == 0xF0))
            return DecodeAacADTS(fileData, outData);
        else
            return DecodeAacMP4(fileData, outData);
#else
        return false;
#endif
    }

#ifdef ENABLE_AAC
    static uint32_t AacReadBE32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    static uint16_t AacReadBE16(const uint8_t* p) {
        return ((uint16_t)p[0] << 8) | p[1];
    }
    static uint64_t AacReadBE64(const uint8_t* p) {
        return ((uint64_t)AacReadBE32(p) << 32) | AacReadBE32(p + 4);
    }
    static uint32_t AacReadDescLen(const uint8_t*& p) {
        uint32_t len = 0; uint8_t b;
        do { b = *p++; len = (len << 7) | (b & 0x7F); } while (b & 0x80);
        return len;
    }
    static bool AacFindBox(const uint8_t* data, size_t dataSize, size_t start, const char type[4], size_t& outOff, size_t& outSize) {
        size_t pos = start;
        while (pos + 8 <= dataSize) {
            uint32_t sz = AacReadBE32(data + pos);
            if (sz < 8) return false;
            if (sz > dataSize - pos) sz = (uint32_t)(dataSize - pos);
            if (memcmp(data + pos + 4, type, 4) == 0) { outOff = pos; outSize = sz; return true; }
            pos += sz;
        }
        return false;
    }

    static bool DecodeAacADTS(const std::vector<uint8_t>& data, WavData& outData) {
        HAACDecoder dec = AACInitDecoder();
        if (!dec) return false;
        std::vector<int16_t> pcm;
        pcm.reserve(4096);
        short outbuf[2048 * 2];
        unsigned char* inbuf = (unsigned char*)data.data();
        int bytesLeft = (int)data.size();
        int ret = AACDecode(dec, &inbuf, &bytesLeft, outbuf);
        if (ret != ERR_AAC_NONE) { AACFreeDecoder(dec); return false; }
        AACFrameInfo fi;
        AACGetLastFrameInfo(dec, &fi);
        int nCh = fi.nChans, sr = fi.sampRateOut;
        if (nCh <= 0 || sr <= 0 || fi.outputSamps <= 0) { AACFreeDecoder(dec); return false; }
        pcm.insert(pcm.end(), outbuf, outbuf + fi.outputSamps * nCh);
        while (bytesLeft > 0 && ret == ERR_AAC_NONE) {
            ret = AACDecode(dec, &inbuf, &bytesLeft, outbuf);
            if (ret == ERR_AAC_INDATA_UNDERFLOW) break;
            if (ret != ERR_AAC_NONE) break;
            AACGetLastFrameInfo(dec, &fi);
            pcm.insert(pcm.end(), outbuf, outbuf + fi.outputSamps * nCh);
        }
        AACFreeDecoder(dec);
        if (pcm.empty()) return false;
        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)nCh;
        outData.wfx.nSamplesPerSec = sr;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = nCh * 2;
        outData.wfx.nAvgBytesPerSec = sr * nCh * 2;
        outData.wfx.cbSize = 0;
        outData.audioData.resize(pcm.size() * sizeof(int16_t));
        memcpy(outData.audioData.data(), pcm.data(), pcm.size() * sizeof(int16_t));
        return true;
    }

    static bool DecodeAacMP4(const std::vector<uint8_t>& data, WavData& outData) {
        const uint8_t* raw = data.data();
        size_t sz = data.size();
        size_t moovOff, moovSize;
        if (!AacFindBox(raw, sz, 0, "moov", moovOff, moovSize)) return false;

        int nCh = 0, sr = 0, profile = AAC_PROFILE_LC;
        std::vector<uint32_t> sampleSizes;
        std::vector<uint32_t> chunkOffsets;
        std::vector<uint32_t> stscData;

        size_t pos = moovOff + 8, moovEnd = moovOff + moovSize;
        while (pos + 8 < moovEnd) {
            uint32_t trakSize = AacReadBE32(raw + pos);
            if (trakSize < 8 || pos + trakSize > sz) break;
            if (memcmp(raw + pos + 4, "trak", 4) == 0) {
                size_t trakEnd = pos + trakSize;
                size_t mdiaOff = pos + 8, mdiaSize;
                if (!AacFindBox(raw, sz, mdiaOff, "mdia", mdiaOff, mdiaSize)) { pos += trakSize; continue; }
                size_t hdlrOff = mdiaOff + 8, hdlrSize;
                if (!AacFindBox(raw, sz, hdlrOff, "hdlr", hdlrOff, hdlrSize)) { pos += trakSize; continue; }
                if (raw + hdlrOff + 16 > raw + sz || memcmp(raw + hdlrOff + 12, "soun", 4) != 0) { pos += trakSize; continue; }

                size_t minfOff = mdiaOff + 8, minfSize;
                if (!AacFindBox(raw, sz, minfOff, "minf", minfOff, minfSize)) { pos += trakSize; continue; }
                size_t stblOff = minfOff + 8, stblSize;
                if (!AacFindBox(raw, sz, stblOff, "stbl", stblOff, stblSize)) { pos += trakSize; continue; }
                size_t stblEnd = stblOff + stblSize;

                size_t stsdOff = stblOff + 8, stsdSize;
                if (AacFindBox(raw, sz, stsdOff, "stsd", stsdOff, stsdSize)) {
                    const uint8_t* stsd = raw + stsdOff + 12;
                    if (AacReadBE32(stsd) >= 1) {
                        const uint8_t* entry = stsd + 4;
                        uint32_t entrySize = AacReadBE32(entry);
                        if (entrySize >= 28 && memcmp(entry + 4, "mp4a", 4) == 0) {
                            nCh = AacReadBE16(entry + 16);
                            sr = AacReadBE32(entry + 24) >> 16;
                            size_t esdsOff = entry - raw + 28, esdsSize;
                            if (AacFindBox(raw, sz, esdsOff, "esds", esdsOff, esdsSize)) {
                                const uint8_t* esds = raw + esdsOff + 12;
                                const uint8_t* esdsEnd = esds + esdsSize - 12;
                                while (esds < esdsEnd) {
                                    if (*esds == 0x05) {
                                        esds++;
                                        uint32_t dsiLen = AacReadDescLen(esds);
                                        if (dsiLen >= 2 && esds + dsiLen <= raw + sz) {
                                            int sri = ((esds[0] & 0x07) << 1) | ((esds[1] >> 7) & 0x01);
                                            int cc = (esds[1] >> 3) & 0x0F;
                                            static const int aacSR[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
                                            if (sri >= 0 && sri < 13) sr = aacSR[sri];
                                            if (cc > 0) nCh = cc;
                                        }
                                        break;
                                    }
                                    if (*esds == 0x03 || *esds == 0x04) { esds++; AacReadDescLen(esds); }
                                    else esds++;
                                }
                            }
                        }
                    }
                }

                size_t stszOff = stblOff + 8, stszSize;
                if (AacFindBox(raw, sz, stszOff, "stsz", stszOff, stszSize)) {
                    const uint8_t* stsz = raw + stszOff + 12;
                    uint32_t constSize = AacReadBE32(stsz);
                    uint32_t numSamp = AacReadBE32(stsz + 4);
                    if (constSize == 0) {
                        for (uint32_t i = 0; i < numSamp; i++)
                            sampleSizes.push_back(AacReadBE32(stsz + 8 + i * 4));
                    } else {
                        for (uint32_t i = 0; i < numSamp; i++)
                            sampleSizes.push_back(constSize);
                    }
                }

                size_t stcoOff = stblOff + 8, stcoSize;
                if (AacFindBox(raw, sz, stcoOff, "stco", stcoOff, stcoSize)) {
                    const uint8_t* stco = raw + stcoOff + 12;
                    uint32_t numChunks = AacReadBE32(stco);
                    for (uint32_t i = 0; i < numChunks; i++)
                        chunkOffsets.push_back(AacReadBE32(stco + 4 + i * 4));
                } else if (AacFindBox(raw, sz, stblOff + 8, "co64", stcoOff, stcoSize)) {
                    const uint8_t* stco = raw + stcoOff + 12;
                    uint32_t numChunks = AacReadBE32(stco);
                    for (uint32_t i = 0; i < numChunks; i++)
                        chunkOffsets.push_back((uint32_t)AacReadBE64(stco + 4 + i * 8));
                }

                size_t stscOff = stblOff + 8, stscSize;
                if (AacFindBox(raw, sz, stscOff, "stsc", stscOff, stscSize)) {
                    const uint8_t* stsc = raw + stscOff + 12;
                    uint32_t numEnt = AacReadBE32(stsc);
                    for (uint32_t i = 0; i < numEnt; i++) {
                        stscData.push_back(AacReadBE32(stsc + 4 + i * 12));
                        stscData.push_back(AacReadBE32(stsc + 4 + i * 12 + 4));
                        stscData.push_back(AacReadBE32(stsc + 4 + i * 12 + 8));
                    }
                }
            }
            pos += trakSize;
        }

        if (nCh <= 0 || sr <= 0 || sampleSizes.empty() || chunkOffsets.empty()) return false;

        struct AacSample { uint32_t off; uint32_t sz; };
        std::vector<AacSample> samples;
        uint32_t chunkIdx = 0, sampleIdx = 0;
        for (size_t e = 0; e + 3 <= stscData.size() && chunkIdx < (uint32_t)chunkOffsets.size(); e += 3) {
            uint32_t first = stscData[e];
            uint32_t perChunk = stscData[e + 1];
            uint32_t last = (e + 6 <= stscData.size()) ? stscData[e + 3] : (uint32_t)(chunkOffsets.size() + 1);
            for (uint32_t c = first; c < last && chunkIdx < (uint32_t)chunkOffsets.size(); c++) {
                uint32_t off = chunkOffsets[chunkIdx];
                for (uint32_t s = 0; s < perChunk && sampleIdx < (uint32_t)sampleSizes.size(); s++, sampleIdx++) {
                    samples.push_back({ off, sampleSizes[sampleIdx] });
                    off += sampleSizes[sampleIdx];
                }
                chunkIdx++;
            }
        }
        if (samples.empty()) {
            for (uint32_t i = 0; i < (uint32_t)chunkOffsets.size() && i < (uint32_t)sampleSizes.size(); i++)
                samples.push_back({ chunkOffsets[i], sampleSizes[i] });
        }

        HAACDecoder dec = AACInitDecoder();
        if (!dec) return false;
        AACFrameInfo rInfo;
        memset(&rInfo, 0, sizeof(rInfo));
        rInfo.nChans = nCh;
        rInfo.sampRateCore = sr;
        rInfo.sampRateOut = sr;
        rInfo.profile = profile;
        rInfo.bitsPerSample = 16;
        rInfo.outputSamps = 1024;
        if (AACSetRawBlockParams(dec, 0, &rInfo) != ERR_AAC_NONE) { AACFreeDecoder(dec); return false; }

        std::vector<int16_t> pcm;
        short outbuf[2048 * 2];
        for (size_t i = 0; i < samples.size(); i++) {
            uint32_t off = samples[i].off, len = samples[i].sz;
            if (off + len > sz) continue;
            unsigned char* inbuf = (unsigned char*)raw + off;
            int rem = (int)len;
            int ret = AACDecode(dec, &inbuf, &rem, outbuf);
            if (ret == ERR_AAC_NONE) {
                AACFrameInfo fi;
                AACGetLastFrameInfo(dec, &fi);
                pcm.insert(pcm.end(), outbuf, outbuf + fi.outputSamps * nCh);
            }
        }
        AACFreeDecoder(dec);
        if (pcm.empty()) return false;
        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)nCh;
        outData.wfx.nSamplesPerSec = sr;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = nCh * 2;
        outData.wfx.nAvgBytesPerSec = sr * nCh * 2;
        outData.wfx.cbSize = 0;
        outData.audioData.resize(pcm.size() * sizeof(int16_t));
        memcpy(outData.audioData.data(), pcm.data(), pcm.size() * sizeof(int16_t));
        return true;
    }
#endif


};
