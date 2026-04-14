#include "PluginProcessor.h"

#include <cmath>

juce::AudioProcessorValueTreeState::ParameterLayout E1MorphAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "morph",
        "Morph",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.0f));
    return { params.begin(), params.end() };
}

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
    , apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{
    worker.startThread(juce::Thread::Priority::normal);
}

E1MorphAudioProcessor::~E1MorphAudioProcessor()
{
    worker.signalThreadShouldExit();
    worker.waitForThreadToExit(2000);
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
    currentSampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
    currentMaxBlockSize = juce::jmax(1, samplesPerBlock);

    const int analysisSynthesisLatency = kFftSize;
    const int queueSafety = juce::jmax(0, currentMaxBlockSize);
    setLatencySamples(analysisSynthesisLatency + queueSafety);

    worker.prepare(currentSampleRate, currentMaxBlockSize);
}

void E1MorphAudioProcessor::releaseResources()
{
    // Thread lifetime is managed by processor constructor/destructor.
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

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, numSamples);

    auto source = getBusBuffer(buffer, true, 0);
    auto output = getBusBuffer(buffer, false, 0);

    const bool hasSidechain = (getBusCount(true) > 1 && getChannelCountOfBus(true, 1) > 0);
    const auto sidechain = hasSidechain ? getBusBuffer(buffer, true, 1) : juce::AudioBuffer<float> {};

    bool targetIsSilent = !hasSidechain;
    if (hasSidechain && sidechain.getNumChannels() > 0 && sidechain.getNumSamples() > 0)
    {
        float maxMagnitude = 0.0f;
        for (int ch = 0; ch < sidechain.getNumChannels(); ++ch)
            maxMagnitude = juce::jmax(maxMagnitude, sidechain.getMagnitude(ch, 0, sidechain.getNumSamples()));

        targetIsSilent = (maxMagnitude <= 1.0e-4f);
    }

    const juce::AudioBuffer<float>& target = (hasSidechain && !targetIsSilent) ? sidechain : source;

    if (auto* morphParam = apvts.getRawParameterValue("morph"))
        worker.setMorphAmount(morphParam->load(std::memory_order_relaxed));

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
    srcBands.setZero(kCompressedBands);
    tgtBands.setZero(kCompressedBands);
    baryBands.setZero(kCompressedBands);
    costMatrix.setZero(kCompressedBands, kCompressedBands);
    kernelK.setZero(kCompressedBands, kCompressedBands);
    transportPlan.setZero(kCompressedBands, kCompressedBands);
    u.setOnes(kCompressedBands);
    v.setOnes(kCompressedBands);
    tmpKv.setZero(kCompressedBands);
    tmpKTu.setZero(kCompressedBands);

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        srcBandsPerChannel[static_cast<size_t>(ch)].setConstant(kCompressedBands, kDistributionFloor);
        tgtBandsPerChannel[static_cast<size_t>(ch)].setConstant(kCompressedBands, kDistributionFloor);
        baryBandsPerChannel[static_cast<size_t>(ch)].setConstant(kCompressedBands, kDistributionFloor);
    }

    for (int i = 0; i < kCompressedBands; ++i)
    {
        for (int j = 0; j < kCompressedBands; ++j)
        {
            const float d = static_cast<float>(i - j);
            const float dist2 = d * d;
            costMatrix(i, j) = dist2;
            kernelK(i, j) = std::exp(-dist2 / kSinkhornEpsilon);
        }
    }

    window.fill(1.0f);
    hannWindow.multiplyWithWindowingTable(window.data(), kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        windowSquared[static_cast<size_t>(i)] = window[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];

    resetStreamingState(sampleRateHz, maxBlock);
}

E1MorphAudioProcessor::WorkerThread::~WorkerThread()
{
}

void E1MorphAudioProcessor::WorkerThread::prepare(double sampleRate, int maxBlockSize) noexcept
{
    pendingSampleRateHz.store((sampleRate > 0.0) ? sampleRate : 44100.0, std::memory_order_relaxed);
    pendingMaxBlock.store(juce::jmax(1, maxBlockSize), std::memory_order_relaxed);
    prepareRequested.store(true, std::memory_order_release);
}

