#pragma once

#include <JuceHeader.h>
#include <Eigen/Core>
#include <Eigen/Dense>
#include <algorithm>
#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <cstring>

class E1MorphAudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxBlockSamples = 2048;
    static constexpr int kFftSize = 2048;
    static constexpr int kFftBins = (kFftSize / 2) + 1;
    static constexpr int kCompressedBands = 64;
    static constexpr int kAudioToWorkerCapacity = 128;
    static constexpr int kWorkerToAudioCapacity = 128;

    struct AudioFrame
    {
        uint64_t sequence = 0;
        int numChannels = 0;
        int numSamples = 0;
        std::array<std::array<float, kMaxBlockSamples>, kMaxChannels> source {};
        std::array<std::array<float, kMaxBlockSamples>, kMaxChannels> target {};
    };

    struct MorphResultFrame
    {
        uint64_t sequence = 0;
        int numChannels = 0;
        int numSamples = 0;
        std::array<std::array<float, kMaxBlockSamples>, kMaxChannels> output {};
    };

    template <typename T, int Capacity>
    class LockFreeFifo
    {
    public:
        bool push(const T& item) noexcept
        {
            int start1, size1, start2, size2;
            fifo.prepareToWrite(1, start1, size1, start2, size2);
            if (size1 <= 0)
                return false;

            storage[static_cast<size_t>(start1)] = item;
            fifo.finishedWrite(size1);
            return true;
        }

        bool pop(T& item) noexcept
        {
            int start1, size1, start2, size2;
            fifo.prepareToRead(1, start1, size1, start2, size2);
            if (size1 <= 0)
                return false;

            item = storage[static_cast<size_t>(start1)];
            fifo.finishedRead(size1);
            return true;
        }

        int getNumReady() const noexcept
        {
            return fifo.getNumReady();
        }

        void reset() noexcept
        {
            fifo.reset();
        }

    private:
        juce::AbstractFifo fifo { Capacity };
        std::array<T, Capacity> storage {};
    };

    E1MorphAudioProcessor();
    ~E1MorphAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    class WorkerThread final : public juce::Thread
    {
    public:
        explicit WorkerThread(E1MorphAudioProcessor& ownerRef);
        ~WorkerThread() override;

        void prepare(double sampleRate, int maxBlockSize) noexcept;
        void setMorphAmount(float amount) noexcept;
        void setFocusAmount(float amount) noexcept;
        void setGlideAmount(float amount) noexcept;
        void setTransientBypassAmount(float amount) noexcept;
        void setEnvModAmount(float amount) noexcept;
        void setEnvShapeAmount(float amount) noexcept;
        void setEnvSource(int source) noexcept;
        void setNonRealtimeMode(bool isNonRealtime) noexcept;
        void setSidechainActive(bool isActive) noexcept;
        void run() override;

    private:
        static constexpr int kHopSize = kFftSize / 4;
        static constexpr int kRingSize = 1 << 17;
        static constexpr int kRingMask = kRingSize - 1;
        static constexpr int kSinkhornIterations = 20;
        static constexpr float kDistributionFloor = 1.0e-6f;
        static constexpr float kSinkhornEpsilon = 10.0f;
        static_assert((kRingSize & (kRingSize - 1)) == 0, "kRingSize must be power-of-two");

        using Cpx = std::complex<float>;

        void processOneFrame(const AudioFrame& in, MorphResultFrame& out);
        void appendInputSamples(const AudioFrame& in);
        void renderAvailableStftFrames();
        void popOutputSamples(int numChannels, int numSamples, MorphResultFrame& out) noexcept;

        void computeStftMagnitudes(uint64_t frameStartSample, int numChannels);
        void compressSpectrumToBands();
        void runSinkhornBarycenter(float morph, float focus);
        void expandBandsToSpectrum(float glide);
        float computeAnalysisEnvelopeFromMagnitudes(int numChannels, bool useSourceForEnvelope) noexcept;
        void synthesizeUsingSourcePhase(uint64_t frameStartSample, int numChannels);
        void resetStreamingState(double sampleRate, int maxBlockSize) noexcept;

        E1MorphAudioProcessor& owner;
        std::atomic<float> morphAmount { 0.5f };
        std::atomic<float> focusAmount { 1.0f };
        std::atomic<float> glideAmount { 0.0f };
        std::atomic<float> transientBypassAmount { 0.0f };
        std::atomic<float> envModAmount { 0.0f };
        std::atomic<float> envShapeAmount { 0.0f };
        std::atomic<int> envSource { 1 }; // 0: Source, 1: Sidechain
        std::atomic<bool> nonRealtimeMode { false };
        std::atomic<bool> sidechainActive { false };
        double sampleRateHz = 44100.0;
        int maxBlock = 512;

        juce::dsp::FFT fft { 11 };
        juce::dsp::WindowingFunction<float> hannWindow {
            static_cast<size_t>(kFftSize),
            juce::dsp::WindowingFunction<float>::hann,
            false
        };
        std::array<float, kFftSize> window {};
        std::array<float, kFftSize> windowSquared {};

        std::array<std::array<float, kRingSize>, kMaxChannels> sourceInputRing {};
        std::array<std::array<float, kRingSize>, kMaxChannels> targetInputRing {};
        std::array<std::array<float, kRingSize>, kMaxChannels> outputOlaRing {};
        std::array<std::array<float, kRingSize>, kMaxChannels> outputNormRing {};

        std::array<std::array<Cpx, kFftSize>, kMaxChannels> srcTime {};
        std::array<std::array<Cpx, kFftSize>, kMaxChannels> tgtTime {};
        std::array<std::array<Cpx, kFftSize>, kMaxChannels> srcSpec {};
        std::array<std::array<Cpx, kFftSize>, kMaxChannels> tgtSpec {};
        std::array<std::array<Cpx, kFftSize>, kMaxChannels> resynthSpec {};
        std::array<std::array<Cpx, kFftSize>, kMaxChannels> ifftTime {};

        std::array<std::array<float, kFftBins>, kMaxChannels> srcMagnitude {};
        std::array<std::array<float, kFftBins>, kMaxChannels> tgtMagnitude {};
        std::array<std::array<float, kFftBins>, kMaxChannels> srcPhase {};
        std::array<std::array<float, kFftBins>, kMaxChannels> morphedMagnitude {};
        std::array<std::array<float, kFftBins>, kMaxChannels> prevMorphedMagnitude {};
        std::array<float, kMaxChannels> prevSrcEnergy {};
        std::array<float, kMaxChannels> transientEnv {};

        std::array<Eigen::VectorXf, kMaxChannels> srcBandsPerChannel;
        std::array<Eigen::VectorXf, kMaxChannels> tgtBandsPerChannel;
        std::array<Eigen::VectorXf, kMaxChannels> baryBandsPerChannel;

        Eigen::VectorXf srcBands;
        Eigen::VectorXf tgtBands;
        Eigen::VectorXf baryBands;
        Eigen::MatrixXf costMatrix;
        Eigen::MatrixXf kernelK;
        Eigen::MatrixXf transportPlan;
        Eigen::VectorXf u;
        Eigen::VectorXf v;
        Eigen::VectorXf tmpKv;
        Eigen::VectorXf tmpKTu;

        int activeChannels = 0;
        uint64_t inputWriteSample = 0;
        uint64_t nextFrameStartSample = 0;
        int64_t outputReadSample = -kFftSize;
        float smoothedMorph = 0.0f;
        float smoothedFocus = 1.0f;
        float smoothedGlide = 0.0f;
        float envelopeFollower = 0.0f;
        float envelopeReference = 1.0f;
        float smoothedShapingGain = 1.0f;
        uint64_t framesProcessedSinceReset = 0;
        uint64_t softStartFramesTotal = 1;

        std::atomic<double> pendingSampleRateHz { 44100.0 };
        std::atomic<int> pendingMaxBlock { 512 };
        std::atomic<bool> prepareRequested { true };
    };

    void pushFrameToWorker(const juce::AudioBuffer<float>& source,
                           const juce::AudioBuffer<float>& target) noexcept;
    bool popProcessedFrame(MorphResultFrame& outFrame) noexcept;
    void copyProcessedToOutput(const MorphResultFrame& frame,
                               juce::AudioBuffer<float>& output) noexcept;
    void copySourceToOutput(const juce::AudioBuffer<float>& source,
                            juce::AudioBuffer<float>& output) noexcept;

    LockFreeFifo<AudioFrame, kAudioToWorkerCapacity> audioToWorkerFifo;
    LockFreeFifo<MorphResultFrame, kWorkerToAudioCapacity> workerToAudioFifo;

    juce::AudioProcessorValueTreeState apvts;
    WorkerThread worker { *this };

    std::atomic<float> uiMorphAmount { 0.5f };
    std::atomic<uint64_t> sequenceCounter { 0 };

    double currentSampleRate = 44100.0;
    int currentMaxBlockSize = 512;
    bool wasOfflineLastBlock = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(E1MorphAudioProcessor)
};
