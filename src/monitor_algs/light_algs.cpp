//
// Created by Jon Sensenig on 8/20/25.
//

#include "light_algs.h"
#include <cmath>
#include <asio/detail/event.hpp>


//bool LightAlgs::ProcessEvent(EventStruct &event) {
//
//    // Get the number of light channels
//
//    return true;
//}

void LightAlgs::MinimalSummary(EventStruct &event) {
    // The unbiased light readout corresponds to ID 0x4. We want to use this to get an unbiased snapshot
    // of channel baseline & RMS loosely correlated with the trigger since it initiates the unbiased readout

    std::cout << "Size ID/Ch/ROI: " << event.light_trigger_id.size() << "/"
            << event.light_channel.size() << "/" << event.light_adc.size() << std::endl;

    for (size_t i = 0; i < event.light_channel.size(); i++) { // loop over each RI in the event
        if (event.light_channel[i] > NUM_LIGHT_CHANNELS-1) continue;
        if (event.light_trigger_id.at(i) != BEAM_GATE_DISC_ID) { // should be a Cosmic ie Disc 1 ROI
            light_rois_[event.light_channel.at(i)]++;
            continue; // skip non-beam gate readout ROIs
        }
        light_baseline_rms_norm_[event.light_channel.at(i)]++;
        BaselineRms(event.light_adc.at(i), event.light_channel.at(i));
    }
    num_events_++;
}

void LightAlgs::BaselineRms(const std::vector<uint16_t> &light_roi_words, uint16_t channel) {
    // Assuming we are receiving the unbiased light readout ROI
    // Calculate the baseline and RMS for the channel, only use the first 8 samples unless
    // there are <8 but shouldn't happen
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
    if (num_events_ < 1) { num_events_ = 1; } // Avoid divide by 0
    /*
     * Finish the aggregated Baseline & RMS calculation and update the metrics
     * Average hits ROIs event and the charge hits to the metrics
     */

    std::array<uint32_t, NUM_LIGHT_CHANNELS> baseline_int{};
    std::array<uint32_t, NUM_LIGHT_CHANNELS> rms_int{};
    std::array<uint32_t, NUM_LIGHT_CHANNELS> avg_rois_int{};
    for (size_t i = 0; i < NUM_LIGHT_CHANNELS; i++) {
        baseline_int[i] = static_cast<int>(baseline_[i] / light_baseline_rms_norm_[i]);
        // TODO could perform the sqrt on ground for safety and efficiency
        // check to make sure rms is non-negative, should never be but better to avoid NaN
        rms_int[i] = static_cast<int>((variance_[i] < 0) ? INT16_MAX : 15 * (std::sqrt(variance_[i] / light_baseline_rms_norm_[i])));
        avg_rois_int[i] = static_cast<uint32_t>(15 * (light_rois_[i] / num_events_));
    }

    // update the metrics
    lbw_metrics.setLightBaselines(baseline_int);
    lbw_metrics.setLightRms(rms_int);
    lbw_metrics.setLightAvgNumRois(avg_rois_int);
}

uint32_t LightAlgs::LightRoiStart2MHzTick(uint32_t light_frame, uint16_t light_sample) {
    // The light sample is on the 64MHz clock
    // Since we are sending with a 32b variable and matching with the TPC (on the 2MHz) clock
    // we convert the 64MHz sample to the nearest 2MHz tick (64MHz_tick % 32 < 16 = 0) and add the frame * 2MHz
    double factor_64MHz_to_2Mhz = 32.0; // 32 64MHz ticks for every 2Mhz ticks a hardware constant
    auto light_sample_dbl = static_cast<double>(light_sample);
    auto rounded_2MHz_tick = std::round(light_sample_dbl / factor_64MHz_to_2Mhz);

    return static_cast<uint32_t>(rounded_2MHz_tick) + light_frame * tpc_readout_2MHz_ticks_;
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
        auto start_tick = LightRoiStart2MHzTick(event.light_frame_number[i], event.light_sample_number[i]);
        light_roi_start_.push_back(start_tick);
    }
    return light_roi_channels_.size();
}

std::vector<uint32_t> LightAlgs::UpdateLightEvent(TpcMonitorLightEvent &tpc_light_metric, size_t roi) {
    if (light_roi_channels_.empty() || light_cosmic_rois_.empty()) return {};
    tpc_light_metric.setChannelNumber(light_roi_channels_[roi]);
    tpc_light_metric.setLightSamples(light_cosmic_rois_[roi]);
    tpc_light_metric.setStartTick(light_roi_start_[roi]);

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
    light_roi_start_.clear();
}
