#include "Bridge.hpp"
#include <ableton/util/FloatIntConversion.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>

namespace ableton {
namespace linkaudio {

namespace {

template <typename T>
T cubicInterpolate(const std::array<T, 4>& p, double t) {
    double a = -0.5 * static_cast<double>(p[0]) + 1.5 * static_cast<double>(p[1])
               - 1.5 * static_cast<double>(p[2]) + 0.5 * static_cast<double>(p[3]);
    double b = static_cast<double>(p[0]) - 2.5 * static_cast<double>(p[1])
               + 2.0 * static_cast<double>(p[2]) - 0.5 * static_cast<double>(p[3]);
    double c = -0.5 * static_cast<double>(p[0]) + 0.5 * static_cast<double>(p[2]);
    double d = static_cast<double>(p[1]);
    return static_cast<T>(a * t * t * t + b * t * t + c * t + d);
}

template <typename T>
T linearInterpolate(T value, T inMin, T inMax, T outMin, T outMax) {
    return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

} // namespace

// --- SinkHandler ---

Bridge::SinkHandler::SinkHandler(LinkAudio& link, const std::string& name, jack_client_t* jack) {
    port = jack_port_register(jack, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!port) {
        throw std::runtime_error("Failed to register JACK input port: " + name);
    }
    // LinkAudioSink(LinkAudio& link, std::string name, size_t maxNumSamples)
    sink = std::make_unique<LinkAudioSink>(link, name, 8192);
}

Bridge::SinkHandler::~SinkHandler() {
    // Port will be unregistered by jack_client_close or we could do it here
}

void Bridge::SinkHandler::process(size_t nframes, typename Link::SessionState sessionState, double sampleRate, 
                                  std::chrono::microseconds hostTime, double quantum, std::chrono::microseconds inputLatency) {
    float* in = static_cast<float*>(jack_port_get_buffer(port, nframes));
    if (!in) return;

    auto buffer = LinkAudioSink::BufferHandle(*sink);
    if (buffer) {
        for (size_t i = 0; i < nframes; ++i) {
            buffer.samples[i] = ableton::util::floatToInt16(in[i]);
        }
        
        // Proper latency: the audio we just read from the buffer was captured at hostTime - inputLatency
        const auto captureTime = hostTime - inputLatency;
        const auto beatsAtBufferBegin = sessionState.beatAtTime(captureTime, quantum);
        
        buffer.commit(sessionState, beatsAtBufferBegin, quantum, nframes, 1, static_cast<uint32_t>(sampleRate));
    }
}

// --- SourceHandler ---

Bridge::SourceHandler::SourceHandler(LinkAudio& link, ChannelId id, const std::string& name, const std::string& peerName, jack_client_t* jack)
    : id(id), name(name), peerName(peerName) {
    
    std::string portName = peerName + "_" + name;
    // Sanitize port name (JACK doesn't like some chars)
    std::replace(portName.begin(), portName.end(), ' ', '_');
    std::replace(portName.begin(), portName.end(), ':', '_');
    
    port = jack_port_register(jack, portName.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!port) {
        throw std::runtime_error("Failed to register JACK output port: " + portName);
    }
    
    auto queue = Queue(4096, {});
    queueWriter = std::make_unique<typename Queue::Writer>(std::move(queue.writer()));
    queueReader = std::make_unique<typename Queue::Reader>(std::move(queue.reader()));
    
    source = std::make_unique<LinkAudioSource>(link, id, [this](LinkAudioSource::BufferHandle handle) {
        onBuffer(handle);
    });
}

Bridge::SourceHandler::~SourceHandler() {
    // port unregistered by jack_client_close
}

void Bridge::SourceHandler::onBuffer(LinkAudioSource::BufferHandle handle) {
    if (queueWriter->retainSlot()) {
        auto& buffer = *((*queueWriter)[0]);
        buffer.info = handle.info;
        // We assume mono for now as per SinkHandler, or we take the first channel
        for (size_t i = 0; i < handle.info.numFrames && i < buffer.samples.size(); ++i) {
            buffer.samples[i] = ableton::util::int16ToFloat<double>(handle.samples[i * handle.info.numChannels]);
        }
        queueWriter->releaseSlot();
    }
}

void Bridge::SourceHandler::process(size_t nframes, typename Link::SessionState sessionState, double sampleRate, 
                                    std::chrono::microseconds hostTime, double quantum, std::chrono::microseconds outputLatency) {
    float* out = static_cast<float*>(jack_port_get_buffer(port, nframes));
    if (!out) return;
    std::fill_n(out, nframes, 0.0f);

    while (queueReader->retainSlot()) {}

    // Proper latency: audio we write now will be heard at hostTime + outputLatency
    const auto playTime = hostTime + outputLatency;
    
    // We want a bit of safety buffer for network jitter
    constexpr auto kLatencyInBeats = 1.0; 
    const auto targetBeatsAtBufferBegin = sessionState.beatAtTime(playTime, quantum) - kLatencyInBeats;
    
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(double(nframes) / sampleRate));
    const auto targetBeatsAtBufferEnd = sessionState.beatAtTime(playTime + duration, quantum) - kLatencyInBeats;

