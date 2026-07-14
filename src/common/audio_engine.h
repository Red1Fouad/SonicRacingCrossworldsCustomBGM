#pragma once
#include <xaudio2.h>
#include <vector>
#include <mutex>
#include <functional>
#include "audio_loader.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

struct ActiveVoice {
    IXAudio2SourceVoice* pVoice;
    BYTE* pBuffer;
    int categoryId;
    bool isBgm;
    bool isFadingOut = false;
    UINT32 totalSamples = 0;
    bool isLooping = false;
    UINT32 loopBegin = 0;
    UINT32 loopLength = 0;
    UINT32 sampleRate = 44100;
};

class AudioEngine {
private:
    IXAudio2* pXAudio2 = nullptr;
    IXAudio2MasteringVoice* pMasterVoice = nullptr;
    std::vector<ActiveVoice> m_voices;
    std::mutex m_mutex;
    bool m_comInit = false;
    bool m_trackFinishedPending = false;

public:
    std::function<void()> OnTrackFinished;
    std::function<void(const char*)> LogMsg;

    ~AudioEngine() {
        StopAll();
        if (pMasterVoice) pMasterVoice->DestroyVoice();
        if (pXAudio2) pXAudio2->Release();
        if (m_comInit) CoUninitialize();
    }

    bool Init() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return false;
        m_comInit = true;
        hr = XAudio2Create(&pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return false;
        hr = pXAudio2->CreateMasteringVoice(&pMasterVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects);
        if (FAILED(hr)) return false;
        return true;
    }

