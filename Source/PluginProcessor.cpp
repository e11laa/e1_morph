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
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "focus",
        "Sinkhorn Focus",
        juce::NormalisableRange<float>(0.5f, 2.0f, 0.001f),
        1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "glide",
        "Spectral Glide",
        juce::NormalisableRange<float>(0.0f, 0.99f, 0.001f),
        0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "envMod",
        "Envelope Mod",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.001f),
        0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "envFollowMode",
        "Env Follow Mode",
        juce::StringArray { "Modulation", "Shaping" },
        0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "envFollowSource",
        "Env Follow Source",
        juce::StringArray { "Source", "Sidechain" },
        1));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "outGain",
        "Out Gain (dB)",
        juce::NormalisableRange<float>(-36.0f, 12.0f, 0.01f),
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
    const juce::AudioBuffer<float>& target = hasSidechain ? sidechain : source;

    float morph = 0.0f;
    if (auto* morphParam = apvts.getRawParameterValue("morph"))
        morph = morphParam->load(std::memory_order_relaxed);
    worker.setMorphAmount(morph);

    float focus = 1.0f;
    if (auto* focusParam = apvts.getRawParameterValue("focus"))
        focus = focusParam->load(std::memory_order_relaxed);
    worker.setFocusAmount(focus);

    float glide = 0.0f;
    if (auto* glideParam = apvts.getRawParameterValue("glide"))
        glide = glideParam->load(std::memory_order_relaxed);
    worker.setGlideAmount(glide);

    float envMod = 0.0f;
    if (auto* envModParam = apvts.getRawParameterValue("envMod"))
        envMod = envModParam->load(std::memory_order_relaxed);
    worker.setEnvModAmount(envMod);

    int envFollowMode = 0;
    if (auto* envFollowModeParam = apvts.getRawParameterValue("envFollowMode"))
        envFollowMode = juce::jlimit(0, 1, static_cast<int>(std::lround(envFollowModeParam->load(std::memory_order_relaxed))));
    worker.setEnvFollowMode(envFollowMode);

    int envFollowSource = 1;
    if (auto* envFollowSourceParam = apvts.getRawParameterValue("envFollowSource"))
        envFollowSource = juce::jlimit(0, 1, static_cast<int>(std::lround(envFollowSourceParam->load(std::memory_order_relaxed))));
    worker.setEnvFollowSource(envFollowSource);

    worker.setSidechainActive(hasSidechain);

    pushFrameToWorker(source, target);

    MorphResultFrame processed {};
    if (popProcessedFrame(processed))
        copyProcessedToOutput(processed, output);
    else
        copySourceToOutput(source, output);

    float outGainDb = 0.0f;
    if (auto* outGainParam = apvts.getRawParameterValue("outGain"))
        outGainDb = outGainParam->load(std::memory_order_relaxed);

    const float outGainLin = juce::Decibels::decibelsToGain(outGainDb);
    for (int ch = 0; ch < output.getNumChannels(); ++ch)
    {
        float* out = output.getWritePointer(ch);
        for (int n = 0; n < numSamples; ++n)
        {
            out[n] *= outGainLin;
            out[n] = juce::jlimit(-1.0f, 1.0f, out[n]);
        }
    }
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

