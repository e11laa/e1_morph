#include "PluginProcessor.h"

E1MorphAudioProcessor::E1MorphAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
      )
{
}

E1MorphAudioProcessor::~E1MorphAudioProcessor()
{
    worker.stop();
}

const juce::String E1MorphAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool E1MorphAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool E1MorphAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool E1MorphAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double E1MorphAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int E1MorphAudioProcessor::getNumPrograms()
{
    return 1;
}

int E1MorphAudioProcessor::getCurrentProgram()
{
    return 0;
}

void E1MorphAudioProcessor::setCurrentProgram(int)
{
}

const juce::String E1MorphAudioProcessor::getProgramName(int)
{
    return {};
}

void E1MorphAudioProcessor::changeProgramName(int, const juce::String&)
{
}

void E1MorphAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentMaxBlockSize = samplesPerBlock;

    audioToWorkerFifo.reset();
    workerToAudioFifo.reset();

    const int analysisSynthesisLatency = kFftSize;
    const int queueSafety = juce::jmax(0, currentMaxBlockSize);
    setLatencySamples(analysisSynthesisLatency + queueSafety);

    worker.start(sampleRate, samplesPerBlock);
}

void E1MorphAudioProcessor::releaseResources()
{
    worker.stop();
    setLatencySamples(0);
}

bool E1MorphAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainIn != mainOut)
        return false;

    if (!(mainIn == juce::AudioChannelSet::mono() || mainIn == juce::AudioChannelSet::stereo()))
        return false;

    if (getBusCount(true) > 1)
    {
        const auto side = layouts.getChannelSet(true, 1);
        const bool sideOk =
            side.isDisabled() ||
            side == juce::AudioChannelSet::mono() ||
            side == juce::AudioChannelSet::stereo();

        if (!sideOk)
            return false;
    }

    return true;
#endif
}

void E1MorphAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    auto source = getBusBuffer(buffer, true, 0);
    auto output = getBusBuffer(buffer, false, 0);

    for (int ch = output.getNumChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const bool hasSidechain = (getBusCount(true) > 1 && getChannelCountOfBus(true, 1) > 0);
    const auto target = hasSidechain ? getBusBuffer(buffer, true, 1) : source;

    pushFrameToWorker(source, target);

    MorphResultFrame processed {};
    if (popProcessedFrame(processed))
        copyProcessedToOutput(processed, output);
    else
        copySourceToOutput(source, output);
}

void E1MorphAudioProcessor::pushFrameToWorker(const juce::AudioBuffer<float>& source,
                                              const juce::AudioBuffer<float>& target) noexcept
{
    AudioFrame frame {};
    frame.sequence = sequenceCounter.fetch_add(1, std::memory_order_relaxed);
    frame.numChannels = juce::jmin(kMaxChannels, source.getNumChannels());
    frame.numSamples = juce::jmin(kMaxBlockSamples, source.getNumSamples());

    for (int ch = 0; ch < frame.numChannels; ++ch)
    {
        std::memcpy(frame.source[static_cast<size_t>(ch)].data(),
                    source.getReadPointer(ch),
                    sizeof(float) * static_cast<size_t>(frame.numSamples));

        const int targetCh = (target.getNumChannels() > 0)
                                 ? juce::jmin(ch, target.getNumChannels() - 1)
                                 : 0;

        std::memcpy(frame.target[static_cast<size_t>(ch)].data(),
                    target.getReadPointer(targetCh),
                    sizeof(float) * static_cast<size_t>(frame.numSamples));
    }

    if (!audioToWorkerFifo.push(frame))
    {
        // FIFO full: drop newest frame to preserve RT safety.
    }
}

bool E1MorphAudioProcessor::popProcessedFrame(MorphResultFrame& outFrame) noexcept
{
    return workerToAudioFifo.pop(outFrame);
}

void E1MorphAudioProcessor::copyProcessedToOutput(const MorphResultFrame& frame,
                                                  juce::AudioBuffer<float>& output) noexcept
{
    output.clear();

    const int numChannels = juce::jmin(output.getNumChannels(), frame.numChannels);
    const int numSamples = juce::jmin(output.getNumSamples(), frame.numSamples);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        std::memcpy(output.getWritePointer(ch),
                    frame.output[static_cast<size_t>(ch)].data(),
                    sizeof(float) * static_cast<size_t>(numSamples));
    }
}

void E1MorphAudioProcessor::copySourceToOutput(const juce::AudioBuffer<float>& source,
                                               juce::AudioBuffer<float>& output) noexcept
{
    output.clear();

    const int numChannels = juce::jmin(output.getNumChannels(), source.getNumChannels());
    const int numSamples = juce::jmin(output.getNumSamples(), source.getNumSamples());

    for (int ch = 0; ch < numChannels; ++ch)
        output.copyFrom(ch, 0, source, ch, 0, numSamples);
}

juce::AudioProcessorEditor* E1MorphAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

