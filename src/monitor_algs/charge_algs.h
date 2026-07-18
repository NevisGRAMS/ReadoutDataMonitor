//
// Created by Jon Sensenig on 8/20/25.
//

#ifndef CHARGE_ALGS_H
#define CHARGE_ALGS_H

//#include "monitor_algs_base.hpp"
#include "tpc_monitor.h"
#include "process_events.h"
#include "tpc_monitor_lbw.h"
#include "tpc_monitor_charge_event.h"

class ChargeAlgs {
public:
    ChargeAlgs() = default;
    ~ChargeAlgs() = default;

//    bool ProcessEvent(EventStruct &event) override;
//    bool UpdateMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics) override;
    void Clear();
    // Minimal or low-bandwidth
    void MinimalSummary(EventStruct &event);
    void UpdateMinimalMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics);
    void BaselineRms(const std::vector<uint16_t> &channel_charge_words, uint16_t channel);
    void HitsAboveThreshold(const std::vector<uint16_t> &channel_charge_words, uint16_t channel);
    // Return an event
    void GetChargeEvent(EventStruct &event);
    void GetFullEventChargeEvent(EventStruct &event);
    std::vector<uint32_t> UpdateChargeEvent(TpcMonitorChargeEvent &tpc_charge_metric, size_t channel);

private:

    // Metric classes
    // LowBwTpcMonitor lbw_metrics_;
    // TpcMonitorChargeEvent charge_event_;

    // Static variables
    // constexpr static size_t NUM_CHANNELS = 192;
    // constexpr static size_t NUM_SAMPLES = 763;

    Histogram charge_histogram_{1024, 4096, 16};

    std::array<double, NUM_CHARGE_CHANNELS> variance_{0};
    std::array<double, NUM_CHARGE_CHANNELS> baseline_{0};
    std::array<size_t, NUM_CHARGE_CHANNELS> charge_hits_{0};
    // std::array<std::array<uint32_t, CHARGE_ONE_FRAME>, NUM_CHARGE_CHANNELS> charge_oneframe_samples_{0};
    std::array<std::vector<uint32_t>, NUM_CHARGE_CHANNELS> charge_oneframe_samples_;
    size_t num_events_ = 0;

};

#endif //CHARGE_ALGS_H
