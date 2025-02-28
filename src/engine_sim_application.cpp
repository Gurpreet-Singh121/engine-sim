#include "../include/engine_sim_application.h"

#include "../include/piston_object.h"
#include "../include/connecting_rod_object.h"
#include "../include/constants.h"
#include "../include/units.h"
#include "../include/crankshaft_object.h"
#include "../include/cylinder_bank_object.h"
#include "../include/cylinder_head_object.h"
#include "../include/ui_button.h"
#include "../include/combustion_chamber_object.h"
#include "../include/csv_io.h"
#include "../include/exhaust_system.h"
#include "../include/feedback_comb_filter.h"
#include "../include/utilities.h"

#include "../scripting/include/compiler.h"

#include <chrono>
#include <stdlib.h>
#include <sstream>

EngineSimApplication::EngineSimApplication() {
    m_assetPath = "";

    m_geometryVertexBuffer = nullptr;
    m_geometryIndexBuffer = nullptr;

    m_paused = false;
    m_recording = false;
    m_screenResolutionIndex = 0;
    for (int i = 0; i < ScreenResolutionHistoryLength; ++i) {
        m_screenResolution[i][0] = m_screenResolution[i][1] = 0;
    }

    m_background = ysColor::srgbiToLinear(0x0E1012);
    m_foreground = ysColor::srgbiToLinear(0xFFFFFF);
    m_shadow = ysColor::srgbiToLinear(0x0E1012);
    m_highlight1 = ysColor::srgbiToLinear(0xEF4545);
    m_highlight2 = ysColor::srgbiToLinear(0xFFFFFF);
    m_pink = ysColor::srgbiToLinear(0xF394BE);
    m_red = ysColor::srgbiToLinear(0xEE4445);
    m_orange = ysColor::srgbiToLinear(0xF4802A);
    m_yellow = ysColor::srgbiToLinear(0xFDBD2E);
    m_blue = ysColor::srgbiToLinear(0x77CEE0);
    m_green = ysColor::srgbiToLinear(0xBDD869);

    m_displayHeight = (float)units::distance(2.0, units::foot);
    m_outputAudioBuffer = nullptr;
    m_audioSource = nullptr;

    m_torque = 0;
    m_dynoSpeed = 0;

    m_engineView = nullptr;
    m_rightGaugeCluster = nullptr;
    m_temperatureGauge = nullptr;
    m_oscCluster = nullptr;
    m_performanceCluster = nullptr;
    m_loadSimulationCluster = nullptr;
    m_mixerCluster = nullptr;
    m_infoCluster = nullptr;
    m_iceEngine = nullptr;

    m_mainRenderTarget = nullptr;

    m_oscillatorSampleOffset = 0;
    m_gameWindowHeight = 256;
    m_screenWidth = 256;
    m_screenHeight = 256;
    m_screen = 0;
    m_viewParameters.Layer0 = 0;
    m_viewParameters.Layer1 = 10;

    buildVersion = "0.1.4a";
}

EngineSimApplication::~EngineSimApplication() {
    /* void */
}

void EngineSimApplication::initialize(void *instance, ysContextObject::DeviceAPI api) {
    dbasic::Path modulePath = dbasic::GetModulePath();
    dbasic::Path confPath = modulePath.Append("delta.conf");

    std::string enginePath = "../dependencies/submodules/delta-studio/engines/basic";
    m_assetPath = "../assets";
    if (confPath.Exists()) {
        std::fstream confFile(confPath.ToString(), std::ios::in);

        std::getline(confFile, enginePath);
        std::getline(confFile, m_assetPath);
        enginePath = modulePath.Append(enginePath).ToString();
        m_assetPath = modulePath.Append(m_assetPath).ToString();

        confFile.close();
    }

    m_engine.GetConsole()->SetDefaultFontDirectory(enginePath + "/fonts/");

    const std::string shaderPath = enginePath + "/shaders/";
    std::string winTitle = "Engine Sim | AngeTheGreat | v" + buildVersion;
    dbasic::DeltaEngine::GameEngineSettings settings;
    settings.API = api;
    settings.DepthBuffer = false;
    settings.Instance = instance;
    settings.ShaderDirectory = shaderPath.c_str();
    settings.WindowTitle = winTitle.c_str();
    settings.WindowPositionX = 0;
    settings.WindowPositionY = 0;
    settings.WindowStyle = ysWindow::WindowStyle::Windowed;
    settings.WindowWidth = 1920;
    settings.WindowHeight = 1081;

    m_engine.CreateGameWindow(settings);

    m_engine.GetDevice()->CreateSubRenderTarget(
        &m_mainRenderTarget,
        m_engine.GetScreenRenderTarget(),
        0,
        0,
        0,
        0);

    m_engine.InitializeShaderSet(&m_shaderSet);
    m_shaders.Initialize(
        &m_shaderSet,
        m_mainRenderTarget,
        m_engine.GetScreenRenderTarget(),
        m_engine.GetDefaultShaderProgram(),
        m_engine.GetDefaultInputLayout());
    m_engine.InitializeConsoleShaders(&m_shaderSet);
    m_engine.SetShaderSet(&m_shaderSet);

    m_shaders.SetClearColor(ysColor::srgbiToLinear(0x34, 0x98, 0xdb));

    m_assetManager.SetEngine(&m_engine);

    m_engine.GetDevice()->CreateIndexBuffer(
        &m_geometryIndexBuffer, sizeof(unsigned short) * 200000, nullptr);
    m_engine.GetDevice()->CreateVertexBuffer(
        &m_geometryVertexBuffer, sizeof(dbasic::Vertex) * 100000, nullptr);

    m_geometryGenerator.initialize(100000, 200000);

    initialize();
}

