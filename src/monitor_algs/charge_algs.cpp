//
// Created by Jon Sensenig on 8/20/25.
//

#include "charge_algs.h"

#include <cmath>
#include <algorithm>

void ChargeAlgs::MinimalSummary(EventStruct &event) {
    for (size_t j = 0; j < event.charge_channel.size(); ++j) {
        const uint16_t channel = event.charge_channel[j];
        if (channel >= NUM_CHARGE_CHANNELS) {
            continue;
        }
        BaselineRmsAndPeaks(event.charge_adc[j], channel);
    }
    num_events_++;
}

void ChargeAlgs::BaselineRmsAndPeaks(const std::vector<uint16_t>& adc, uint16_t channel) {
    if (adc.empty()) {
        return;
    }

    // Waveform is trigger-aligned: t0 = CHARGE_START_SAMPLES (256), not the
    // FEMHeader6 in-frame trigger_sample (0–255). Pedestal [0, t0−10).
    const int end = std::min(static_cast<int>(CHARGE_START_SAMPLES) - kPreTriggerGuard,
                             static_cast<int>(adc.size()));
    if (end < 2) {
        return;
    }

    uint16_t lo = adc[0];
    uint16_t hi = adc[0];
    double sum = 0.0;
    for (int i = 0; i < end; ++i) {
        const uint16_t v = adc[i];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        sum += v;
    }

    // Dirty pre-trigger: drop this event from the run average of baseline,
    // RMS, and hit# (per-event DQM can still compute it separately).
    if (static_cast<uint16_t>(hi - lo) > kPedestalMaxMinReject) {
        return;
    }

    const double ped = sum / static_cast<double>(end);
    double var_sum = 0.0;
    for (int i = 0; i < end; ++i) {
        const double d = adc[i] - ped;
        var_sum += d * d;
    }
    const double rms = std::sqrt(var_sum / static_cast<double>(end));

    const double threshold = ped + std::max(kPeakAbsFloorAdc, kPeakRmsSigma * rms);
    bool above = false;
    int width = 0;
    size_t hits = 0;
    for (const uint16_t sample : adc) {
        if (sample > threshold) {
            if (!above) {
                above = true;
                width = 1;
            } else {
                width++;
            }
        } else if (above) {
            if (width >= kPeakMinWidth) {
                hits++;
            }
            above = false;
            width = 0;
        }
    }
    if (above && width >= kPeakMinWidth) {
        hits++;
    }

    baseline_[channel] += ped;
    rms_sum_[channel] += rms;
    charge_hits_[channel] += static_cast<double>(hits);
    accepted_norm_[channel]++;
}

void ChargeAlgs::UpdateMinimalMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics) {
    (void)metrics;
    if (num_events_ < 1) {
        num_events_ = 1;
    }
    std::cout << "num_events_ = " << num_events_ << std::endl;
    std::array<uint32_t, NUM_CHARGE_CHANNELS> baseline_int{};
    std::array<uint32_t, NUM_CHARGE_CHANNELS> rms_int{};
    std::array<uint32_t, NUM_CHARGE_CHANNELS> avg_hits_int{};
    for (size_t i = 0; i < NUM_CHARGE_CHANNELS; i++) {
        if (accepted_norm_[i] == 0) {
            continue;
        }
        const double n = static_cast<double>(accepted_norm_[i]);
        baseline_int[i] = static_cast<uint32_t>(std::lround(LBW_BASELINE_SCALE * (baseline_[i] / n)));
        rms_int[i] = static_cast<uint32_t>(std::lround(LBW_RMS_SCALE * (rms_sum_[i] / n)));
        avg_hits_int[i] = static_cast<uint32_t>(std::lround(LBW_HIT_SCALE * (charge_hits_[i] / n)));
    }

    lbw_metrics.setChargeBaselines(baseline_int);
    lbw_metrics.setChargeRms(rms_int);
    lbw_metrics.setAvgNumHits(avg_hits_int);
}

void ChargeAlgs::GetChargeEvent(EventStruct &event) {
    std::cout << event.charge_adc.size() << "/" << charge_oneframe_samples_.size() << std::endl;
    for (size_t j = 0; j < event.charge_adc.size(); j++) {
        auto charge_one_frame_size = static_cast<size_t>(event.charge_adc[j].size() / 3);
        charge_oneframe_samples_[event.charge_channel[j]].resize(charge_one_frame_size);
        std::copy(event.charge_adc[j].begin() + charge_one_frame_size,
                   event.charge_adc[j].begin() + 2 * charge_one_frame_size,
                 charge_oneframe_samples_[event.charge_channel[j]].data());
    }
}

void ChargeAlgs::GetFullEventChargeEvent(EventStruct &event) {
    for (size_t j = 0; j < event.charge_adc.size(); j++) {
        const auto& adc = event.charge_adc[j];
        const size_t start = FULL_EVENT_CHARGE_START;
        const size_t end = std::min(FULL_EVENT_CHARGE_END, adc.size());
        const size_t window_size = (end > start) ? (end - start) : 0;
        charge_oneframe_samples_[event.charge_channel[j]].resize(window_size);
        if (window_size > 0) {
            std::copy(adc.begin() + start, adc.begin() + end,
                      charge_oneframe_samples_[event.charge_channel[j]].data());
        }
    }
}

std::vector<uint32_t> ChargeAlgs::UpdateChargeEvent(TpcMonitorChargeEvent &tpc_charge_metric, size_t channel) {
    tpc_charge_metric.setChannelNumber(channel);
    tpc_charge_metric.setChargeSamples(charge_oneframe_samples_[channel]);

    return tpc_charge_metric.serialize();
}

void ChargeAlgs::Clear() {
    for (size_t i = 0; i < NUM_CHARGE_CHANNELS; i++) {
        baseline_[i] = 0;
        rms_sum_[i] = 0;
        charge_hits_[i] = 0;
        accepted_norm_[i] = 0;
        std::fill(charge_oneframe_samples_[i].begin(), charge_oneframe_samples_[i].end(), 0);
    }
    num_events_ = 0;
}