    // Drop old slots
    while (!startReadPos && queueReader->numRetainedSlots() > 0) {
        auto endBeats = (*queueReader)[0]->info.endBeats(sessionState, quantum);
        if (endBeats && *endBeats < targetBeatsAtBufferBegin) {
            queueReader->releaseSlot();
        } else {
            break;
        }
    }

    if (queueReader->numRetainedSlots() == 0) {
        lastFrameIdx = std::nullopt;
        startReadPos = std::nullopt;
        return;
    }

    // Next buffer too new?
    auto beginBeats = (*queueReader)[0]->info.beginBeats(sessionState, quantum);
    if (!startReadPos && beginBeats && *beginBeats > targetBeatsAtBufferBegin) {
        return;
    }

    if (!startReadPos && beginBeats) {
        auto endBeats = (*queueReader)[0]->info.endBeats(sessionState, quantum);
        startReadPos = linearInterpolate(targetBeatsAtBufferBegin, *beginBeats, *endBeats, 0.0, double((*queueReader)[0]->info.numFrames));
    }

    if (!startReadPos) return;

    // Calculate frame increment for re-pitching
    double totalFramesInQueueRange = 0.0;
    bool foundEnd = false;
    for (size_t i = 0; i < queueReader->numRetainedSlots(); ++i) {
        auto b = (*queueReader)[i]->info.beginBeats(sessionState, quantum);
        auto e = (*queueReader)[i]->info.endBeats(sessionState, quantum);
        if (b && e && targetBeatsAtBufferEnd >= *b && targetBeatsAtBufferEnd < *e) {
            totalFramesInQueueRange += linearInterpolate(targetBeatsAtBufferEnd, *b, *e, 0.0, double((*queueReader)[i]->info.numFrames));
            foundEnd = true;
            break;
        } else {
            totalFramesInQueueRange += (*queueReader)[i]->info.numFrames;
        }
    }

    if (!foundEnd) {
        // Not enough audio yet
        return;
    }

    totalFramesInQueueRange -= *startReadPos;
    if (totalFramesInQueueRange <= 0.0) {
        lastFrameIdx = std::nullopt;
        startReadPos = std::nullopt;
        return;
    }

    const double frameIncrement = totalFramesInQueueRange / double(nframes);
    double readPos = *startReadPos;

    auto getSample = [&](size_t idx) -> double {
        size_t bufferIdx = 0;
        size_t currentIdx = idx;
        while (bufferIdx < queueReader->numRetainedSlots()) {
            auto& buf = *((*queueReader)[bufferIdx]);
            if (currentIdx < buf.info.numFrames) return buf.samples[currentIdx];
            currentIdx -= buf.info.numFrames;
            ++bufferIdx;
        }
        return 0.0;
    };

    for (size_t i = 0; i < nframes; ++i) {
        const double framePos = readPos + i * frameIncrement;
        const size_t frameIdx = static_cast<size_t>(std::floor(framePos));
        const double t = framePos - std::floor(framePos);

        while (!lastFrameIdx || frameIdx > *lastFrameIdx) {
            sampleCache[3] = sampleCache[2];
            sampleCache[2] = sampleCache[1];
            sampleCache[1] = sampleCache[0];
            sampleCache[0] = (frameIdx > 0) ? getSample(frameIdx - 1) : getSample(0);
            lastFrameIdx = lastFrameIdx ? (*lastFrameIdx + 1) : frameIdx;
        }

        out[i] = static_cast<float>(cubicInterpolate(sampleCache, t));
        
        // Drop consumed buffers
        const auto& currentInfo = (*queueReader)[0]->info;
        if (frameIdx >= currentInfo.numFrames) {
            readPos -= currentInfo.numFrames;
            *lastFrameIdx -= currentInfo.numFrames;
            queueReader->releaseSlot();
        }
    }
    