void EngineSimApplication::initialize() {
    m_shaders.SetClearColor(ysColor::srgbiToLinear(0x34, 0x98, 0xdb));
    m_assetManager.CompileInterchangeFile((m_assetPath + "/assets").c_str(), 1.0f, true);
    m_assetManager.LoadSceneFile((m_assetPath + "/assets").c_str(), true);

    m_textRenderer.SetEngine(&m_engine);
    m_textRenderer.SetRenderer(m_engine.GetUiRenderer());
    m_textRenderer.SetFont(m_engine.GetConsole()->GetFont());

    // Scripting
#ifdef ATG_ENGINE_PIRANHA_ENABLED

    es_script::Compiler compiler;
    compiler.initialize();
    const bool compiled = compiler.compile("../assets/main.mr");
    if (compiled) {
        const es_script::Compiler::Output output = compiler.execute();
        m_iceEngine = output.engine;
    }
    else {
        m_iceEngine = nullptr;
    }

    compiler.destroy();

#endif /* PIRANHA_ENABLED */

    Vehicle::Parameters vehParams;
    vehParams.Mass = units::mass(1597, units::kg);
    vehParams.DiffRatio = 3.42;
    vehParams.TireRadius = units::distance(10, units::inch);
    vehParams.DragCoefficient = 0.25;
    vehParams.CrossSectionArea = units::distance(6.0, units::foot) * units::distance(6.0, units::foot);
    vehParams.RollingResistance = 2000.0;
    Vehicle *vehicle = new Vehicle;
    vehicle->initialize(vehParams);

    const double gearRatios[] = { 2.97, 2.07, 1.43, 1.00, 0.84, 0.56 };
    Transmission::Parameters tParams;
    tParams.GearCount = 6;
    tParams.GearRatios = gearRatios;
    tParams.MaxClutchTorque = units::torque(1000.0, units::ft_lb);
    Transmission *transmission = new Transmission;
    transmission->initialize(tParams);

    Simulator::Parameters simulatorParams;
    simulatorParams.Engine = m_iceEngine;
    simulatorParams.SystemType = Simulator::SystemType::NsvOptimized;
    simulatorParams.Transmission = transmission;
    simulatorParams.Vehicle = vehicle;
    simulatorParams.SimulationFrequency = 10000;
    simulatorParams.FluidSimulationSteps = 8;
    m_simulator.initialize(simulatorParams);
    m_simulator.startAudioRenderingThread();
    createObjects(m_iceEngine);

    for (int i = 0; i < m_iceEngine->getExhaustSystemCount(); ++i) {
        ImpulseResponse *response = m_iceEngine->getExhaustSystem(i)->getImpulseResponse();

        ysWindowsAudioWaveFile waveFile;
        waveFile.OpenFile(response->getFilename().c_str());
        waveFile.InitializeInternalBuffer(waveFile.GetSampleCount());
        waveFile.FillBuffer(0);
        waveFile.CloseFile();

        m_simulator.getSynthesizer()->initializeImpulseResponse(
            reinterpret_cast<const int16_t *>(waveFile.GetBuffer()),
            waveFile.GetSampleCount(),
            response->getVolume(),
            i
        );

        waveFile.DestroyInternalBuffer();
    }

    m_uiManager.initialize(this);

    m_engineView = m_uiManager.getRoot()->addElement<EngineView>();
    m_rightGaugeCluster = m_uiManager.getRoot()->addElement<RightGaugeCluster>();
    m_rightGaugeCluster->m_engine = m_iceEngine;
    m_rightGaugeCluster->m_simulator = &m_simulator;

    m_oscCluster = m_uiManager.getRoot()->addElement<OscilloscopeCluster>();
    m_oscCluster->m_simulator = &m_simulator;
    m_oscCluster->setDynoMaxRange(units::toRpm(m_iceEngine->getRedline()));

    m_performanceCluster = m_uiManager.getRoot()->addElement<PerformanceCluster>();
    m_performanceCluster->setSimulator(&m_simulator);

    m_loadSimulationCluster = m_uiManager.getRoot()->addElement<LoadSimulationCluster>();
    m_loadSimulationCluster->setSimulator(&m_simulator);

    m_mixerCluster = m_uiManager.getRoot()->addElement<MixerCluster>();
    m_mixerCluster->setSimulator(&m_simulator);

    m_infoCluster = m_uiManager.getRoot()->addElement<InfoCluster>();
    m_infoCluster->setEngine(m_iceEngine);

    m_audioBuffer.initialize(44100, 44100);
    m_audioBuffer.m_writePointer = (int)(44100 * 0.1);

    ysAudioParameters params;
    params.m_bitsPerSample = 16;
    params.m_channelCount = 1;
    params.m_sampleRate = 44100;
    m_outputAudioBuffer =
        m_engine.GetAudioDevice()->CreateBuffer(&params, 44100);

    m_audioSource = m_engine.GetAudioDevice()->CreateSource(m_outputAudioBuffer);
    m_audioSource->SetMode(ysAudioSource::Mode::Loop);
    m_audioSource->SetPan(0.0f);
    m_audioSource->SetVolume(1.0f);
}