bool E1MorphAudioProcessor::hasEditor() const
{
    return true;
}

void E1MorphAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ignoreUnused(destData);
}

void E1MorphAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::ignoreUnused(data, sizeInBytes);
}

E1MorphAudioProcessor::WorkerThread::WorkerThread(E1MorphAudioProcessor& ownerRef)
    : juce::Thread("OTMorphWorker")
    , owner(ownerRef)
{
}

E1MorphAudioProcessor::WorkerThread::~WorkerThread()
{
    stop();
}

void E1MorphAudioProcessor::WorkerThread::start(double sampleRate, int maxBlockSize)
{
    sampleRateHz = sampleRate;
    maxBlock = maxBlockSize;

    srcBands = Eigen::VectorXf::Zero(kCompressedBands);
    tgtBands = Eigen::VectorXf::Zero(kCompressedBands);
    baryBands = Eigen::VectorXf::Zero(kCompressedBands);
    costMatrix = Eigen::MatrixXf::Zero(kCompressedBands, kCompressedBands);
    kernelK = Eigen::MatrixXf::Ones(kCompressedBands, kCompressedBands);
    u = Eigen::VectorXf::Ones(kCompressedBands);
    v = Eigen::VectorXf::Ones(kCompressedBands);

    window.fill(1.0f);
    hannWindow.multiplyWithWindowingTable(window.data(), kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        windowSquared[static_cast<size_t>(i)] = window[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        std::fill(sourceInputRing[static_cast<size_t>(ch)].begin(), sourceInputRing[static_cast<size_t>(ch)].end(), 0.0f);
        std::fill(targetInputRing[static_cast<size_t>(ch)].begin(), targetInputRing[static_cast<size_t>(ch)].end(), 0.0f);
        std::fill(outputOlaRing[static_cast<size_t>(ch)].begin(), outputOlaRing[static_cast<size_t>(ch)].end(), 0.0f);
        std::fill(outputNormRing[static_cast<size_t>(ch)].begin(), outputNormRing[static_cast<size_t>(ch)].end(), 0.0f);
    }

    activeChannels = 0;
    inputWriteSample = 0;
    nextFrameStartSample = 0;
    outputReadSample = -kFftSize;

    if (!isThreadRunning())
        startThread(juce::Thread::Priority::normal);
}

void E1MorphAudioProcessor::WorkerThread::stop()
{
    signalThreadShouldExit();
    waitForThreadToExit(2000);
}

void E1MorphAudioProcessor::WorkerThread::setMorphAmount(float amount) noexcept
{
    morphAmount.store(juce::jlimit(0.0f, 1.0f, amount), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::run()
{
    while (!threadShouldExit())
    {
        AudioFrame in {};
        if (!owner.audioToWorkerFifo.pop(in))
        {
            wait(1);
            continue;
        }

        MorphResultFrame out {};
        out.sequence = in.sequence;
        out.numChannels = in.numChannels;
        out.numSamples = in.numSamples;

        processOneFrame(in, out);

        if (!owner.workerToAudioFifo.push(out))
        {
            MorphResultFrame throwAway {};
            owner.workerToAudioFifo.pop(throwAway);
            owner.workerToAudioFifo.push(out);
        }
    }
}

void E1MorphAudioProcessor::WorkerThread::processOneFrame(const AudioFrame& in, MorphResultFrame& out)
{
    if (in.numSamples <= 0 || in.numChannels <= 0)
        return;

    activeChannels = juce::jlimit(1, kMaxChannels, in.numChannels);

    appendInputSamples(in);
    renderAvailableStftFrames();
    popOutputSamples(activeChannels, in.numSamples, out);
}

void E1MorphAudioProcessor::WorkerThread::appendInputSamples(const AudioFrame& in)
{
    const int channels = activeChannels;
    const int samples = in.numSamples;

    for (int n = 0; n < samples; ++n)
    {
        const size_t ringPos = static_cast<size_t>(inputWriteSample & kRingMask);

        for (int ch = 0; ch < channels; ++ch)
        {
            sourceInputRing[static_cast<size_t>(ch)][ringPos] = in.source[static_cast<size_t>(ch)][static_cast<size_t>(n)];
            targetInputRing[static_cast<size_t>(ch)][ringPos] = in.target[static_cast<size_t>(ch)][static_cast<size_t>(n)];
        }

        ++inputWriteSample;
    }
}

void E1MorphAudioProcessor::WorkerThread::renderAvailableStftFrames()
{
    while (inputWriteSample >= (nextFrameStartSample + static_cast<uint64_t>(kFftSize)))
    {
        computeStftMagnitudes(nextFrameStartSample, activeChannels);

        compressSpectrumToBands();
        runSinkhornBarycenter();
        expandBandsToSpectrum();

        synthesizeUsingSourcePhase(nextFrameStartSample, activeChannels);

        nextFrameStartSample += static_cast<uint64_t>(kHopSize);
    }
}

void E1MorphAudioProcessor::WorkerThread::popOutputSamples(int numChannels, int numSamples, MorphResultFrame& out) noexcept
{
    for (int n = 0; n < numSamples; ++n)
    {
        const int64_t absoluteSample = outputReadSample++;

        if (absoluteSample < 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                out.output[static_cast<size_t>(ch)][static_cast<size_t>(n)] = 0.0f;
            continue;
        }

        const size_t ringPos = static_cast<size_t>(static_cast<uint64_t>(absoluteSample) & kRingMask);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float y = 0.0f;
            const float denom = outputNormRing[static_cast<size_t>(ch)][ringPos];

            if (denom > 1.0e-12f)
                y = outputOlaRing[static_cast<size_t>(ch)][ringPos] / denom;

            out.output[static_cast<size_t>(ch)][static_cast<size_t>(n)] = y;

            outputOlaRing[static_cast<size_t>(ch)][ringPos] = 0.0f;
            outputNormRing[static_cast<size_t>(ch)][ringPos] = 0.0f;
        }
    }
}

void E1MorphAudioProcessor::WorkerThread::computeStftMagnitudes(uint64_t frameStartSample, int numChannels)
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        for (int n = 0; n < kFftSize; ++n)
        {
            const size_t ringPos = static_cast<size_t>((frameStartSample + static_cast<uint64_t>(n)) & kRingMask);
            const float w = window[static_cast<size_t>(n)];

            srcTime[static_cast<size_t>(ch)][static_cast<size_t>(n)] = Cpx {
                sourceInputRing[static_cast<size_t>(ch)][ringPos] * w, 0.0f
            };
            tgtTime[static_cast<size_t>(ch)][static_cast<size_t>(n)] = Cpx {
                targetInputRing[static_cast<size_t>(ch)][ringPos] * w, 0.0f
            };
        }

        fft.perform(srcTime[static_cast<size_t>(ch)].data(), srcSpec[static_cast<size_t>(ch)].data(), false);
        fft.perform(tgtTime[static_cast<size_t>(ch)].data(), tgtSpec[static_cast<size_t>(ch)].data(), false);

        for (int k = 0; k < kFftBins; ++k)
        {
            const Cpx s = srcSpec[static_cast<size_t>(ch)][static_cast<size_t>(k)];
            const Cpx t = tgtSpec[static_cast<size_t>(ch)][static_cast<size_t>(k)];

            srcMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(k)] = std::abs(s);
            tgtMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(k)] = std::abs(t);
            srcPhase[static_cast<size_t>(ch)][static_cast<size_t>(k)] = std::atan2(s.imag(), s.real());
            morphedMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(k)] =
                srcMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(k)];
        }
    }
}

