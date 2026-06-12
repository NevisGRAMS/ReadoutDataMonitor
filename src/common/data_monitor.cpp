//
// Created by Jon Sensenig on 5/5/25.
//

#include "data_monitor.h"
#include <iostream>
#include <random>

namespace data_monitor {

    DataMonitor::DataMonitor(asio::io_context& io_context, const std::string& ip_address, const uint16_t command_port,
                             const uint16_t status_port, bool is_server, bool is_running) :
    // command_client_(io_context, ip_address, command_port, is_server, true, false),
    // status_client_(io_context, ip_address, status_port, is_server, false, true),
    random_generator_(std::random_device()()),
    event_distrib_(event_min, event_max),
    charge_channel_distrib_(charge_min,charge_max),
    light_channel_distrib_(light_min, light_max),
    light_algs_(),
    charge_algs_(),
    debug_(true)
    {
        std::cout << "DM" << std::endl;
        command_client_ = std::make_shared<TCPConnection>(io_context, ip_address, command_port, is_server, true, false);
        status_client_ = std::make_shared<TCPConnection>(io_context, ip_address, status_port, is_server, false, true);
        command_client_->Start();
        status_client_->Start();
        process_events_ = std::make_unique<ProcessEvents>(light_slot_, false, std::vector<uint16_t>(), false);
        process_events_->UseEventStride(true);
        GetEnvVariables();
        std::cout << "DM End" << std::endl;
    }

    DataMonitor::~DataMonitor() {
        // TODO make sure this doesn't leak memory
        // if this class is destructed with an open file
        process_events_.reset();
    }

    void DataMonitor::GetEnvVariables() {
        const char *data_basedir = std::getenv("DATA_BASE_DIR");

        if (data_basedir != nullptr) {
            data_basedir_ = data_basedir;
        } else {
            std::cerr << "Environment variable DATA_BASE_DIR does not exist!" << std::endl;
        }   
        std::cout << "Data base directory: " << data_basedir_ << std::endl;
    } 

    void DataMonitor::SetRunning(const bool run) {
        is_running_.store(run);
        if (!run) {
            command_client_->setStopCmdRead();
            status_client_->setStopCmdRead();
        }
    }
   
    void DataMonitor::Run() {
        is_running_ = true;
        ReceiveCommand();
    }

    void DataMonitor::ReceiveCommand() {
        while (is_running_.load()) {
            Command cmd = command_client_->ReadRecvBuffer();
            HandleCommand(cmd);
        }
    }

    void DataMonitor::setNumEvent(std::vector<uint32_t> &args) {
        uint32_t num_events_ = args.at(2);
        event_stride_ = args.at(3);
        event_stride_ = event_stride_ == 0 ? event_stride_ + 1 : event_stride_;
        process_num_events_ = num_events_ * event_stride_;
        // 5k events per file so return error if requesting more
        // TODO set error bit
        if (process_num_events_ > 5000) process_num_events_ = 5000;
    }

    void DataMonitor::setEventNumber(std::vector<uint32_t> &args) {
        uint32_t event_number = args.at(2);
        event_stride_ = event_number;
        event_stride_ = event_stride_ == 0 ? event_stride_ + 1 : event_stride_;
        process_num_events_ = event_number + 1;
        // TODO set error bit
        if (process_num_events_ > 5000) process_num_events_ = 5000;
    }

    void DataMonitor::setFileName(std::vector<uint32_t> &args) {
        run_number_ = args.at(0);
        file_number_ = args.at(1);
        monitor_file_ = data_basedir_ + "pGRAMS_bin_" + std::to_string(run_number_) + "_" + std::to_string(file_number_) + ".dat";
        std::cout << "Requested file: " << monitor_file_ << std::endl;
    }

    void DataMonitor:: HandleCommand(Command& cmd) {
        std::cout << "Received command: 0x" << std::hex << cmd.command << std::dec << std::endl;
        switch (cmd.command) {
            case static_cast<int>(CommunicationCodes::TPCMonitor_Query_LB_Data): {
                // Create file name
                if (cmd.arguments.size() < 4) break;
                setFileName(cmd.arguments);
                setNumEvent(cmd.arguments);
                // Set the functions to process events, create metrics and update them
                // since the class members have an implicit this pointer to the current instance of the
                // class we have to make it explicit with a lambda function. Repeated for each case below.
                metric_creator_ = [this](EventStruct& evt) { this->CreateMinimalMetrics(evt); };
                update_metrics_ = [this](size_t evt_number) { this->UpdateMinimalMetrics(evt_number); };
                ProcessFile();
                break;
            }
            case static_cast<int>(CommunicationCodes::TPCMonitor_Query_Event_Data): {
                // Create file name
                if (cmd.arguments.size() < 4) break;
                setFileName(cmd.arguments);
                // We want to take one stride to the event we want, process it and quit.
                setEventNumber(cmd.arguments);
                choose_random_ = cmd.arguments.at(3) == 1;

                metric_creator_ = [this](EventStruct& evt) { this->CreateEventMetrics(evt); };
                update_metrics_ = [this](size_t evt_number) { this->UpdateEventMetrics(evt_number); };
                ProcessFile();
                break;
            }
            case kDecodeEvent: {
                break;
            }
            default: {
                std::cerr << "Unknown command: 0x" << std::hex << cmd.command << std::dec << std::endl;
            }
        }
    }

    void SetMetrics(uint32_t charge_metric, uint32_t light_metric) {
        switch (charge_metric) {
            case 0x1: {
                // TpcMonitor charge_monitor_;
                break;
            }
            default: {
                std::cerr << "Unknown charge metric: 0x" << std::hex << charge_metric << std::dec << std::endl;
            }
        }
        switch (light_metric) {
            case 0x1: {
                break;
            }
            default: {
                std::cerr << "Unknown light metric: 0x" << std::hex << charge_metric << std::dec << std::endl;
            }
        }
    }