void E1MorphAudioProcessor::WorkerThread::setFocusAmount(float amount) noexcept
{
    focusAmount.store(juce::jlimit(0.5f, 2.0f, amount), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::setGlideAmount(float amount) noexcept
{
    glideAmount.store(juce::jlimit(0.0f, 0.99f, amount), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::setEnvModAmount(float amount) noexcept
{
    envModAmount.store(juce::jlimit(-1.0f, 1.0f, amount), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::setEnvFollowMode(int mode) noexcept
{
    envFollowMode.store(juce::jlimit(0, 1, mode), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::setEnvFollowSource(int source) noexcept
{
    envFollowSource.store(juce::jlimit(0, 1, source), std::memory_order_relaxed);
}

void E1MorphAudioProcessor::WorkerThread::setSidechainActive(bool isActive) noexcept
{
    sidechainActive.store(isActive, std::memory_order_relaxed);
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
        std::fill(prevMorphedMagnitude[static_cast<size_t>(ch)].begin(), prevMorphedMagnitude[static_cast<size_t>(ch)].end(), 0.0f);
    }

    activeChannels = 0;
    inputWriteSample = 0;
    nextFrameStartSample = 0;
    outputReadSample = -kFftSize;
    smoothedMorph = morphAmount.load(std::memory_order_relaxed);
    smoothedFocus = focusAmount.load(std::memory_order_relaxed);
    smoothedGlide = glideAmount.load(std::memory_order_relaxed);
    envelopeFollower = 0.0f;
    envelopeReference = 1.0f;
    smoothedShapingGain = 1.0f;
    sidechainActive.store(false, std::memory_order_relaxed);

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
    constexpr float kParamSmooth = 0.2f;
    constexpr float kEnvAttack = 0.3f;
    constexpr float kEnvRelease = 0.06f;
    constexpr float kGainAttack = 0.35f;
    constexpr float kGainRelease = 0.08f;

    const float morphTarget = juce::jlimit(0.0f, 1.0f, morphAmount.load(std::memory_order_relaxed));
    const float focusTarget = juce::jlimit(0.5f, 2.0f, focusAmount.load(std::memory_order_relaxed));
    const float glideTarget = juce::jlimit(0.0f, 0.99f, glideAmount.load(std::memory_order_relaxed));
    const float envMod = juce::jlimit(-1.0f, 1.0f, envModAmount.load(std::memory_order_relaxed));
    const int mode = juce::jlimit(0, 1, envFollowMode.load(std::memory_order_relaxed));
    const int sourceSel = juce::jlimit(0, 1, envFollowSource.load(std::memory_order_relaxed));

    smoothedMorph += kParamSmooth * (morphTarget - smoothedMorph);
    smoothedFocus += kParamSmooth * (focusTarget - smoothedFocus);
    smoothedGlide += kParamSmooth * (glideTarget - smoothedGlide);

    while (inputWriteSample >= (nextFrameStartSample + static_cast<uint64_t>(kFftSize)))
    {
        computeStftMagnitudes(nextFrameStartSample, activeChannels);

        const bool useSourceForEnvelope = (sourceSel == 0) || !sidechainActive.load(std::memory_order_relaxed);
        const float envInstant = computeAnalysisEnvelopeFromMagnitudes(activeChannels, useSourceForEnvelope);
        const float coeff = (envInstant > envelopeFollower) ? kEnvAttack : kEnvRelease;
        envelopeFollower += coeff * (envInstant - envelopeFollower);

        envelopeFollower = juce::jlimit(0.0f, 1.0f, envelopeFollower);

        const float mod = (mode == 0) ? (envMod * envelopeFollower) : 0.0f;
        const float effectiveMorph = juce::jlimit(0.0f, 1.0f, smoothedMorph + mod);
        const float effectiveFocus = juce::jlimit(0.5f, 2.0f, smoothedFocus + (0.5f * mod));

        if (effectiveMorph > 0.0f)
        {
            compressSpectrumToBands();
            runSinkhornBarycenter(effectiveMorph, effectiveFocus);
            expandBandsToSpectrum(smoothedGlide);
        }
        else
        {
            for (int ch = 0; ch < activeChannels; ++ch)
            {
                std::copy(srcMagnitude[static_cast<size_t>(ch)].begin(),
                          srcMagnitude[static_cast<size_t>(ch)].end(),
                          prevMorphedMagnitude[static_cast<size_t>(ch)].begin());
            }
        }

        if (mode == 1)
        {
            float analysisEnergy = 0.0f;
            float morphedEnergy = 0.0f;

            for (int ch = 0; ch < activeChannels; ++ch)
            {
                const auto& analysis = useSourceForEnvelope
                                           ? srcMagnitude[static_cast<size_t>(ch)]
                                           : tgtMagnitude[static_cast<size_t>(ch)];

                const Eigen::Map<const Eigen::ArrayXf> analysisArray(analysis.data(), kFftBins);
                const Eigen::Map<const Eigen::ArrayXf> morphedArray(morphedMagnitude[static_cast<size_t>(ch)].data(), kFftBins);
                analysisEnergy += analysisArray.square().sum();
                morphedEnergy += morphedArray.square().sum();
            }

            const float targetGain = std::sqrt(analysisEnergy / juce::jmax(kDistributionFloor, morphedEnergy));
            const float gainCoeff = (targetGain > smoothedShapingGain) ? kGainAttack : kGainRelease;
            smoothedShapingGain += gainCoeff * (targetGain - smoothedShapingGain);
            smoothedShapingGain = juce::jmax(0.0f, smoothedShapingGain);

            for (int ch = 0; ch < activeChannels; ++ch)
            {
                Eigen::Map<Eigen::ArrayXf> morphedArray(morphedMagnitude[static_cast<size_t>(ch)].data(), kFftBins);
                morphedArray *= smoothedShapingGain;
            }
        }
        else
        {
            smoothedShapingGain += 0.1f * (1.0f - smoothedShapingGain);
        }

        synthesizeUsingSourcePhase(nextFrameStartSample, activeChannels);

        nextFrameStartSample += static_cast<uint64_t>(kHopSize);
    }
}

float E1MorphAudioProcessor::WorkerThread::computeAnalysisEnvelopeFromMagnitudes(int numChannels,
                                                                                 bool useSourceForEnvelope) noexcept
{
    if (numChannels <= 0)
        return 0.0f;

    float averageMagnitude = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto& analysis = useSourceForEnvelope
                                   ? srcMagnitude[static_cast<size_t>(ch)]
                                   : tgtMagnitude[static_cast<size_t>(ch)];
        const Eigen::Map<const Eigen::ArrayXf> analysisArray(analysis.data(), kFftBins);
        averageMagnitude += analysisArray.sum();
    }

    averageMagnitude /= static_cast<float>(numChannels * kFftBins);
    envelopeReference = juce::jmax(kDistributionFloor, juce::jmax(averageMagnitude, envelopeReference * 0.995f));

    const float normalized = averageMagnitude / juce::jmax(kDistributionFloor, envelopeReference);
    return juce::jlimit(0.0f, 1.0f, normalized);
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

void E1MorphAudioProcessor::WorkerThread::runSinkhornBarycenter(float morph, float focus)
{
    if (morph <= 0.0f)
    {
        for (int ch = 0; ch < activeChannels; ++ch)
            baryBandsPerChannel[static_cast<size_t>(ch)] = srcBandsPerChannel[static_cast<size_t>(ch)];
        return;
    }

    const float clampedFocus = juce::jlimit(0.5f, 2.0f, focus);

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
                const float mass = juce::jmax(0.0f, transportPlan(i, j));
                const float focusedMass = std::pow(mass + kDistributionFloor, clampedFocus);
                const float targetIdx = ((1.0f - morph) * static_cast<float>(i)) + (morph * static_cast<float>(j));
                const int k = juce::jlimit(0, kCompressedBands - 1, static_cast<int>(std::lround(targetIdx)));
                baryBands(k) += focusedMass;
            }
        }

        baryBands = baryBands.array().max(kDistributionFloor);

        const float srcSum = juce::jmax(kDistributionFloor, srcBands.sum());
        const float barySum = juce::jmax(kDistributionFloor, baryBands.sum());
        baryBands *= (srcSum / barySum);

        baryBandsPerChannel[static_cast<size_t>(ch)] = baryBands;
    }
}

void E1MorphAudioProcessor::WorkerThread::expandBandsToSpectrum(float glide)
{
    constexpr float denom = static_cast<float>(kFftBins - 1);
    constexpr float bandMax = static_cast<float>(kCompressedBands - 1);
    const float clampedGlide = juce::jlimit(0.0f, 0.99f, glide);

    for (int ch = 0; ch < activeChannels; ++ch)
    {
        const auto& baryBand = baryBandsPerChannel[static_cast<size_t>(ch)];
        const auto& srcBand = srcBandsPerChannel[static_cast<size_t>(ch)];
        auto& prevBand = prevMorphedMagnitude[static_cast<size_t>(ch)];

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
            const float currentMag = juce::jmax(0.0f,
                                                srcMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(bin)] * ratio);
            const float smoothedMag = (clampedGlide * prevBand[static_cast<size_t>(bin)])
                                      + ((1.0f - clampedGlide) * currentMag);
            prevBand[static_cast<size_t>(bin)] = smoothedMag;

            morphedMagnitude[static_cast<size_t>(ch)][static_cast<size_t>(bin)] = smoothedMag;
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