void EngineSimApplication::process(float frame_dt) {
    frame_dt = clamp(frame_dt, 1 / 100.0f, 1 / 30.0f);

    double speed = 1.0 / 1.0;
    if (m_engine.IsKeyDown(ysKey::Code::N1)) {
        speed = 1 / 10.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N2)) {
        speed = 1 / 100.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N3)) {
        speed = 1 / 200.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N4)) {
        speed = 1 / 500.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N5)) {
        speed = 1 / 1000.0;
    }

    m_simulator.setSimulationSpeed(speed);

    m_simulator.startFrame(frame_dt);

    auto proc_t0 = std::chrono::steady_clock::now();
    const int iterationCount = m_simulator.getFrameIterationCount();
    while (m_simulator.simulateStep()) {
        const double cylinderPressure = m_simulator.getEngine()->getChamber(0)->m_system.pressure()
            + m_simulator.getEngine()->getChamber(0)->m_system.dynamicPressure(-1.0, 0.0);

        if (m_simulator.getCurrentIteration() % 2 == 0) {
            m_oscCluster->getTotalExhaustFlowOscilloscope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(),
                m_simulator.getTotalExhaustFlow() / m_simulator.getTimestep());
            m_oscCluster->getCylinderPressureScope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(constants::pi),
                std::sqrt(cylinderPressure));
            m_oscCluster->getExhaustFlowOscilloscope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(),
                m_simulator.getEngine()->getChamber(0)->getLastTimestepExhaustFlow() / m_simulator.getTimestep());
            m_oscCluster->getIntakeFlowOscilloscope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(),
                m_simulator.getEngine()->getChamber(0)->getLastTimestepIntakeFlow() / m_simulator.getTimestep());
            m_oscCluster->getCylinderMoleculesScope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(),
                m_simulator.getEngine()->getChamber(0)->m_system.n());
            m_oscCluster->getExhaustValveLiftOscilloscope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(),
                m_simulator.getEngine()->getChamber(0)->getCylinderHead()->exhaustValveLift(
                    m_simulator.getEngine()->getChamber(0)->getPiston()->getCylinderIndex()));
            m_oscCluster->getIntakeValveLiftOscilloscope()->addDataPoint(
                m_simulator.getEngine()->getCrankshaft(0)->getCycleAngle(),
                m_simulator.getEngine()->getChamber(0)->getCylinderHead()->intakeValveLift(
                    m_simulator.getEngine()->getChamber(0)->getPiston()->getCylinderIndex()));
            m_oscCluster->getPvScope()->addDataPoint(
                m_simulator.getEngine()->getChamber(0)->getVolume(),
                std::sqrt(m_simulator.getEngine()->getChamber(0)->m_system.pressure()));
        }
    }

    auto proc_t1 = std::chrono::steady_clock::now();

    m_simulator.endFrame();

    auto duration = proc_t1 - proc_t0;
    if (iterationCount > 0) {
        m_performanceCluster->addTimePerTimestepSample(
            (duration.count() / 1E9) / iterationCount);
    }

    const SampleOffset currentAudioPosition = m_audioSource->GetCurrentPosition();
    const SampleOffset safeWritePosition = m_audioSource->GetCurrentWritePosition();
    const SampleOffset writePosition = m_audioBuffer.m_writePointer;

    SampleOffset targetWritePosition =
        m_audioBuffer.getBufferIndex(currentAudioPosition, (int)(44100 * 0.1));
    SampleOffset maxWrite = m_audioBuffer.offsetDelta(writePosition, targetWritePosition);

    SampleOffset currentLead = m_audioBuffer.offsetDelta(currentAudioPosition, writePosition);
    SampleOffset newLead = m_audioBuffer.offsetDelta(currentAudioPosition, targetWritePosition);

    if (currentLead > newLead) {
        maxWrite = 0;
    }

    int16_t *samples = new int16_t[maxWrite];
    const int readSamples = m_simulator.readAudioOutput(maxWrite, samples);

    for (SampleOffset i = 0; i < (SampleOffset)readSamples && i < maxWrite; ++i) {
        const int16_t sample = samples[i];
        if (m_oscillatorSampleOffset % 4 == 0) {
            m_oscCluster->getAudioWaveformOscilloscope()->addDataPoint(
                m_oscillatorSampleOffset,
                sample / (float)(INT16_MAX));
        }

        m_audioBuffer.writeSample(sample, m_audioBuffer.m_writePointer, (int)i);

        m_oscillatorSampleOffset = (m_oscillatorSampleOffset + 1) % (44100 / 10);
    }

    delete[] samples;

    if (readSamples > 0) {
        SampleOffset size0, size1;
        void *data0, *data1;
        m_audioSource->LockBufferSegment(
            m_audioBuffer.m_writePointer, readSamples, &data0, &size0, &data1, &size1);

        m_audioBuffer.copyBuffer(
            reinterpret_cast<int16_t *>(data0), m_audioBuffer.m_writePointer, size0);
        m_audioBuffer.copyBuffer(
            reinterpret_cast<int16_t *>(data1),
            m_audioBuffer.getBufferIndex(m_audioBuffer.m_writePointer, size0),
            size1);

        m_audioSource->UnlockBufferSegments(data0, size0, data1, size1);
        m_audioBuffer.commitBlock(readSamples);
    }

    m_performanceCluster->addAudioLatencySample(
        m_audioBuffer.offsetDelta(m_audioSource->GetCurrentPosition(), m_audioBuffer.m_writePointer) / (44100 * 0.1));
    m_performanceCluster->addInputBufferUsageSample(
        (double)m_simulator.getSynthesizerInputLatency() / m_simulator.getSynthesizerInputLatencyTarget());
}

