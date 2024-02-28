#include "sdr_device.h"

#include <config.h>
#include <gnuradio/blocks/float_to_char.h>
#include <gnuradio/blocks/stream_to_vector.h>
#include <gnuradio/fft/fft_v.h>
#include <gnuradio/fft/window.h>
#include <gnuradio/soapy/source.h>
#include <logger.h>
#include <radio/blocks/callback.h>
#include <radio/blocks/decimator.h>
#include <radio/blocks/psd.h>

constexpr auto LABEL = "sdr";

namespace {
std::string getSoapyArgs(const std::string& driver, const std::string& serial) {
  char tmp[1024];
  snprintf(tmp, 1024, "driver=%s,serial=%s", driver.c_str(), serial.c_str());
  return {tmp};
}
}  // namespace

SdrDevice::SdrDevice(
    const std::string& driver,
    const std::string& serial,
    const std::map<std::string, float> gains,
    const Frequency sampleRate,
    Mqtt& mqtt,
    TransmissionNotification& notification,
    const int recordersCount)
    : m_driver(driver),
      m_serial(serial),
      m_sampleRate(sampleRate),
      m_fftSize(getFft(m_sampleRate, MAX_STEP_AFTER_FFT)),
      m_isInitialized(false),
      m_frequencyRange({0, 0}),
      m_dataController(mqtt, driver + "_" + serial),
      m_tb(gr::make_top_block("sdr")),
      m_connector(m_tb),
      m_lastSpectogramDataSendTime(0) {
  Logger::info(LABEL, "starting");
  Logger::info(
      LABEL,
      "driver: {}, serial: {}, sample rate: {}, fft size: {}, step: {}, recorders: {}",
      m_driver,
      m_serial,
      formatFrequency(m_sampleRate),
      m_fftSize,
      formatFrequency(TUNING_STEP),
      recordersCount);

  m_source = gr::soapy::source::make(getSoapyArgs(driver, serial).c_str(), "fc32", 1);

  setupRawFileChain();
  setupPowerChain(notification);

  for (int i = 0; i < recordersCount; ++i) {
    m_recorders.push_back(std::make_unique<Recorder>(m_tb, m_source, m_sampleRate, m_dataController));
  }

  for (const auto& [key, value] : gains) {
    m_source->set_gain(0, key.c_str(), value);
  }
  m_source->set_sample_rate(0, sampleRate);

  m_tb->start();
  Logger::info(LABEL, "started");
}

SdrDevice::~SdrDevice() {
  Logger::info(LABEL, "stopping");
  m_tb->stop();
  m_tb->wait();
  Logger::info(LABEL, "stopped");
}

void SdrDevice::setFrequencyRange(FrequencyRange frequencyRange) {
  const auto frequency = (frequencyRange.first + frequencyRange.second) / 2;
  m_noiseLearner->setProcessing(false);
  m_transmission->setProcessing(false);
  if (DEBUG_SAVE_FULL_RAW_IQ) m_rawFileSink->stopRecording();

  m_frequencyRange = {0, 0};
  for (int i = 0; i < 10; ++i) {
    try {
      m_source->set_frequency(0, frequency);
      Logger::info(LABEL, "set frequency range: {} - {}, center frequency: {}", formatFrequency(frequencyRange.first), formatFrequency(frequencyRange.second), formatFrequency(frequency));
      break;
    } catch (std::exception& e) {
    }
  }
  if (!m_isInitialized) {
    Logger::info(LABEL, "waiting, initial sleep: {} ms", INITIAL_DELAY.count());
    std::this_thread::sleep_for(INITIAL_DELAY);
    m_isInitialized = true;
  }
  m_frequencyRange = frequencyRange;
  if (DEBUG_SAVE_FULL_RAW_IQ) m_rawFileSink->startRecording(getRawFileName("full", "fc", frequency, m_sampleRate));
  m_transmission->setProcessing(true);
  m_noiseLearner->setProcessing(true);
}

