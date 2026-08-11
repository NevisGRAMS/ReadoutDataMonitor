//
// Created by Jon Sensenig on 5/5/25.
//

#ifndef DATA_MONITOR_H
#define DATA_MONITOR_H

#include "tcp_connection.h"
#include "CommunicationCodes.hh"
#include "process_events.h"
#include "light_algs.h"
#include "charge_algs.h"
#include "tpc_monitor_fem_header.h"
#include "tpc_monitor_full_event_complete.h"
#include <random>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace data_monitor {

    using namespace pgrams::communication;

struct ReadoutFileCandidate {
    uint32_t run = 0;
    uint32_t file = 0;
    std::string path;
};

class DataMonitor {
public:

    DataMonitor(asio::io_context& io_context, const std::string& ip_address,
                uint16_t command_port, uint16_t status_port, bool is_server, bool is_running);
    ~DataMonitor();

    void SetRunning(bool run);

    // FIXME shoulf be private, public for testing
    void ProcessFile();
    void GetEventMetrics();
    void Run();
    void ReceiveCommand();
    void SetMonitorFile(const std::string &monitor_file) { monitor_file_ = monitor_file; }
    void RunMetrics();

    // Expose these so we can run it from command line
    void HandleCommand(Command& cmd);

private:

    static constexpr uint32_t kAutoRun = 99999;
    static constexpr uint32_t kAutoFile = 99999;
    static constexpr uint32_t kAutoEvent = 99999;
    static constexpr uint32_t kAutoLLag = 99999;
    static constexpr uint32_t kMinPeriodSec = 1;
    static constexpr uint32_t kMaxPeriodSec = 3600;

    void GetEnvVariables();
    void setFileName(std::vector<uint32_t>& args);
    void setNumEvent(std::vector<uint32_t>& args);
    void setEventNumber(std::vector<uint32_t>& args);

    static bool IsAutoRun(uint32_t run);
    static bool IsAutoFile(uint32_t file);
    static bool IsAutoEvent(uint32_t event);
    static bool IsAutoLLag(uint32_t l_lag);

    struct FemStamp {
        uint32_t event_frame = 0;  // FEMHeader6 event frame (f#)
        bool valid = false;
    };

    struct LightLagMatch {
        uint32_t l_lag = 0;
        bool exact = false;
        bool found = false;
    };

    std::vector<std::string> ReadoutSearchDirs() const;
    std::string BuildMonitorFilePath(const std::string& readout_dir, uint32_t run, uint32_t file) const;
    std::vector<ReadoutFileCandidate> CollectReadoutFiles(uint32_t run_filter) const;
    std::optional<ReadoutFileCandidate> FindExplicitReadoutFile(uint32_t run, uint32_t file) const;
    std::optional<ReadoutFileCandidate> FindClosedReadoutFile(uint32_t run_filter, uint32_t file_filter,
                                                              uint32_t min_file_exclusive) const;
    std::optional<ReadoutFileCandidate> FindInitialClosedReadoutFile(uint32_t run_filter) const;
    std::optional<ReadoutFileCandidate> FindNextClosedReadoutFile(uint32_t run_filter, uint32_t after_run,
                                                                  uint32_t after_file) const;
    bool ResolveMonitorTarget(uint32_t run, uint32_t file, uint32_t& run_out, uint32_t& file_out,
                              std::string& path_out) const;
    // Full-event only: each of run/file may be 99999 independently; file must be closed.
    bool ResolveClosedFullEventFile(uint32_t run, uint32_t file, ReadoutFileCandidate& out) const;
    bool CountEventsInOpenFile(uint32_t& last_evt_idx);
    static FemStamp StampFromEvent(const EventStruct& event, uint16_t slot);
    static FemStamp FirstChargeStamp(const EventStruct& event, uint16_t light_slot);
    // Auto L_lag: match on f# only (La.f# == Q.f#). t#/s# are checked on ground.
    static LightLagMatch MatchLightLag(const FemStamp& q,
                                       const std::vector<FemStamp>& light_from_base_evt);

    void RunLbQueryOnCurrentTarget();
    void StartContinuousLbw(const std::vector<uint32_t>& args);
    void StopContinuousLbw();
    void ContinuousLbwLoop();
    bool OpenContinuousFile(const ReadoutFileCandidate& target);
    bool ProcessAndSendOneLbEvent();
    bool AdvanceContinuousToNextClosedFile();
    bool TryOpenInitialContinuousFile();
    void ReleaseContinuousFile();
    bool IsFixedContinuousTarget() const;
    void StopContinuousLbwLocked(const char* reason);