void EngineSimApplication::render() {
    for (SimulationObject *object : m_objects) {
        object->generateGeometry();
        object->render(&getViewParameters());
    }

    m_uiManager.render();
}

float EngineSimApplication::pixelsToUnits(float pixels) const {
    const float f = m_displayHeight / m_engineView->m_bounds.height();
    return pixels * f;
}

float EngineSimApplication::unitsToPixels(float units) const {
    const float f = m_engineView->m_bounds.height() / m_displayHeight;
    return units * f;
}

void EngineSimApplication::run() {
    double throttle = 1.0;
    double targetThrottle = 1.0;

    double clutchPressure = 1.0;
    int lastMouseWheel = 0;

    while (true) {
        const float dt = m_engine.GetFrameLength();
        const bool fineControlMode = m_engine.IsKeyDown(ysKey::Code::Space);

        const int mouseWheel = m_engine.GetMouseWheel();
        const int mouseWheelDelta = mouseWheel - lastMouseWheel;
        lastMouseWheel = mouseWheel;

        m_gameWindowHeight = m_engine.GetGameWindow()->GetGameHeight();
        m_screenHeight = m_engine.GetGameWindow()->GetScreenHeight();
        m_screenWidth = m_engine.GetGameWindow()->GetScreenWidth();

        if (m_engine.ProcessKeyDown(ysKey::Code::Escape)) {
            break;
        }

        m_engine.StartFrame();
        if (!m_engine.IsOpen()) break;

        updateScreenSizeStability();

        if (m_engine.ProcessKeyDown(ysKey::Code::F)) {
            m_engine.GetGameWindow()->SetWindowStyle(ysWindow::WindowStyle::Fullscreen);
            m_infoCluster->setLogMessage("Entered fullscreen mode");
        }

        double newClutchPressure = 1.0;
        bool fineControlInUse = false;
        if (m_engine.IsKeyDown(ysKey::Code::Z)) {
            const double rate = fineControlMode
                ? 0.001
                : 0.01;

            Synthesizer::AudioParameters audioParams = m_simulator.getSynthesizer()->getAudioParameters();
            audioParams.Volume = clamp(audioParams.Volume + mouseWheelDelta * rate * dt);

            m_simulator.getSynthesizer()->setAudioParameters(audioParams);
            fineControlInUse = true;

            m_infoCluster->setLogMessage("[Z] - Set volume to " + std::to_string(audioParams.Volume));
        }
        else if (m_engine.IsKeyDown(ysKey::Code::X)) {
            const double rate = fineControlMode
                ? 0.001
                : 0.01;

            Synthesizer::AudioParameters audioParams = m_simulator.getSynthesizer()->getAudioParameters();
            audioParams.Convolution = clamp(audioParams.Convolution + mouseWheelDelta * rate * dt);

            m_simulator.getSynthesizer()->setAudioParameters(audioParams);
            fineControlInUse = true;

            m_infoCluster->setLogMessage("[X] - Set convolution level to " + std::to_string(audioParams.Convolution));
        }
        else if (m_engine.IsKeyDown(ysKey::Code::C)) {
            const double rate = fineControlMode
                ? 0.00001
                : 0.001;

            Synthesizer::AudioParameters audioParams = m_simulator.getSynthesizer()->getAudioParameters();
            audioParams.dF_F_mix = clamp(audioParams.dF_F_mix + mouseWheelDelta * rate * dt);

            m_simulator.getSynthesizer()->setAudioParameters(audioParams);
            fineControlInUse = true;

            m_infoCluster->setLogMessage("[C] - Set high freq. gain to " + std::to_string(audioParams.dF_F_mix));
        }
        else if (m_engine.IsKeyDown(ysKey::Code::V)) {
            const double rate = fineControlMode
                ? 0.001
                : 0.01;

            Synthesizer::AudioParameters audioParams = m_simulator.getSynthesizer()->getAudioParameters();
            audioParams.AirNoise = clamp(audioParams.AirNoise + mouseWheelDelta * rate * dt);

            m_simulator.getSynthesizer()->setAudioParameters(audioParams);
            fineControlInUse = true;

            m_infoCluster->setLogMessage("[V] - Set low freq. noise to " + std::to_string(audioParams.AirNoise));
        }
        else if (m_engine.IsKeyDown(ysKey::Code::B)) {
            const double rate = fineControlMode
                ? 0.001
                : 0.01;

            Synthesizer::AudioParameters audioParams = m_simulator.getSynthesizer()->getAudioParameters();
            audioParams.InputSampleNoise = clamp(audioParams.InputSampleNoise + mouseWheelDelta * rate * dt);

            m_simulator.getSynthesizer()->setAudioParameters(audioParams);
            fineControlInUse = true;

            m_infoCluster->setLogMessage("[B] - Set high freq. noise to " + std::to_string(audioParams.InputSampleNoise));
        }
        else if (m_engine.IsKeyDown(ysKey::Code::N)) {
            const double rate = fineControlMode
                ? 10.0
                : 100.0;

            m_simulator.setSimulationFrequency(m_simulator.getSimulationFrequency() + mouseWheelDelta * rate * dt);
            fineControlInUse = true;

            m_infoCluster->setLogMessage("[N] - Set simulation freq to " + std::to_string(m_simulator.getSimulationFrequency()));
        }

        const double prevTargetThrottle = targetThrottle;
        targetThrottle = fineControlMode ? targetThrottle : 1.0;
        if (m_engine.IsKeyDown(ysKey::Code::Q)) {
            targetThrottle = 0.99;
        }
        else if (m_engine.IsKeyDown(ysKey::Code::W)) {
            targetThrottle = 0.9;
        }
        else if (m_engine.IsKeyDown(ysKey::Code::E)) {
            targetThrottle = 0.8;
        }
        else if (m_engine.IsKeyDown(ysKey::Code::R)) {
            targetThrottle = 0.0;
        }
        else if (fineControlMode && !fineControlInUse) {
            targetThrottle = std::fmax(0.0, std::fmin(1.0, targetThrottle - mouseWheelDelta * 0.0001));
        }

        if (prevTargetThrottle != targetThrottle) {
            m_infoCluster->setLogMessage("Throttle set to " + std::to_string(targetThrottle));
        }

        throttle = targetThrottle * 0.5 + 0.5 * throttle;

        m_iceEngine->setThrottle(throttle);

        if (m_engine.ProcessKeyDown(ysKey::Code::M)) {
            const int currentLayer = getViewParameters().Layer0;
            if (currentLayer + 1 < m_iceEngine->getMaxDepth()) {
                setViewLayer(currentLayer + 1);
            }

            m_infoCluster->setLogMessage("[M] - Set render layer to " + std::to_string(getViewParameters().Layer0));
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::OEM_Comma)) {
            if (getViewParameters().Layer0 - 1 >= 0)
                setViewLayer(getViewParameters().Layer0 - 1);

            m_infoCluster->setLogMessage("[,] - Set render layer to " + std::to_string(getViewParameters().Layer0));
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::D)) {
            m_simulator.m_dyno.m_enabled = !m_simulator.m_dyno.m_enabled;

            const std::string msg = m_simulator.m_dyno.m_enabled
                ? "DYNOMOMETER ENABLED"
                : "DYNOMOMETER DISABLED";
            m_infoCluster->setLogMessage(msg);
        }

        if (m_simulator.m_dyno.m_enabled) {
            if (m_simulator.getFilteredDynoTorque() > units::torque(1.0, units::ft_lb)) {
                m_dynoSpeed += units::rpm(500) * dt;
            }
            else {
                m_dynoSpeed *= (1 / (1 + dt));
            }

            if ((m_dynoSpeed + units::rpm(1000)) > m_iceEngine->getRedline()) {
                m_simulator.m_dyno.m_enabled = false;
                m_dynoSpeed = units::rpm(0);
            }
        }
        else {
            m_dynoSpeed = units::rpm(0);
        }

        m_simulator.m_dyno.m_rotationSpeed = m_dynoSpeed + units::rpm(1000);

        const bool prevStarterEnabled = m_simulator.m_starterMotor.m_enabled;
        if (m_engine.IsKeyDown(ysKey::Code::S)) {
            m_simulator.m_starterMotor.m_enabled = true;
        }
        else {
            m_simulator.m_starterMotor.m_enabled = false;
        }

        if (prevStarterEnabled != m_simulator.m_starterMotor.m_enabled) {
            const std::string msg = m_simulator.m_starterMotor.m_enabled
                ? "STARTER ENABLED"
                : "STARTER DISABLED";
            m_infoCluster->setLogMessage(msg);
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::A)) {
            m_simulator.getEngine()->getIgnitionModule()->m_enabled =
                !m_simulator.getEngine()->getIgnitionModule()->m_enabled;

            const std::string msg = m_simulator.getEngine()->getIgnitionModule()->m_enabled
                ? "IGNITION ENABLED"
                : "IGNITION DISABLED";
            m_infoCluster->setLogMessage(msg);
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::Up)) {
            m_simulator.getTransmission()->changeGear(m_simulator.getTransmission()->getGear() + 1);

            m_infoCluster->setLogMessage(
                "UPSHIFTED TO " + std::to_string(m_simulator.getTransmission()->getGear() + 1));
        }
        else if (m_engine.ProcessKeyDown(ysKey::Code::Down)) {
            m_simulator.getTransmission()->changeGear(m_simulator.getTransmission()->getGear() - 1);

            if (m_simulator.getTransmission()->getGear() != -1) {
                m_infoCluster->setLogMessage(
                    "DOWNSHIFTED TO " + std::to_string(m_simulator.getTransmission()->getGear() + 1));
            }
            else {
                m_infoCluster->setLogMessage("SHIFTED TO NEUTRAL");
            }
        }

        if (m_engine.IsKeyDown(ysKey::Code::Shift)) {
            newClutchPressure = 0.0;

            m_infoCluster->setLogMessage("CLUTCH DEPRESSED");
        }

        double clutchRC = 0.001;
        if (m_engine.IsKeyDown(ysKey::Code::Space)) {
            clutchRC = 1.0;
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::Tab)) {
            m_screen++;
            if (m_screen > 2) m_screen = 0;
        }

        const double clutch_s = dt / (dt + clutchRC);
        clutchPressure = clutchPressure * (1 - clutch_s) + newClutchPressure * clutch_s;
        m_simulator.getTransmission()->setClutchPressure(clutchPressure);

        if (m_engine.ProcessKeyDown(ysKey::Code::M) &&
            m_engine.GetGameWindow()->IsActive()) {
            if (!isRecording() && readyToRecord()) {
                startRecording();
            }
            else if (isRecording()) {
                stopRecording();
            }
        }

        if (isRecording() && !readyToRecord()) {
            stopRecording();
        }

        if (!m_paused || m_engine.ProcessKeyDown(ysKey::Code::Right)) {
            process(m_engine.GetFrameLength());
        }

        m_uiManager.update(m_engine.GetFrameLength());

        renderScene();

        m_engine.EndFrame();

        if (isRecording()) {
            recordFrame();
        }
    }

    if (isRecording()) {
        stopRecording();
    }

    m_simulator.endAudioRenderingThread();
}

