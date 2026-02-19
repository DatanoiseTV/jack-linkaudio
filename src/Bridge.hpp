#pragma once

#include <ableton/LinkAudio.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/link_audio/Queue.hpp>
#include <jack/jack.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ableton {
namespace linkaudio {

class Bridge {
public:
    Bridge(const std::string& name, int numInputs);
    ~Bridge();

    void start();
    void stop();

    bool isRunning() const { return mRunning; }

private:
    // Handler for a single Link Audio Source (Link -> JACK Output)
    struct SourceHandler {
        ChannelId id;
        std::string name;
        std::string peerName;
        std::unique_ptr<LinkAudioSource> source;
        jack_port_t* port;
        
        // Resampling / Buffering logic similar to LinkAudioRenderer
        struct Buffer {
            std::array<double, 4096> samples;
            LinkAudioSource::BufferHandle::Info info;
        };
        using Queue = link_audio::Queue<Buffer>;
        std::unique_ptr<typename Queue::Writer> queueWriter;
        std::unique_ptr<typename Queue::Reader> queueReader;
        
        std::optional<double> startReadPos;
        std::array<double, 4> sampleCache = {{0.0, 0.0, 0.0, 0.0}};
        std::optional<size_t> lastFrameIdx;
        
        SourceHandler(LinkAudio& link, ChannelId id, const std::string& name, const std::string& peerName, jack_client_t* jack);
        ~SourceHandler();
        
        void onBuffer(LinkAudioSource::BufferHandle handle);
        void process(size_t nframes, typename Link::SessionState sessionState, double sampleRate, std::chrono::microseconds hostTime, double quantum, std::chrono::microseconds outputLatency);
    };

    // Handler for a single Link Audio Sink (JACK Input -> Link)
    struct SinkHandler {
        std::unique_ptr<LinkAudioSink> sink;
        jack_port_t* port;
        
        SinkHandler(LinkAudio& link, const std::string& name, jack_client_t* jack);
        ~SinkHandler();
        
        void process(size_t nframes, typename Link::SessionState sessionState, double sampleRate, std::chrono::microseconds hostTime, double quantum, std::chrono::microseconds inputLatency);
    };

    static int jackProcessCallback(jack_nframes_t nframes, void* arg);
    int process(jack_nframes_t nframes);
    
    static void jackLatencyCallback(jack_latency_callback_mode_t mode, void* arg);
    void updateLatencies();

    static void jackShutdownCallback(void* arg);

    void onChannelsChanged();

    std::string mName;
    jack_client_t* mJackClient = nullptr;
    LinkAudio mLink;
    link::HostTimeFilter<Link::Clock> mHostTimeFilter;
    
    std::vector<std::unique_ptr<SinkHandler>> mSinks;
    std::map<ChannelId, std::unique_ptr<SourceHandler>> mSources;
    std::mutex mSourcesMutex;
    
    double mSampleRate = 44100.0;
    double mSampleTime = 0.0;
    std::atomic<bool> mRunning{false};
    
    // Latencies in microseconds
    std::atomic<int64_t> mGlobalInputLatency{0};
    std::atomic<int64_t> mGlobalOutputLatency{0};
};

} // namespace linkaudio
} // namespace ableton
