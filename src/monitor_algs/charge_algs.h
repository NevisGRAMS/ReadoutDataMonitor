//
// Created by Jon Sensenig on 8/20/25.
//

#ifndef CHARGE_ALGS_H
#define CHARGE_ALGS_H

#include "tpc_monitor.h"
#include "process_events.h"
#include "tpc_monitor_lbw.h"
#include "tpc_monitor_charge_event.h"

class ChargeAlgs {
public:
    ChargeAlgs() = default;
    ~ChargeAlgs() = default;

    void Clear();
    void MinimalSummary(EventStruct &event);
    // TpcMonitor& is unused: 0x4001 has no histogram / SEM fields.
    void UpdateMinimalMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics);
    // Deprecated: old Query_Event_Data (0x4002) middle-1/3 slice.
    // Full-event telemetry uses GetFullEventChargeEvent ([248, 520)).
    void GetChargeEvent(EventStruct &event);
    void GetFullEventChargeEvent(EventStruct &event);
    std::vector<uint32_t> UpdateChargeEvent(TpcMonitorChargeEvent &tpc_charge_metric, size_t channel);

private:
    static constexpr double kPeakRmsSigma = 5.0;
    static constexpr double kPeakAbsFloorAdc = 5.0;
    static constexpr int kPeakMinWidth = 3;
    static constexpr int kPreTriggerGuard = 10;
    static constexpr uint16_t kPedestalMaxMinReject = 30;

    void BaselineRmsAndPeaks(const std::vector<uint16_t>& adc, uint16_t channel);

    // Deprecated: leftover from TpcMonitor histogram LBW; never filled.
    Histogram charge_histogram_{1024, 4096, 16};

    std::array<double, NUM_CHARGE_CHANNELS> baseline_{0};
    std::array<double, NUM_CHARGE_CHANNELS> rms_sum_{0};
    std::array<double, NUM_CHARGE_CHANNELS> charge_hits_{0};
    std::array<size_t, NUM_CHARGE_CHANNELS> accepted_norm_{0};
    std::array<std::vector<uint32_t>, NUM_CHARGE_CHANNELS> charge_oneframe_samples_;
    size_t num_events_ = 0;
};

#endif //CHARGE_ALGS_H
