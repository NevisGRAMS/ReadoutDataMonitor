//
// Created by Jon Sensenig on 8/20/25.
//

#ifndef LIGHT_ALGS_H
#define LIGHT_ALGS_H

#include "tpc_monitor.h"
#include "process_events.h"
#include "tpc_monitor_lbw.h"
#include "tpc_monitor_light_event.h"

class LightAlgs {
public:
    LightAlgs() = default;
    ~LightAlgs() = default;

    void Clear();

    void MinimalSummary(EventStruct& event);
    // TpcMonitor& is unused: 0x4001 has no histogram / SEM fields.
    void UpdateMinimalMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics);
    // Compact all light ROIs for 0x4003 (full-event and deprecated query).
    size_t GetLightEvent(EventStruct &event);
    std::vector<uint32_t> UpdateLightEvent(TpcMonitorLightEvent &tpc_light_metric, size_t roi);
    bool isLightRoi() { return !(light_roi_channels_.empty() || light_cosmic_rois_.empty()); }
    void setTpcReadoutLength(uint32_t length) { tpc_readout_2MHz_ticks_ = length; }

    private:

    static constexpr size_t kTailSamples = 20;
    static constexpr size_t kTailExcludeLast = 2;
    static constexpr uint16_t kTailMaxMinReject = 30;

    bool QuietPedestal(const std::vector<uint16_t> &light_roi_words, double &ped, double &rms) const;

    std::array<double, NUM_LIGHT_CHANNELS> rms_sum_{0};
    std::array<double, NUM_LIGHT_CHANNELS> baseline_{0};
    std::array<size_t, NUM_LIGHT_CHANNELS> light_rois_{0};
    std::array<size_t, NUM_LIGHT_CHANNELS> light_baseline_rms_norm_{0};
    std::vector<std::vector<uint32_t>> light_cosmic_rois_{0};
    std::vector<uint16_t> light_roi_channels_{0};
    std::vector<uint8_t> light_roi_frame_mod8_{0};
    std::vector<uint16_t> light_roi_sample_64_{0};
    size_t num_events_ = 0;
    uint32_t tpc_readout_2MHz_ticks_ = 255;

};

#endif //LIGHT_ALGS_H