    *startReadPos = readPos + nframes * frameIncrement;
}

// --- Bridge ---

Bridge::Bridge(const std::string& name, int numInputs, bool sync)
    : mName(name), mLink(120.0, name), mSyncEnabled(sync) {
    
    jack_status_t status;
    mJackClient = jack_client_open(name.c_str(), JackNoStartServer, &status);
    if (!mJackClient) {
        throw std::runtime_error("Failed to open JACK client");
    }

    mSampleRate = jack_get_sample_rate(mJackClient);
    
    jack_set_process_callback(mJackClient, jackProcessCallback, this);
    jack_set_latency_callback(mJackClient, jackLatencyCallback, this);
    jack_on_shutdown(mJackClient, jackShutdownCallback, this);

    if (mSyncEnabled) {
        jack_set_timebase_callback(mJackClient, true, jackTimebaseCallback, this);
    }

    for (int i = 0; i < numInputs; ++i) {
        std::string portName = "in_" + std::to_string(i + 1);
        mSinks.push_back(std::make_unique<SinkHandler>(mLink, portName, mJackClient));
    }

    mLink.setChannelsChangedCallback([this]() { onChannelsChanged(); });
    mLink.enable(true);
    mLink.enableLinkAudio(true);
}

Bridge::~Bridge() {
    if (mSyncEnabled && mJackClient) {
        jack_set_timebase_callback(mJackClient, false, nullptr, nullptr);
    }
    stop();
    if (mJackClient) {
        jack_client_close(mJackClient);
    }
}

void Bridge::start() {
    if (jack_activate(mJackClient) != 0) {
        throw std::runtime_error("Failed to activate JACK client");
    }
    mRunning = true;
}

void Bridge::stop() {
    if (mRunning) {
        jack_deactivate(mJackClient);
        mRunning = false;
    }
}

int Bridge::jackProcessCallback(jack_nframes_t nframes, void* arg) {
    return static_cast<Bridge*>(arg)->process(nframes);
}

int Bridge::process(jack_nframes_t nframes) {
    const auto hostTime = mHostTimeFilter.sampleTimeToHostTime(mSampleTime);
    mSampleTime += nframes;

    auto sessionState = mLink.captureAudioSessionState();
    double quantum = 4.0; // Default quantum

    std::chrono::microseconds inLat(mGlobalInputLatency.load());
    std::chrono::microseconds outLat(mGlobalOutputLatency.load());

    for (auto& sink : mSinks) {
        sink->process(nframes, sessionState, mSampleRate, hostTime, quantum, inLat);
    }

    {
        std::unique_lock<std::mutex> lock(mSourcesMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (auto& pair : mSources) {
                pair.second->process(nframes, sessionState, mSampleRate, hostTime, quantum, outLat);
            }
        }
    }

    return 0;
}

void Bridge::jackLatencyCallback(jack_latency_callback_mode_t mode, void* arg) {
    static_cast<Bridge*>(arg)->updateLatencies();
}

void Bridge::updateLatencies() {
    jack_latency_range_t range;
    
    // Average or max input latency
    int64_t maxIn = 0;
    for (auto& sink : mSinks) {
        jack_port_get_latency_range(sink->port, JackCaptureLatency, &range);
        if (range.max > (jack_nframes_t)maxIn) maxIn = range.max;
    }
    mGlobalInputLatency.store(static_cast<int64_t>(1e6 * maxIn / mSampleRate));

    // For output, we look at the first source if it exists, or just use a default
    int64_t maxOut = 0;
    std::lock_guard<std::mutex> lock(mSourcesMutex);
    for (auto& pair : mSources) {
        jack_port_get_latency_range(pair.second->port, JackPlaybackLatency, &range);
        if (range.max > (jack_nframes_t)maxOut) maxOut = range.max;
    }
    mGlobalOutputLatency.store(static_cast<int64_t>(1e6 * maxOut / mSampleRate));
}

void Bridge::jackShutdownCallback(void* arg) {
    static_cast<Bridge*>(arg)->mRunning = false;
}

void Bridge::jackTimebaseCallback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t* pos, int new_pos, void* arg) {
    static_cast<Bridge*>(arg)->timebaseCallback(state, nframes, pos, new_pos);
}

void Bridge::timebaseCallback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t* pos, int new_pos) {
    auto sessionState = mLink.captureAudioSessionState();
    const auto time = mLink.clock().micros();
    const double quantum = 4.0;
    const double beat = sessionState.beatAtTime(time, quantum);

    pos->valid = JackPositionBBT;
    pos->beats_per_bar = static_cast<float>(quantum);
    pos->beat_type = 4;
    pos->ticks_per_beat = 1920;
    pos->beats_per_minute = sessionState.tempo();
    
    pos->bar = static_cast<int32_t>(beat / quantum) + 1;
    pos->beat = static_cast<int32_t>(fmod(beat, quantum)) + 1;
    pos->tick = static_cast<int32_t>(fmod(beat, 1.0) * pos->ticks_per_beat);
    
    if (sessionState.isPlaying()) {
        if (state == JackTransportStopped) {
            jack_transport_start(mJackClient);
        }
        jack_transport_reposition(mJackClient, pos);
    } else {
        if (state == JackTransportRolling) {
            jack_transport_stop(mJackClient);
        }
    }
}

void Bridge::onChannelsChanged() {
    auto channels = mLink.channels();
    std::lock_guard<std::mutex> lock(mSourcesMutex);

    // Add new sources
    for (const auto& ch : channels) {
        if (mSources.find(ch.id) == mSources.end()) {
            try {
                mSources[ch.id] = std::make_unique<SourceHandler>(mLink, ch.id, ch.name, ch.peerName, mJackClient);
                std::cout << "Added source: " << ch.peerName << " - " << ch.name << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Failed to add source: " << e.what() << std::endl;
            }
        }
    }

    // Remove old sources
    for (auto it = mSources.begin(); it != mSources.end(); ) {
        bool found = false;
        for (const auto& ch : channels) {
            if (ch.id == it->first) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cout << "Removed source: " << it->second->peerName << " - " << it->second->name << std::endl;
            it = mSources.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace linkaudio
} // namespace ableton
