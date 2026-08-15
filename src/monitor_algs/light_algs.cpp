//
// Created by Jon Sensenig on 8/20/25.
//

#include "light_algs.h"
#include <cmath>
#include <algorithm>


void LightAlgs::MinimalSummary(EventStruct &event) {
    std::cout << "Size ID/Ch/ROI: " << event.light_trigger_id.size() << "/"
            << event.light_channel.size() << "/" << event.light_adc.size() << std::endl;

    std::array<double, NUM_LIGHT_CHANNELS> ev_ped{};
    std::array<double, NUM_LIGHT_CHANNELS> ev_rms{};
    std::array<size_t, NUM_LIGHT_CHANNELS> ev_n_quiet{};
    std::array<size_t, NUM_LIGHT_CHANNELS> ev_n_roi{};

    for (size_t i = 0; i < event.light_channel.size(); i++) {
        const uint16_t channel = event.light_channel[i];
        if (channel > NUM_LIGHT_CHANNELS - 1) {
            continue;
        }
        ev_n_roi[channel]++;
        double ped = 0.0;
        double rms = 0.0;
        if (QuietPedestal(event.light_adc[i], ped, rms)) {
            ev_ped[channel] += ped;
            ev_rms[channel] += rms;
            ev_n_quiet[channel]++;
        }
    }

    for (size_t ch = 0; ch < NUM_LIGHT_CHANNELS; ch++) {
        if (ev_n_roi[ch] == 0) {
            continue;
        }
        light_rois_[ch] += ev_n_roi[ch];
        if (ev_n_quiet[ch] == 0) {
            continue;
        }
        baseline_[ch] += ev_ped[ch] / static_cast<double>(ev_n_quiet[ch]);
        rms_sum_[ch] += ev_rms[ch] / static_cast<double>(ev_n_quiet[ch]);
        light_baseline_rms_norm_[ch]++;
    }
    num_events_++;
}

bool LightAlgs::QuietPedestal(const std::vector<uint16_t> &light_roi_words,
                              double &ped, double &rms) const {
    if (light_roi_words.size() < kTailExcludeLast + kTailSamples) {
        return false;
    }
    // Pedestal window [-22, -3] (20 samples), dropping the last two ticks.
    const size_t start = light_roi_words.size() - kTailExcludeLast - kTailSamples;
    const size_t end = light_roi_words.size() - kTailExcludeLast;
    uint16_t lo = light_roi_words[start];
    uint16_t hi = light_roi_words[start];
    double sum = 0.0;
    for (size_t i = start; i < end; ++i) {
        const uint16_t v = light_roi_words[i];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        sum += v;
    }
    if (static_cast<uint16_t>(hi - lo) > kTailMaxMinReject) {
        return false;
    }

    ped = sum / static_cast<double>(kTailSamples);
    double var_sum = 0.0;
    for (size_t i = start; i < end; ++i) {
        const double d = light_roi_words[i] - ped;
        var_sum += d * d;
    }
    rms = std::sqrt(var_sum / static_cast<double>(kTailSamples));
    return true;
}


void LightAlgs::UpdateMinimalMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics) {
    (void)metrics;  // Deprecated TpcMonitor histogram path; 0x4001 has no SEM fields.
    if (num_events_ < 1) { num_events_ = 1; }

    std::array<uint32_t, NUM_LIGHT_CHANNELS> baseline_int{};
    std::array<uint32_t, NUM_LIGHT_CHANNELS> rms_int{};
    std::array<uint32_t, NUM_LIGHT_CHANNELS> avg_rois_int{};
    for (size_t i = 0; i < NUM_LIGHT_CHANNELS; i++) {
        if (light_baseline_rms_norm_[i] > 0) {
            const double n = static_cast<double>(light_baseline_rms_norm_[i]);
            baseline_int[i] = static_cast<uint32_t>(std::lround(LBW_BASELINE_SCALE * (baseline_[i] / n)));
            rms_int[i] = static_cast<uint32_t>(std::lround(LBW_RMS_SCALE * (rms_sum_[i] / n)));
        }
        avg_rois_int[i] = static_cast<uint32_t>(
            std::lround(LBW_HIT_SCALE * (light_rois_[i] / static_cast<double>(num_events_))));
    }

    lbw_metrics.setLightBaselines(baseline_int);
    lbw_metrics.setLightRms(rms_int);
    lbw_metrics.setLightAvgNumRois(avg_rois_int);
}


size_t LightAlgs::GetLightEvent(EventStruct &event) {
    // Compact all discriminator IDs (name light_cosmic_rois_ is historical).
    // Indexing used to be sparse by original ROI i while channels_ was
    // compacted, so UpdateLightEvent(roi) pulled the wrong waveform.
    light_cosmic_rois_.clear();
    light_roi_channels_.clear();
    light_roi_frame_mod8_.clear();
    light_roi_sample_64_.clear();
    light_cosmic_rois_.reserve(event.light_adc.size());
    for (size_t i = 0; i < event.light_adc.size(); i++) {
        light_cosmic_rois_.emplace_back(event.light_adc[i].size());
        std::copy(event.light_adc[i].begin(),
                  event.light_adc[i].end(),
                  light_cosmic_rois_.back().begin());
        light_roi_channels_.push_back(event.light_channel[i]);
        light_roi_frame_mod8_.push_back(
            i < event.light_frame_mod8.size()
                ? event.light_frame_mod8[i]
                : static_cast<uint8_t>(event.light_frame_number[i] & 0x7));
        light_roi_sample_64_.push_back(event.light_sample_number[i]);
    }
    return light_roi_channels_.size();
}

std::vector<uint32_t> LightAlgs::UpdateLightEvent(TpcMonitorLightEvent &tpc_light_metric, size_t roi) {
    if (light_roi_channels_.empty() || light_cosmic_rois_.empty()) return {};
    tpc_light_metric.setChannelNumber(light_roi_channels_[roi]);
    tpc_light_metric.setLightSamples(light_cosmic_rois_[roi]);
    tpc_light_metric.setFrameNum(light_roi_frame_mod8_[roi]);
    tpc_light_metric.setStartSample(light_roi_sample_64_[roi]);

    return tpc_light_metric.serialize();
}


void LightAlgs::Clear() {
    for (size_t i = 0; i < NUM_LIGHT_CHANNELS; i++) {
        rms_sum_[i] = 0;
        baseline_[i] = 0;
        light_rois_[i] = 0;
        light_baseline_rms_norm_[i] = 0;
    }

    for (auto & light_cosmic_roi : light_cosmic_rois_) {
        std::fill(light_cosmic_roi.begin(), light_cosmic_roi.end(), 0);
    }
    light_roi_channels_.clear();
    light_roi_frame_mod8_.clear();
    light_roi_sample_64_.clear();
    num_events_ = 0;
}