void EngineSimApplication::destroy() {
    m_shaderSet.Destroy();

    m_engine.GetDevice()->DestroyGPUBuffer(m_geometryVertexBuffer);
    m_engine.GetDevice()->DestroyGPUBuffer(m_geometryIndexBuffer);

    m_assetManager.Destroy();
    m_engine.Destroy();

    m_simulator.destroy();
}

void EngineSimApplication::drawGenerated(
    const GeometryGenerator::GeometryIndices &indices,
    int layer)
{
    drawGenerated(indices, layer, m_shaders.GetRegularFlags());
}

void EngineSimApplication::drawGeneratedUi(
    const GeometryGenerator::GeometryIndices &indices,
    int layer)
{
    drawGenerated(indices, layer, m_shaders.GetUiFlags());
}

void EngineSimApplication::drawGenerated(
    const GeometryGenerator::GeometryIndices &indices,
    int layer,
    dbasic::StageEnableFlags flags)
{
    m_engine.DrawGeneric(
        flags,
        m_geometryIndexBuffer,
        m_geometryVertexBuffer,
        sizeof(dbasic::Vertex),
        indices.BaseIndex,
        indices.BaseVertex,
        indices.FaceCount,
        false,
        layer);
}

void EngineSimApplication::createObjects(Engine *engine) {
    for (int i = 0; i < engine->getCylinderCount(); ++i) {
        ConnectingRodObject *rodObject = new ConnectingRodObject;
        rodObject->initialize(this);
        rodObject->m_connectingRod = engine->getConnectingRod(i);
        m_objects.push_back(rodObject);

        PistonObject *pistonObject = new PistonObject;
        pistonObject->initialize(this);
        pistonObject->m_piston = engine->getPiston(i);
        m_objects.push_back(pistonObject);

        CombustionChamberObject *ccObject = new CombustionChamberObject;
        ccObject->initialize(this);
        ccObject->m_chamber = m_iceEngine->getChamber(i);
        m_objects.push_back(ccObject);
    }

    for (int i = 0; i < engine->getCrankshaftCount(); ++i) {
        CrankshaftObject *crankshaftObject = new CrankshaftObject;
        crankshaftObject->initialize(this);
        crankshaftObject->m_crankshaft = engine->getCrankshaft(i);
        m_objects.push_back(crankshaftObject);
    }

    for (int i = 0; i < engine->getCylinderBankCount(); ++i) {
        CylinderBankObject *cbObject = new CylinderBankObject;
        cbObject->initialize(this);
        cbObject->m_bank = engine->getCylinderBank(i);
        cbObject->m_head = engine->getHead(i);
        m_objects.push_back(cbObject);

        CylinderHeadObject *chObject = new CylinderHeadObject;
        chObject->initialize(this);
        chObject->m_head = engine->getHead(i);
        m_objects.push_back(chObject);
    }
}

