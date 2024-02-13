#include "transmission.h"

#include <config.h>
#include <logger.h>
#include <utils.h>

constexpr auto LABEL = "transmission";

Transmission::Transmission(
    const int itemSize, const int groupSize, TransmissionNotification& notification, std::function<Frequency(const int index)> indexToFrequency, std::function<Frequency(const int index)> indexToShift)
    : gr::sync_block("Transmission", gr::io_signature::make(1, 1, sizeof(float) * itemSize), gr::io_signature::make(0, 0, 0)),
      m_itemSize(itemSize),
      m_groupSize(groupSize),
      m_notification(notification),
      m_indexToFrequency(indexToFrequency),
      m_indexToShift(indexToShift),
      m_isProcessing(false),
      m_indexesLastDataTime(m_itemSize) {
  Logger::info(LABEL, "group size: {}", m_groupSize);
}

int Transmission::work(int noutput_items, gr_vector_const_void_star& input_items, gr_vector_void_star&) {
  const float* input_buf = static_cast<const float*>(input_items[0]);

  if (!m_isProcessing) {
    return noutput_items;
  }

  std::unique_lock<std::mutex> lock(m_mutex);
  for (int i = 0; i < noutput_items; ++i) {
    const auto power = &input_buf[i * m_itemSize];
    const auto indexes = getSortedIndexes(power);
    updateIndexesTime(power);
    clearIndexes(power);
    addIndexes(power, indexes);
    m_notification.notify(getSortedTransmissions(power));
  }

  return noutput_items;
}

void Transmission::setProcessing(const bool isProcessing) {
  if (!isProcessing) {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (const auto& index : m_indexes) {
      Logger::info(LABEL, "stop transmission, frequency: {} Hz{}", m_indexToFrequency(index));
    }
    m_indexes.clear();
  }
  m_isProcessing = isProcessing;
}

std::vector<Transmission::Index> Transmission::getSortedIndexes(const float* power) const {
  std::vector<Index> indexes;
  for (int i = 0; i < m_itemSize; ++i) {
    if (RECORDING_START_THRESHOLD <= power[i]) {
      indexes.push_back(i);
    }
  }
  std::sort(indexes.begin(), indexes.end(), [power](const Index& i1, const Index& i2) { return power[i1] > power[i2]; });
  return indexes;
}

void Transmission::clearIndexes(const float* power) {
  const auto now = getTime();
  for (auto it = m_indexes.begin(); it != m_indexes.cend();) {
    const auto index = *it;
    const auto lastDataTime = now - m_indexesLastDataTime[index];
    const auto frequency = m_indexToFrequency(index);
    Logger::debug(LABEL, "active transmission, frequency: {} Hz, avg power: {:.2f}, last data: {} ms ago", frequency, power[index], lastDataTime.count());
    if (RECORDING_TIMEOUT < lastDataTime) {
      Logger::info(LABEL, "stop transmission, frequency: {} Hz, avg power: {:.2f}", frequency, power[index]);
      m_indexes.erase(it++);
    } else {
      it++;
    }
  }
}

void Transmission::addIndexes(const float* power, const std::vector<Index>& indexes) {
  if (!indexes.empty()) {
    const auto& index = indexes.front();
    const auto frequency = m_indexToFrequency(index);
    Logger::debug(LABEL, "best group, frequency: {} Hz, avg power: {:.2f}", frequency, power[index]);
  }
  for (const auto& index : indexes) {
    const auto frequency = m_indexToFrequency(index);
    Logger::debug(LABEL, "group, frequency: {} Hz, avg power: {:.2f}", frequency, power[index]);
    if (!containsWithMargin(m_indexes, index, m_groupSize)) {
      Logger::info(LABEL, "start transmission, frequency: {} Hz, avg power: {:.2f}", frequency, power[index]);
      m_indexes.insert(index);
    }
  }
}

void Transmission::updateIndexesTime(const float* power) {
  const auto now = getTime();
  for (int i = 0; i < m_itemSize; ++i) {
    if (RECORDING_STOP_THRESHOLD <= power[i]) {
      m_indexesLastDataTime[i] = now;
    }
  }
}

std::vector<Frequency> Transmission::getSortedTransmissions(const float* power) const {
  std::vector<Index> indexes(m_indexes.begin(), m_indexes.end());
  std::sort(indexes.begin(), indexes.end(), [power](const Index& i1, const Index& i2) { return power[i1] > power[i2]; });
  std::vector<Frequency> transmissions;
  for (const auto& index : indexes) {
    transmissions.push_back(getTunedFrequency(m_indexToShift(index), TUNING_STEP));
  }
  return transmissions;
}