void E1MorphAudioProcessor::WorkerThread::compressSpectrumToBands()
{
    // TODO Step 3: Downsample FFT bins to Mel/Bark bands.
}

void E1MorphAudioProcessor::WorkerThread::runSinkhornBarycenter()
{
    // TODO Step 3: Run Sinkhorn iterations with Eigen.
}

void E1MorphAudioProcessor::WorkerThread::expandBandsToSpectrum()
{
    // TODO Step 3: Upsample morphed bands back to FFT bins.
}

void E1MorphAudioProcessor::WorkerThread::synthesizeUsingSourcePhase(uint64_t frameStartSample, int numChannels)
{
    constexpr float invFftSize = 1.0f / static_cast<float>(kFftSize);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& spec = resynthSpec[static_cast<size_t>(ch)];
        std::fill(spec.begin(), spec.end(), Cpx { 0.0f, 0.0f });

        spec[0] = Cpx { morphedMagnitude[static_cast<size_t>(ch)][0], 0.0f };

        for (int k = 1; k < (kFftSize / 2); ++k)
        {
            const float mag = morphedMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(k)];
            const float ph = srcPhase[static_cast<size_t>(ch)][static_cast<size_t>(k)];
            const Cpx c = std::polar(mag, ph);

            spec[static_cast<size_t>(k)] = c;
            spec[static_cast<size_t>(kFftSize - k)] = std::conj(c);
        }

        spec[static_cast<size_t>(kFftSize / 2)] =
            Cpx { morphedMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(kFftSize / 2)], 0.0f };

        fft.perform(spec.data(), ifftTime[static_cast<size_t>(ch)].data(), true);

        for (int n = 0; n < kFftSize; ++n)
        {
            const float timeSample = ifftTime[static_cast<size_t>(ch)][static_cast<size_t>(n)].real() * invFftSize;
            const float w = window[static_cast<size_t>(n)];
            const size_t ringPos = static_cast<size_t>((frameStartSample + static_cast<uint64_t>(n)) & kRingMask);

            outputOlaRing[static_cast<size_t>(ch)][ringPos] += timeSample * w;
            outputNormRing[static_cast<size_t>(ch)][ringPos] += windowSquared[static_cast<size_t>(n)];
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new E1MorphAudioProcessor();
}
