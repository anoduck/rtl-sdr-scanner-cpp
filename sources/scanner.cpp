#include "scanner.h"

#include <config.h>
#include <logger.h>

constexpr auto LABEL = "scanner";

Scanner::Scanner(const Config& config, const Device& device, Mqtt& mqtt, const int recordersCount)
    : m_device(config, device, mqtt, m_notification, recordersCount), m_ranges(device.m_ranges), m_isRunning(true), m_thread([this]() { worker(); }) {
  Logger::info(LABEL, "starting");
  Logger::info(LABEL, "ignored ranges: {}", colored(GREEN, "{}", config.ignoredRanges().size()));
  for (const auto& range : config.ignoredRanges()) {
    Logger::info(LABEL, "ignored range: {} - {}", formatFrequency(range.first), formatFrequency(range.second));
  }
  Logger::info(LABEL, "scanned ranges: {}", colored(GREEN, "{}", m_ranges.size()));
  for (const auto& range : m_ranges) {
    Logger::info(LABEL, "scanned range: {} - {}", formatFrequency(range.first), formatFrequency(range.second));
  }
  Logger::info(LABEL, "started");
}

Scanner::~Scanner() {
  m_isRunning = false;
  m_notification.notify({});
  m_thread.join();
}

void Scanner::worker() {
  Logger::info(LABEL, "thread started");
  if (m_ranges.empty()) {
    Logger::warn(LABEL, "empty scanned ranges");
  } else if (m_ranges.size() == 1) {
    m_device.setFrequencyRange(m_ranges.front());
    while (m_isRunning) {
      m_device.updateRecordings(m_notification.wait());
    }
  } else {
    while (m_isRunning) {
      for (const auto& range : m_ranges) {
        m_device.setFrequencyRange(range);

        const auto startScanningTime = getTime();
        bool work = true;
        while (getTime() <= startScanningTime + RANGE_SCANNING_TIME && work && m_isRunning) {
          work = !m_device.updateRecordings(m_notification.wait());
        }
        if (!m_isRunning) {
          break;
        }
      }
    }
  }
  Logger::info(LABEL, "thread stopped");
}
