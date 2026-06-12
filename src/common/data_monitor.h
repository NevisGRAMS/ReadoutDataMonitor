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
#include <random>
#include <atomic>
#include <thread>
#include <cstdint>
#include <functional>

namespace data_monitor {

    using namespace pgrams::communication;

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

    void GetEnvVariables();
    // Command/Conrol helper fucntions
    void setFileName(std::vector<uint32_t>& args);
    void setNumEvent(std::vector<uint32_t>& args);
    void setEventNumber(std::vector<uint32_t>& args);

    // Minimal metrics
    void CreateMinimalMetrics(EventStruct & event);
    void UpdateMinimalMetrics(size_t evt_number);

    // Send events
    void CreateEventMetrics(EventStruct & event);
    void UpdateEventMetrics(size_t evt_number);

    void SendMetrics(LowBwTpcMonitor &lbw_metrics, TpcMonitor &metrics);
    void SendMetric(std::vector<uint32_t> &metric_vec, uint32_t metric_id);
    void SetMetrics(uint32_t charge_metric, uint32_t light_metric);

    // TCPConnection command_client_;
    // TCPConnection status_client_;
    std::shared_ptr<TCPConnection> command_client_;
    std::shared_ptr<TCPConnection> status_client_;
    std::unique_ptr<ProcessEvents> process_events_;

    // Seed the random number generator
    // std::random_device rd;
    std::mt19937 random_generator_;

    // Create a uniform integer distribution object
    constexpr static uint16_t light_slot_ = 16;
    const size_t events_per_file = 5;
    std::vector<size_t> selected_events_;
    constexpr static int event_min = 0, event_max = 5000;
    constexpr static int charge_min = 0, charge_max = 188;
    constexpr static int light_min = 0, light_max = 32;
    std::uniform_int_distribution<size_t> event_distrib_;
    std::uniform_int_distribution<uint16_t> charge_channel_distrib_;
    std::uniform_int_distribution<uint16_t> light_channel_distrib_;

    // Base directory for data
    std::string data_basedir_{};

    std::atomic_bool is_running_;
    std::atomic_bool is_decoding_;

    std::thread decode_thread_;

    // The event step size, analyze every N events
    size_t process_num_events_;
    size_t event_stride_ = 500;
    // Set a hard upper limit to ensure no infinite loops while decoding
    constexpr static size_t EVENT_LOOP_MAX = 10000;

    // This struct will hold the metrics
    LowBwTpcMonitor lbw_metrics_;
    TpcMonitor metrics_;
    TpcMonitorChargeEvent charge_event_metric_;
    TpcMonitorLightEvent light_event_metric_;

    // Define the metric algorithm classes
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

    // Function to process the data and create metrics
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