bool SdrDevice::updateRecordings(const std::vector<FrequencyFlush> sortedShifts) {
  const auto isWaitingForRecording = [&sortedShifts](const Frequency shift) {
    return std::find_if(sortedShifts.begin(), sortedShifts.end(), [shift](const FrequencyFlush shiftFlush) {
             // improve auto formatter
             return shift == shiftFlush.first;
           }) != sortedShifts.end();
  };
  const auto getShiftRecorder = [this](const Frequency shift) {
    return std::find_if(m_recorders.begin(), m_recorders.end(), [shift](const std::unique_ptr<Recorder>& recorder) {
      // improve auto formatter
      return recorder->getShift() == shift;
    });
  };
  const auto getFreeRecorder = [this]() {
    return std::find_if(m_recorders.begin(), m_recorders.end(), [](const std::unique_ptr<Recorder>& recorder) {
      // improve auto formatter
      return !recorder->isRecording();
    });
  };

  for (auto& recorder : m_recorders) {
    if (recorder->isRecording()) {
      const auto shift = recorder->getShift();
      if (!isWaitingForRecording(shift)) {
        recorder->stopRecording();
        Logger::info(LABEL, "stop recorder, frequency: {}, time: {} ms", formatFrequency(getFrequency() + shift, RED), recorder->getDuration().count());
      }
    }
  }

  for (const auto& [shift, flush] : sortedShifts) {
    const auto itRecorder = getShiftRecorder(shift);
    if (itRecorder != m_recorders.end()) {
      const auto& recorder = *itRecorder;
      if (!recorder->isRecording()) {
        Logger::warn(LABEL, "start recorder that should be already started, frequency: {}", formatFrequency(getFrequency() + shift), GREEN);
      }
      if (flush) {
        recorder->flush();
      }
    } else {
      const auto itFreeRecorder = getFreeRecorder();
      if (itFreeRecorder != m_recorders.end()) {
        const auto& freeRecorder = *itFreeRecorder;
        freeRecorder->startRecording(getFrequency(), shift);
        Logger::info(LABEL, "start recorder, frequency: {}", formatFrequency(getFrequency() + shift, GREEN));
      } else {
        if (!ignoredTransmissions.count(shift)) {
          Logger::info(LABEL, "no recorders available, frequency: {}", formatFrequency(getFrequency() + shift, YELLOW));
          ignoredTransmissions.insert(shift);
        }
      }
    }
  }

  for (auto it = ignoredTransmissions.begin(); it != ignoredTransmissions.cend();) {
    if (isWaitingForRecording(*it)) {
      it++;
    } else {
      ignoredTransmissions.erase(it++);
    }
  }

  for (auto& recorder : m_recorders) {
    if (recorder->isRecording()) {
      return true;
    }
  }

  return false;
}

Frequency SdrDevice::getFrequency() const { return (m_frequencyRange.first + m_frequencyRange.second) / 2; }

void SdrDevice::setupPowerChain(TransmissionNotification& notification) {
  const auto step = static_cast<double>(m_sampleRate) / m_fftSize;
  const auto indexToFrequency = [this, step](const int index) { return getFrequency() + static_cast<Frequency>(step * (index + 0.5)) - m_sampleRate / 2; };
  const auto indexToShift = [this, step](const int index) { return static_cast<Frequency>(step * (index + 0.5)) - m_sampleRate / 2; };
  const auto isIndexInRange = [this, indexToFrequency](const int index) {
    const auto f = indexToFrequency(index);
    return m_frequencyRange.first <= f && f <= m_frequencyRange.second;
  };
  const auto indexStep = static_cast<Frequency>(RECORDING_BANDWIDTH / (static_cast<double>(m_sampleRate) / m_fftSize));

  const auto s2c = gr::blocks::stream_to_vector::make(sizeof(gr_complex), m_fftSize * DECIMATOR_FACTOR);
  const auto decimator = std::make_shared<Decimator<gr_complex>>(m_fftSize, DECIMATOR_FACTOR);
  const auto fft = gr::fft::fft_v<gr_complex, true>::make(m_fftSize, gr::fft::window::hamming(m_fftSize), true);
  const auto psd = std::make_shared<PSD>(m_fftSize, m_sampleRate);
  m_noiseLearner = std::make_shared<NoiseLearner>(m_fftSize, m_frequencyRange, indexToFrequency);
  m_transmission = std::make_shared<Transmission>(m_fftSize, indexStep, notification, indexToFrequency, indexToShift, isIndexInRange);
  m_connector.connect<std::shared_ptr<gr::basic_block>>(m_source, s2c, decimator, fft, psd, m_noiseLearner, m_transmission);

  const auto spectogramFactor = getDecimatorFactor(step, SPECTROGRAM_MIN_STEP);
  const auto spectogramDecimator = std::make_shared<Decimator<float>>(m_fftSize / spectogramFactor, spectogramFactor);
  const auto f2c = gr::blocks::float_to_char::make(m_fftSize / spectogramFactor);
  const auto callback = std::make_shared<Callback<int8_t>>(m_fftSize / spectogramFactor, [this, spectogramFactor](const int8_t* data, const int size) {
    const auto now = getTime();
    const auto frequency = getFrequency();
    const auto fftSize = m_fftSize / spectogramFactor;
    for (int i = 0; i < size; ++i) {
      if (m_lastSpectogramDataSendTime + SPECTROGRAM_SEND_INTERVAL < now && frequency != 0) {
        m_dataController.pushSpectrogram(now, frequency, m_sampleRate, data + i * fftSize, fftSize);
        m_lastSpectogramDataSendTime = now;
      }
    }
  });
  m_connector.connect<std::shared_ptr<gr::basic_block>>(psd, spectogramDecimator, f2c, callback);
}

void SdrDevice::setupRawFileChain() {
  if (DEBUG_SAVE_FULL_RAW_IQ) {
    m_rawFileSink = std::make_shared<FileSink<gr_complex>>(1, false);
    m_connector.connect<std::shared_ptr<gr::basic_block>>(m_source, m_rawFileSink);
  }
}