void E1MorphAudioProcessor::WorkerThread::setMorphAmount(float amount) noexcept
{
    morphAmount.store(juce::jlimit(0.0f, 1.0f, amount), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::run()
{
    while (!threadShouldExit())
    {
        if (prepareRequested.exchange(false, std::memory_order_acq_rel))
        {
            const auto newSampleRate = pendingSampleRateHz.load(std::memory_order_relaxed);
            const auto newMaxBlock = pendingMaxBlock.load(std::memory_order_relaxed);
            resetStreamingState(newSampleRate, newMaxBlock);
        }

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

void E1MorphAudioProcessor::WorkerThread::resetStreamingState(double sampleRate, int maxBlockSize) noexcept
{
    sampleRateHz = (sampleRate > 0.0) ? sampleRate : 44100.0;
    maxBlock = juce::jmax(1, maxBlockSize);

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

    AudioFrame throwAwayIn {};
    while (owner.audioToWorkerFifo.pop(throwAwayIn))
    {
    }

    MorphResultFrame throwAwayOut {};
    while (owner.workerToAudioFifo.pop(throwAwayOut))
    {
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
    const float morph = juce::jlimit(0.0f, 1.0f, morphAmount.load(std::memory_order_relaxed));

    while (inputWriteSample >= (nextFrameStartSample + static_cast<uint64_t>(kFftSize)))
    {
        computeStftMagnitudes(nextFrameStartSample, activeChannels);

        if (morph > 0.0f)
        {
            compressSpectrumToBands();
            runSinkhornBarycenter(morph);
            expandBandsToSpectrum();
        }

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
    for (int ch = 0; ch < activeChannels; ++ch)
    {
        auto& srcBand = srcBandsPerChannel[static_cast<size_t>(ch)];
        auto& tgtBand = tgtBandsPerChannel[static_cast<size_t>(ch)];

        srcBand.setConstant(kDistributionFloor);
        tgtBand.setConstant(kDistributionFloor);

        for (int band = 0; band < kCompressedBands; ++band)
        {
            const int start = (band * kFftBins) / kCompressedBands;
            const int end = ((band + 1) * kFftBins) / kCompressedBands;
            const int clampedEnd = juce::jmax(start + 1, end);
            const int count = clampedEnd - start;

            float srcAccum = 0.0f;
            float tgtAccum = 0.0f;
            for (int bin = start; bin < clampedEnd; ++bin)
            {
                srcAccum += srcMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(bin)];
                tgtAccum += tgtMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(bin)];
            }

            srcBand(band) = juce::jmax(kDistributionFloor, srcAccum / static_cast<float>(count));
            tgtBand(band) = juce::jmax(kDistributionFloor, tgtAccum / static_cast<float>(count));
        }

        const float srcSum = juce::jmax(kDistributionFloor, srcBand.sum());
        float tgtSum = tgtBand.sum();
        if (tgtSum <= kDistributionFloor)
        {
            tgtBand.setConstant(kDistributionFloor);
            tgtSum = tgtBand.sum();
        }

        const float scale = srcSum / juce::jmax(kDistributionFloor, tgtSum);
        tgtBand *= scale;
    }
}

void E1MorphAudioProcessor::WorkerThread::runSinkhornBarycenter(float morph)
{
    if (morph <= 0.0f)
    {
        for (int ch = 0; ch < activeChannels; ++ch)
            baryBandsPerChannel[static_cast<size_t>(ch)] = srcBandsPerChannel[static_cast<size_t>(ch)];
        return;
    }

    for (int ch = 0; ch < activeChannels; ++ch)
    {
        srcBands = srcBandsPerChannel[static_cast<size_t>(ch)];
        tgtBands = tgtBandsPerChannel[static_cast<size_t>(ch)];

        u.setOnes();
        v.setOnes();

        for (int iter = 0; iter < kSinkhornIterations; ++iter)
        {
            tmpKv.noalias() = kernelK * v;
            u = srcBands.array() / tmpKv.array().max(kDistributionFloor);

            tmpKTu.noalias() = kernelK.transpose() * u;
            v = tgtBands.array() / tmpKTu.array().max(kDistributionFloor);
        }

        transportPlan.setZero();
        for (int i = 0; i < kCompressedBands; ++i)
        {
            const float ui = u(i);
            for (int j = 0; j < kCompressedBands; ++j)
                transportPlan(i, j) = ui * kernelK(i, j) * v(j);
        }

        baryBands.setZero();
        for (int i = 0; i < kCompressedBands; ++i)
        {
            for (int j = 0; j < kCompressedBands; ++j)
            {
                const float mass = transportPlan(i, j);
                const float targetIdx = ((1.0f - morph) * static_cast<float>(i)) + (morph * static_cast<float>(j));
                const int k = juce::jlimit(0, kCompressedBands - 1, static_cast<int>(std::lround(targetIdx)));
                baryBands(k) += mass;
            }
        }

        baryBands = baryBands.array().max(kDistributionFloor);

        const float srcSum = juce::jmax(kDistributionFloor, srcBands.sum());
        const float barySum = juce::jmax(kDistributionFloor, baryBands.sum());
        baryBands *= (srcSum / barySum);

        baryBandsPerChannel[static_cast<size_t>(ch)] = baryBands;
    }
}

void E1MorphAudioProcessor::WorkerThread::expandBandsToSpectrum()
{
    constexpr float denom = static_cast<float>(kFftBins - 1);
    constexpr float bandMax = static_cast<float>(kCompressedBands - 1);

    for (int ch = 0; ch < activeChannels; ++ch)
    {
        const auto& baryBand = baryBandsPerChannel[static_cast<size_t>(ch)];
        const auto& srcBand = srcBandsPerChannel[static_cast<size_t>(ch)];

        for (int bin = 0; bin < kFftBins; ++bin)
        {
            const float pos = (static_cast<float>(bin) / denom) * bandMax;
            const int i0 = juce::jlimit(0, kCompressedBands - 1, static_cast<int>(std::floor(pos)));
            const int i1 = juce::jmin(kCompressedBands - 1, i0 + 1);
            const float frac = pos - static_cast<float>(i0);

            const float baryV0 = baryBand(i0);
            const float baryV1 = baryBand(i1);
            const float srcV0 = srcBand(i0);
            const float srcV1 = srcBand(i1);

            const float interpolatedBary = ((1.0f - frac) * baryV0) + (frac * baryV1);
            const float interpolatedSrc = ((1.0f - frac) * srcV0) + (frac * srcV1);
            const float ratio = interpolatedBary / juce::jmax(kDistributionFloor, interpolatedSrc);

            morphedMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(bin)] =
                srcMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(bin)] * ratio;
        }
    }
}

void E1MorphAudioProcessor::WorkerThread::synthesizeUsingSourcePhase(uint64_t frameStartSample, int numChannels)
{
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
            const float timeSample = ifftTime[static_cast<size_t>(ch)][static_cast<size_t>(n)].real();
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