const SimulationObject::ViewParameters &EngineSimApplication
        ::getViewParameters() const
{
    return m_viewParameters;
}

void EngineSimApplication::renderScene() {
    m_shaders.SetClearColor(ysColor::linearToSrgb(m_shadow));

    const int screenWidth = m_engine.GetGameWindow()->GetGameWidth();
    const int screenHeight = m_engine.GetGameWindow()->GetGameHeight();
    const float aspectRatio = screenWidth / (float)screenHeight;

    const Point cameraPos = m_engineView->getCameraPosition();
    m_shaders.m_cameraPosition = ysMath::LoadVector(cameraPos.x, cameraPos.y);

    m_shaders.CalculateUiCamera(screenWidth, screenHeight);

    if (m_screen == 0) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        Grid grid;
        grid.v_cells = 2;
        grid.h_cells = 3;
        Grid grid3x3;
        grid3x3.v_cells = 3;
        grid3x3.h_cells = 3;
        m_engineView->setDrawFrame(true);
        m_engineView->setBounds(grid.get(windowBounds, 1, 0, 1, 1));
        m_engineView->setLocalPosition({ 0, 0 });

        m_rightGaugeCluster->m_bounds = grid.get(windowBounds, 2, 0, 1, 2);
        m_oscCluster->m_bounds = grid.get(windowBounds, 1, 1);
        m_performanceCluster->m_bounds = grid3x3.get(windowBounds, 0, 1);
        m_loadSimulationCluster->m_bounds = grid3x3.get(windowBounds, 0, 2);

        Grid grid1x3;
        grid1x3.v_cells = 3;
        grid1x3.h_cells = 1;
        m_mixerCluster->m_bounds = grid1x3.get(grid3x3.get(windowBounds, 0, 0), 0, 2);
        m_infoCluster->m_bounds = grid1x3.get(grid3x3.get(windowBounds, 0, 0), 0, 0, 1, 2);

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(true);
        m_oscCluster->setVisible(true);
        m_performanceCluster->setVisible(true);
        m_loadSimulationCluster->setVisible(true);
        m_mixerCluster->setVisible(true);
        m_infoCluster->setVisible(true);

        m_oscCluster->activate();
    }
    else if (m_screen == 1) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        m_engineView->setDrawFrame(false);
        m_engineView->setBounds(windowBounds);
        m_engineView->setLocalPosition({ 0, 0 });
        m_engineView->activate();

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(false);
        m_oscCluster->setVisible(false);
        m_performanceCluster->setVisible(false);
        m_loadSimulationCluster->setVisible(false);
        m_mixerCluster->setVisible(false);
        m_infoCluster->setVisible(false);
    }
    else if (m_screen == 2) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        Grid grid;
        grid.v_cells = 1;
        grid.h_cells = 3;
        m_engineView->setDrawFrame(true);
        m_engineView->setBounds(grid.get(windowBounds, 0, 0, 2, 1));
        m_engineView->setLocalPosition({ 0, 0 });
        m_engineView->activate();

        m_rightGaugeCluster->m_bounds = grid.get(windowBounds, 2, 0, 1, 1);

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(true);
        m_oscCluster->setVisible(false);
        m_performanceCluster->setVisible(false);
        m_loadSimulationCluster->setVisible(false);
        m_mixerCluster->setVisible(false);
        m_infoCluster->setVisible(false);
    }

    const float cameraAspectRatio =
        m_engineView->m_bounds.width() / m_engineView->m_bounds.height();
    m_engine.GetDevice()->ResizeRenderTarget(
        m_mainRenderTarget,
        m_engineView->m_bounds.width(),
        m_engineView->m_bounds.height(),
        m_engineView->m_bounds.width(),
        m_engineView->m_bounds.height()
    );
    m_engine.GetDevice()->RepositionRenderTarget(
        m_mainRenderTarget,
        m_engineView->m_bounds.getPosition(Bounds::tl).x,
        screenHeight - m_engineView->m_bounds.getPosition(Bounds::tl).y
    );
    m_shaders.CalculateCamera(
        cameraAspectRatio * m_displayHeight / m_engineView->m_zoom,
        m_displayHeight / m_engineView->m_zoom,
        m_engineView->m_bounds,
        m_screenWidth,
        m_screenHeight);

    m_geometryGenerator.reset();

    render();

    m_engine.GetDevice()->EditBufferDataRange(
        m_geometryVertexBuffer,
        (char *)m_geometryGenerator.getVertexData(),
        sizeof(dbasic::Vertex) * m_geometryGenerator.getCurrentVertexCount(),
        0);

    m_engine.GetDevice()->EditBufferDataRange(
        m_geometryIndexBuffer,
        (char *)m_geometryGenerator.getIndexData(),
        sizeof(unsigned short) * m_geometryGenerator.getCurrentIndexCount(),
        0);
}