    void Update() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_voices.begin(); it != m_voices.end(); ) {
                if (it->isFadingOut) {
                    float vol;
                    it->pVoice->GetVolume(&vol);
                    vol -= 0.02f;
                    if (vol <= 0.0f) {
                        if (LogMsg) { char buf[256]; snprintf(buf, sizeof(buf), "Update: fadingOut voice destroyed, isBgm=%d", it->isBgm); LogMsg(buf); }
                        it->pVoice->Stop();
                        it->pVoice->FlushSourceBuffers();
                        it->pVoice->DestroyVoice();
                        delete[] it->pBuffer;
                        it = m_voices.erase(it);
                        continue;
                    } else {
                        it->pVoice->SetVolume(vol);
                    }
                }
                XAUDIO2_VOICE_STATE state;
                it->pVoice->GetState(&state);
                if (state.BuffersQueued == 0) {
                    bool wasBgm = it->isBgm && !it->isFadingOut;
                    if (LogMsg) { char buf[256]; snprintf(buf, sizeof(buf), "Update: BuffersQueued=0, isBgm=%d, isFadingOut=%d, wasBgm=%d", it->isBgm, it->isFadingOut, wasBgm); LogMsg(buf); }
                    it->pVoice->DestroyVoice();
                    delete[] it->pBuffer;
                    it = m_voices.erase(it);
                    if (wasBgm && OnTrackFinished)
                        m_trackFinishedPending = true;
                } else {
                    ++it;
                }
            }
        }
        if (m_trackFinishedPending) {
            m_trackFinishedPending = false;
            if (OnTrackFinished) OnTrackFinished();
        }
    }

    void StopAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            v.pVoice->DestroyVoice();
            delete[] v.pBuffer;
        }
        m_voices.clear();
    }

    void SetCategoryVolume(int catId, float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            if (v.categoryId == catId && !v.isFadingOut)
                v.pVoice->SetVolume(volume);
        }
    }

    void StopCategory(int catId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        int count = 0;
        for (auto& v : m_voices) {
            if (v.categoryId == catId) {
                v.isFadingOut = true;
                count++;
            }
        }
        if (LogMsg) { char buf[128]; snprintf(buf, sizeof(buf), "StopCategory(%d): marked %d voices fading", catId, count); LogMsg(buf); }
    }

    void StopCategoryImmediate(int catId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_voices.begin(); it != m_voices.end(); ) {
            if (it->categoryId == catId) {
                it->pVoice->Stop();
                it->pVoice->FlushSourceBuffers();
                it->pVoice->DestroyVoice();
                delete[] it->pBuffer;
                it = m_voices.erase(it);
            } else {
                ++it;
            }
        }
    }

    void PauseCategory(int catId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            if (v.categoryId == catId && !v.isFadingOut)
                v.pVoice->Stop(0);
        }
    }

    void ResumeCategory(int catId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            if (v.categoryId == catId && !v.isFadingOut)
                v.pVoice->Start(0);
        }
    }

    bool GetPlaybackProgress(float& outProgress, float& outSeconds, float& outDuration) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            if (v.isBgm && !v.isFadingOut) {
                XAUDIO2_VOICE_STATE state;
                v.pVoice->GetState(&state);
                float rate = (float)v.sampleRate;
                if (rate <= 0.0f) rate = 44100.0f;
                UINT64 played = state.SamplesPlayed;
                if (v.isLooping && v.loopLength > 0) {
                    UINT64 loopRegion = played - v.loopBegin;
                    UINT32 posInLoop = (loopRegion < (UINT64)v.loopLength) ? (UINT32)loopRegion : (UINT32)(loopRegion % v.loopLength);
                    outSeconds = (float)posInLoop / rate;
                    outDuration = (float)v.loopLength / rate;
                    outProgress = (float)posInLoop / (float)v.loopLength;
                } else {
                    outSeconds = (float)played / rate;
                    outDuration = (float)v.totalSamples / rate;
                    outProgress = (v.totalSamples > 0) ? (float)played / (float)v.totalSamples : 0.0f;
                }
                if (outProgress > 1.0f) outProgress = 1.0f;
                return true;
            }
        }
        outProgress = 0.0f;
        outSeconds = 0.0f;
        outDuration = 0.0f;
        return false;
    }

    void PlayPreloaded(const WavData& data, float volume, int categoryId, bool allowLoop = true) {
        if (!pXAudio2) return;
        bool isBgm = (categoryId == 0);
        if (isBgm) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& v : m_voices) {
                if (v.isBgm) v.isFadingOut = true;
            }
        }
        IXAudio2SourceVoice* pSourceVoice;
        HRESULT hr = pXAudio2->CreateSourceVoice(&pSourceVoice, &data.wfx);
        if (FAILED(hr)) return;
        XAUDIO2_BUFFER buffer = { 0 };
        buffer.AudioBytes = (UINT32)data.audioData.size();
        BYTE* persistentBuffer = new BYTE[data.audioData.size()];
        memcpy(persistentBuffer, data.audioData.data(), data.audioData.size());
        buffer.pAudioData = persistentBuffer;
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        if (data.hasLoop && data.loopLength > 0) {
            buffer.LoopBegin = data.loopBegin;
            buffer.LoopLength = data.loopLength;
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        } else if (isBgm && allowLoop) {
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        hr = pSourceVoice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            pSourceVoice->DestroyVoice();
            delete[] persistentBuffer;
            return;
        }
        pSourceVoice->SetVolume(volume);
        pSourceVoice->Start(0);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            UINT32 bytesPerSample = data.wfx.nChannels * (data.wfx.wBitsPerSample / 8);
            UINT32 totalSamp = (bytesPerSample > 0) ? buffer.AudioBytes / bytesPerSample : 0;
            bool looping = (buffer.LoopCount != 0);
            UINT32 lb = looping ? (UINT32)buffer.LoopBegin : 0;
            UINT32 ll = looping ? (UINT32)buffer.LoopLength : 0;
            UINT32 sr = data.wfx.nSamplesPerSec > 0 ? data.wfx.nSamplesPerSec : 44100;
            m_voices.push_back({ pSourceVoice, persistentBuffer, categoryId, isBgm, false, totalSamp, looping, lb, ll, sr });
        }
    }
};
