//
// Created by Jon Sensenig on 8/20/25.
//

#include "light_algs.h"
#include <cmath>
#include <asio/detail/event.hpp>


void LightAlgs::MinimalSummary(EventStruct &event) {
    std::cout << "Size ID/Ch/ROI: " << event.light_trigger_id.size() << "/"
            << event.light_channel.size() << "/" << event.light_adc.size() << std::endl;

    for (size_t i = 0; i < event.light_channel.size(); i++) {
        if (event.light_channel[i] > NUM_LIGHT_CHANNELS-1) continue;
        if (event.light_trigger_id.at(i) != BEAM_GATE_DISC_ID) {
            light_rois_[event.light_channel.at(i)]++;
            continue;
        }
        light_baseline_rms_norm_[event.light_channel.at(i)]++;
        BaselineRms(event.light_adc.at(i), event.light_channel.at(i));
    }
    num_events_++;
}

void LightAlgs::BaselineRms(const std::vector<uint16_t> &light_roi_words, uint16_t channel) {
    size_t num_samples = light_roi_words.size() > 7 ? 8 : light_roi_words.size();
    if (num_samples < 1) return;

    double baseline_sum = 0;
    for (size_t i = 0; i < num_samples; i++) { baseline_sum += light_roi_words[i]; }
    baseline_sum /= num_samples;
    baseline_[channel] += baseline_sum;

    double variance_sum = 0;
    for (size_t i = 0; i < num_samples; i++) {
        variance_sum += (light_roi_words[i] - baseline_sum) * (light_roi_words[i] - baseline_sum);
    }
    variance_sum /= num_samples;
    variance_[channel] += variance_sum;
}


void LightAlgs::UpdateMinimalMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics) {
    if (num_events_ < 1) { num_events_ = 1; }

    std::array<uint32_t, NUM_LIGHT_CHANNELS> baseline_int{};
    std::array<uint32_t, NUM_LIGHT_CHANNELS> rms_int{};
    std::array<uint32_t, NUM_LIGHT_CHANNELS> avg_rois_int{};
    for (size_t i = 0; i < NUM_LIGHT_CHANNELS; i++) {
        baseline_int[i] = static_cast<int>(baseline_[i] / light_baseline_rms_norm_[i]);
        rms_int[i] = static_cast<int>((variance_[i] < 0) ? INT16_MAX : 15 * (std::sqrt(variance_[i] / light_baseline_rms_norm_[i])));
        avg_rois_int[i] = static_cast<uint32_t>(15 * (light_rois_[i] / num_events_));
    }

    lbw_metrics.setLightBaselines(baseline_int);
    lbw_metrics.setLightRms(rms_int);
    lbw_metrics.setLightAvgNumRois(avg_rois_int);
}


size_t LightAlgs::GetLightEvent(EventStruct &event) {
    light_cosmic_rois_.resize(event.light_adc.size());
    for (size_t i = 0; i < event.light_adc.size(); i++) {
        if (event.light_trigger_id[i] != COSMIC_DISC_ID) continue;
        light_cosmic_rois_[i].resize(event.light_adc[i].size());
        std::copy(event.light_adc[i].begin(),
                  event.light_adc[i].end(),
                  light_cosmic_rois_[i].begin());
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
        variance_[i] = 0;
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
}