void EngineSimApplication::startRecording() {
    m_recording = true;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    atg_dtv::Encoder::VideoSettings settings{};

    // Output filename
    settings.fname = "../workspace/video_capture/engine_sim_video_capture.mp4";
    settings.inputWidth = m_engine.GetScreenWidth();
    settings.inputHeight = m_engine.GetScreenHeight();
    settings.width = settings.inputWidth;
    settings.height = settings.inputHeight;
    settings.hardwareEncoding = true;
    settings.inputAlpha = true;
    settings.bitRate = 40000000;

    m_encoder.run(settings, 2);
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}

void EngineSimApplication::updateScreenSizeStability() {
    m_screenResolution[m_screenResolutionIndex][0] = m_engine.GetScreenWidth();
    m_screenResolution[m_screenResolutionIndex][1] = m_engine.GetScreenHeight();

    m_screenResolutionIndex = (m_screenResolutionIndex + 1) % ScreenResolutionHistoryLength;
}

bool EngineSimApplication::readyToRecord() {
    const int w = m_screenResolution[0][0];
    const int h = m_screenResolution[0][1];

    if (w <= 0 && h <= 0) return false;
    if ((w % 2) != 0 || (h % 2) != 0) return false;

    for (int i = 1; i < ScreenResolutionHistoryLength; ++i) {
        if (m_screenResolution[i][0] != w) return false;
        if (m_screenResolution[i][1] != h) return false;
    }

    return true;
}

void EngineSimApplication::stopRecording() {
    m_recording = false;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    m_encoder.commit();
    m_encoder.stop();
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}

void EngineSimApplication::recordFrame() {
#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    atg_dtv::Frame *frame = m_encoder.newFrame(false);
    if (frame != nullptr && m_encoder.getError() == atg_dtv::Encoder::Error::None) {
        m_engine.GetDevice()->ReadRenderTarget(m_engine.GetScreenRenderTarget(), frame->m_rgb);
    }

    m_encoder.submitFrame();
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}