    void DataMonitor::ProcessFile() {
        // if (!OpenFile()) { return; }
        if (!process_events_->OpenFile(monitor_file_)) {
            std::cerr << "Failed to load file!" << std::endl;
        }
        GetEventMetrics();
    }

    void DataMonitor::GetEventMetrics() {
        if (debug_) std::cout << "entering processing" << std::endl;
        // Set the decoder stride. When requesting 1 event we want to make one stride directly to the
        // desired event. This is because in the decoder striding skips filling the data structure for
        // the intermediate events. Thus, it is more efficient to stride than to get each event.
        process_events_->SetEventStride(event_stride_);

        // Loop through desired events, either adjacent events or with some stride
        size_t event_count = 0;
        while (process_events_->GetEvent() && (event_count < process_num_events_) && (event_count < EVENT_LOOP_MAX)) {
            // the decoder must iterate through each event since we don't know a priori the event size
            if ((event_count % event_stride_) != 0 || (event_count == 0 && process_num_events_ != 1)) {
                event_count++;
                continue;
            }
            if (debug_) std::cout << "Processing event: " << event_count << std::endl;
            EventStruct evt_data = process_events_->GetEventStruct();
            // Calculate event metrics
            metric_creator_(evt_data);
            event_count++;
        }
        // The number of desired events have been processed and metrics created
        // so update the metrics and send them
        update_metrics_(event_count);
    }

    void DataMonitor::SendMetric(std::vector<uint32_t> &metric_vec, uint32_t metric_id) {
        // Send the metrics
        Command lbw_cmd(metric_id, metric_vec.size());
        lbw_cmd.arguments = std::move(metric_vec);
        status_client_->WriteSendBuffer(lbw_cmd);
        std::cout << "Sent metrics.." << std::endl;
    }

    void DataMonitor::CreateMinimalMetrics(EventStruct & event) {
        charge_algs_.MinimalSummary(event);
        if (debug_) std::cout << "Processed charge.." << std::endl;
        light_algs_.MinimalSummary(event);
        if (debug_) std::cout << "Processed light.." << std::endl;
    }

    void DataMonitor::UpdateMinimalMetrics(size_t evt_number) {
        lbw_metrics_.setRunNumber(run_number_);
        lbw_metrics_.setFileNumber(file_number_);
        lbw_metrics_.setEvtNumber(0); // set to 0 since the metric is an aggregate across events
        charge_algs_.UpdateMinimalMetrics(lbw_metrics_, metrics_);
        if (debug_) std::cout << "Updated charge.." << std::endl;
        light_algs_.UpdateMinimalMetrics(lbw_metrics_, metrics_);

        auto tmp_vec = lbw_metrics_.serialize();
        SendMetric(tmp_vec, 0x4001);

        if (debug_) std::cout << "Updated light.." << std::endl;
        if (debug_) lbw_metrics_.print();
        // Clear the metrics for the next file
        charge_algs_.Clear();
        light_algs_.Clear();
    }

    void DataMonitor::CreateEventMetrics(EventStruct & event) {
        charge_algs_.GetChargeEvent(event);
        if (debug_) std::cout << "Processed charge event.." << std::endl;
        //num_light_rois_ = light_algs_.GetLightEvent(event);
        if (debug_) std::cout << "Processed light event.." << std::endl;
    }

    void DataMonitor::UpdateEventMetrics(size_t evt_number) {
        if (debug_) std::cout << "Updating Event Metrics.." << std::endl;
        charge_event_metric_.setRunNumber(run_number_);
        charge_event_metric_.setFileNumber(file_number_);
        charge_event_metric_.setEvtNumber(evt_number);
        light_event_metric_.setRunNumber(run_number_);
        light_event_metric_.setFileNumber(file_number_);
        light_event_metric_.setEvtNumber(evt_number);
        if (choose_random_) {
            auto charge_uniform = std::uniform_int_distribution<size_t>(0, NUM_CHARGE_CHANNELS);
            size_t charge_channel = charge_uniform(random_generator_);
            if (debug_) std::cout << "Random charge ch: " << charge_channel << std::endl;
            auto tmp_vec = charge_algs_.UpdateChargeEvent(charge_event_metric_, charge_channel);
            SendMetric(tmp_vec, 0x4002);

            auto light_uniform = std::uniform_int_distribution<size_t>(0, num_light_rois_);
            size_t light_roi = light_uniform(random_generator_);
            if (debug_) std::cout << "Random light roi: " << light_roi << std::endl;
            if (light_algs_.isLightRoi()) {
                tmp_vec = light_algs_.UpdateLightEvent(light_event_metric_, light_roi);
                SendMetric(tmp_vec, 0x4003);
            }
        } else {
            for (size_t i = 0; i < NUM_CHARGE_CHANNELS; i++) {
                auto tmp_vec = charge_algs_.UpdateChargeEvent(charge_event_metric_, i);
                if (debug_) std::cout << "Updated charge event.." << std::endl;
                SendMetric(tmp_vec, 0x4002);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            for (size_t i = 0; i < num_light_rois_; i++) {
                auto tmp_vec = light_algs_.UpdateLightEvent(light_event_metric_, i);
                if (debug_) std::cout << "Updated light event.." << std::endl;
                SendMetric(tmp_vec, 0x4003);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        // Make sure to clear it
        charge_algs_.Clear();
        light_algs_.Clear();
    }

} // data_monitor