    void SendFullEventData(const std::vector<uint32_t>& args);
    bool LoadEventsForFullEvent(uint32_t evt_idx, uint32_t l_lag, EventStruct& base_out,
                                EventStruct& l_header_out, EventStruct& l_adc_out,
                                bool& have_l_header, bool& have_l_adc);
    bool ResolveAutoLightLag(uint32_t evt_idx, uint32_t& l_lag_out, bool& exact_out);
    EventStruct MergeFullEventWithLLag(const EventStruct& base, const EventStruct& l_header,
                                       const EventStruct& l_adc, uint32_t l_lag) const;
    void SendFemHeaders(const EventStruct& event, uint32_t evt_idx);
    void SendFullEventPayload(EventStruct& event, uint32_t evt_idx,
                              uint32_t& num_fem, uint32_t& num_charge, uint32_t& num_light);
    void SendFullEventComplete(uint32_t evt_idx, uint32_t l_lag, uint32_t num_fem,
                               uint32_t num_charge, uint32_t num_light,
                               TpcMonitorFullEventComplete::Status status);

    void CreateMinimalMetrics(EventStruct & event);
    void UpdateMinimalMetrics(size_t evt_number);

    void CreateEventMetrics(EventStruct & event);
    void UpdateEventMetrics(size_t evt_number);

    void SendMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics);
    void SendMetric(std::vector<uint32_t> &metric_vec, uint32_t metric_id);
    void SetMetrics(uint32_t charge_metric, uint32_t light_metric);

    std::shared_ptr<TCPConnection> command_client_;
    std::shared_ptr<TCPConnection> status_client_;
    std::unique_ptr<ProcessEvents> process_events_;

    std::mt19937 random_generator_;

    constexpr static uint16_t light_slot_ = 16;
    static constexpr uint32_t kTelemFemHeader = 0x4004;
    static constexpr uint32_t kTelemFullEventComplete = 0x4005;
    static constexpr uint32_t kSamplesPerFrame = 256;
    static constexpr uint32_t kQueryEventPacketDelayMs = 50;
    // Pace TCP status packets so Hub/Starlink queues are not flooded.
    static constexpr uint32_t kFullEventPacketDelayMs = 1;
    const size_t events_per_file = 5;
    std::vector<size_t> selected_events_;
    constexpr static int event_min = 0, event_max = 5000;
    constexpr static int charge_min = 0, charge_max = 188;
    constexpr static int light_min = 0, light_max = 32;
    std::uniform_int_distribution<size_t> event_distrib_;
    std::uniform_int_distribution<uint16_t> charge_channel_distrib_;
    std::uniform_int_distribution<uint16_t> light_channel_distrib_;

    std::string data_basedir_;
    std::string data_ssd0_dir_;
    std::string data_ssd1_dir_;

    std::atomic_bool is_running_;
    std::atomic_bool is_decoding_;

    std::thread decode_thread_;
    std::thread continuous_lbw_thread_;
    std::atomic_bool continuous_lbw_running_{false};
    std::mutex process_mutex_;

    uint32_t continuous_run_request_ = kAutoRun;
    uint32_t continuous_file_request_ = kAutoFile;
    uint32_t continuous_stride_ = 1;
    uint32_t continuous_period_sec_ = kMinPeriodSec;
    uint32_t continuous_next_evt_ = 0;
    uint32_t continuous_resolved_run_ = 0;
    uint32_t continuous_resolved_file_ = 0;
    std::string continuous_open_path_;
    bool continuous_file_open_ = false;
    bool continuous_had_opened_file_ = false;
    bool continuous_auto_run_ = true;
    bool continuous_auto_file_ = true;

    size_t process_num_events_;
    size_t event_stride_ = 500;
    constexpr static size_t EVENT_LOOP_MAX = 10000;

    LowBwTpcMonitor lbw_metrics_;
    TpcMonitor metrics_;
    TpcMonitorChargeEvent charge_event_metric_;
    TpcMonitorLightEvent light_event_metric_;
    TpcMonitorFemHeader fem_header_metric_;
    TpcMonitorFullEventComplete full_event_complete_metric_;

    LightAlgs light_algs_;
    ChargeAlgs charge_algs_;

    uint32_t charge_metric_;
    uint32_t light_metric_;

    std::string monitor_file_;

    enum ControlCmds : uint16_t {
        kMinimalQuery = 1,
        kStopDecoder = 2,
        kDecodeEvent = 3
    };

    std::function<void(EventStruct&)> metric_creator_;
    std::function<void(size_t evt_number)> update_metrics_;

    std::atomic_bool debug_;
    bool choose_random_ = false;
    size_t num_light_rois_ = 0;
    uint32_t run_number_ = 0;
    uint32_t file_number_ = 0;

};

} // data_monitor

#endif //DATA_MONITOR_H
