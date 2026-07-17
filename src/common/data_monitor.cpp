//
// Created by Jon Sensenig on 5/5/25.
//

#include "data_monitor.h"
#include <iostream>
#include <random>
#include <filesystem>
#include <regex>
#include <chrono>
#include <algorithm>
#include <cstdlib>

namespace data_monitor {

namespace {

std::string JoinPath(const std::string& base, const std::string& leaf) {
    if (base.empty()) {
        return leaf;
    }
    if (base.back() == '/') {
        return base + leaf;
    }
    return base + "/" + leaf;
}

std::string EnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
}

} // namespace

    DataMonitor::DataMonitor(asio::io_context& io_context, const std::string& ip_address, const uint16_t command_port,
                             const uint16_t status_port, bool is_server, bool is_running) :
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
        StopContinuousLbw();
        process_events_.reset();
    }

    void DataMonitor::GetEnvVariables() {
        data_basedir_ = EnvOrEmpty("DATA_BASE_DIR");
        data_ssd0_dir_ = EnvOrEmpty("DATA_SSD0_DIR");
        data_ssd1_dir_ = EnvOrEmpty("DATA_SSD1_DIR");

        if (data_basedir_.empty()) {
            std::cerr << "Environment variable DATA_BASE_DIR does not exist!" << std::endl;
        }
        std::cout << "Data base directory: " << data_basedir_ << std::endl;
        std::cout << "Data SSD0 directory: " << data_ssd0_dir_ << std::endl;
        std::cout << "Data SSD1 directory: " << data_ssd1_dir_ << std::endl;
        for (const auto& dir : ReadoutSearchDirs()) {
            std::cout << "Readout search directory: " << dir << std::endl;
        }
    }

    bool DataMonitor::IsAutoRun(const uint32_t run) {
        return run == kAutoRun;
    }

    bool DataMonitor::IsAutoFile(const uint32_t file) {
        return file == kAutoFile;
    }

    std::vector<std::string> DataMonitor::ReadoutSearchDirs() const {
        std::vector<std::string> dirs;
        auto add_dir = [&](const std::string& base) {
            if (base.empty()) {
                return;
            }
            const std::string readout_dir = JoinPath(base, "readout_data");
            if (std::find(dirs.begin(), dirs.end(), readout_dir) == dirs.end()) {
                dirs.push_back(readout_dir);
            }
        };

        add_dir(data_ssd0_dir_);
        add_dir(data_ssd1_dir_);
        add_dir(data_basedir_);
        return dirs;
    }

    std::string DataMonitor::BuildMonitorFilePath(const std::string& readout_dir, const uint32_t run,
                                                  const uint32_t file) const {
        const std::string file_name = "pGRAMS_bin_" + std::to_string(run) + "_" + std::to_string(file) + ".dat";
        return JoinPath(readout_dir, file_name);
    }

    std::vector<ReadoutFileCandidate> DataMonitor::CollectReadoutFiles(const uint32_t run_filter) const {
        namespace fs = std::filesystem;
        static const std::regex pattern(R"(pGRAMS_bin_(\d+)_(\d+)\.dat)");
        std::vector<ReadoutFileCandidate> files;

        for (const auto& readout_dir : ReadoutSearchDirs()) {
            const fs::path dir_path(readout_dir);
            if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(dir_path)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::string name = entry.path().filename().string();
                std::smatch match;
                if (!std::regex_match(name, match, pattern)) {
                    continue;
                }
                const uint32_t run = static_cast<uint32_t>(std::stoul(match[1].str()));
                const uint32_t file = static_cast<uint32_t>(std::stoul(match[2].str()));
                if (!IsAutoRun(run_filter) && run != run_filter) {
                    continue;
                }
                files.push_back({run, file, entry.path().string()});
            }
        }

        std::sort(files.begin(), files.end(), [](const ReadoutFileCandidate& a, const ReadoutFileCandidate& b) {
            if (a.run != b.run) {
                return a.run < b.run;
            }
            return a.file < b.file;
        });
        return files;
    }

    std::optional<ReadoutFileCandidate> DataMonitor::FindExplicitReadoutFile(const uint32_t run,
                                                                             const uint32_t file) const {
        namespace fs = std::filesystem;
        const auto files = CollectReadoutFiles(run);
        std::optional<ReadoutFileCandidate> best;
        fs::file_time_type best_time{};
        for (const auto& entry : files) {
            if (entry.run != run || entry.file != file) {
                continue;
            }
            const fs::file_time_type mtime = fs::last_write_time(entry.path);
            if (!best.has_value() || mtime > best_time) {
                best = entry;
                best_time = mtime;
            }
        }
        return best;
    }

    std::optional<ReadoutFileCandidate> DataMonitor::FindClosedReadoutFile(const uint32_t run_filter,
                                                                           const uint32_t file_filter,
                                                                           const uint32_t min_file_exclusive) const {
        const auto files = CollectReadoutFiles(run_filter);
        if (files.empty()) {
            return std::nullopt;
        }

        auto pick_closed_for_run = [&](const uint32_t run) -> std::optional<ReadoutFileCandidate> {
            uint32_t max_file = 0;
            std::optional<ReadoutFileCandidate> max_file_entry;
            for (const auto& entry : files) {
                if (entry.run != run) {
                    continue;
                }
                if (!max_file_entry.has_value() || entry.file > max_file) {
                    max_file = entry.file;
                    max_file_entry = entry;
                }
            }
            if (!max_file_entry.has_value()) {
                return std::nullopt;
            }
            if (max_file == 0) {
                return std::nullopt;
            }
            const uint32_t closed_file = max_file - 1;
            if (closed_file <= min_file_exclusive) {
                return std::nullopt;
            }
            if (!IsAutoFile(file_filter) && closed_file != file_filter) {
                return std::nullopt;
            }
            return FindExplicitReadoutFile(run, closed_file);
        };

        if (!IsAutoRun(run_filter)) {
            return pick_closed_for_run(run_filter);
        }

        std::optional<ReadoutFileCandidate> best;
        std::vector<uint32_t> runs;
        runs.reserve(files.size());
        for (const auto& entry : files) {
            if (std::find(runs.begin(), runs.end(), entry.run) == runs.end()) {
                runs.push_back(entry.run);
            }
        }

        for (const uint32_t run : runs) {
            const auto candidate = pick_closed_for_run(run);
            if (!candidate.has_value()) {
                continue;
            }
            if (!best.has_value() || candidate->run > best->run ||
                (candidate->run == best->run && candidate->file > best->file)) {
                best = candidate;
            }
        }
        return best;
    }

    std::optional<ReadoutFileCandidate> DataMonitor::FindInitialClosedReadoutFile(const uint32_t run_filter) const {
        return FindClosedReadoutFile(run_filter, kAutoFile, 0);
    }

    std::optional<ReadoutFileCandidate> DataMonitor::FindNextClosedReadoutFile(const uint32_t run_filter,
                                                                               const uint32_t after_run,
                                                                               const uint32_t after_file) const {
        const auto files = CollectReadoutFiles(run_filter);
        if (files.empty()) {
            return std::nullopt;
        }

        std::optional<ReadoutFileCandidate> best_next;
        std::vector<uint32_t> runs;
        runs.reserve(files.size());
        for (const auto& entry : files) {
            if (std::find(runs.begin(), runs.end(), entry.run) == runs.end()) {
                runs.push_back(entry.run);
            }
        }

        auto consider_run = [&](const uint32_t run) {
            uint32_t max_file = 0;
            for (const auto& entry : files) {
                if (entry.run == run && entry.file > max_file) {
                    max_file = entry.file;
                }
            }
            if (max_file == 0) {
                return;
            }
            const uint32_t closed_file = max_file - 1;
            const bool is_after = run > after_run || (run == after_run && closed_file > after_file);
            if (!is_after) {
                return;
            }
            const auto closed_entry = FindExplicitReadoutFile(run, closed_file);
            if (!closed_entry.has_value()) {
                return;
            }
            if (!best_next.has_value() || closed_entry->run < best_next->run ||
                (closed_entry->run == best_next->run && closed_entry->file < best_next->file)) {
                best_next = closed_entry;
            }
        };

        if (!IsAutoRun(run_filter)) {
            consider_run(run_filter);
            return best_next;
        }

        for (const uint32_t run : runs) {
            consider_run(run);
        }
        return best_next;
    }

    bool DataMonitor::ResolveMonitorTarget(const uint32_t run, const uint32_t file, uint32_t& run_out,
                                           uint32_t& file_out, std::string& path_out) const {
        if (!IsAutoRun(run) && !IsAutoFile(file)) {
            const auto explicit_file = FindExplicitReadoutFile(run, file);
            if (!explicit_file.has_value()) {
                return false;
            }
            run_out = explicit_file->run;
            file_out = explicit_file->file;
            path_out = explicit_file->path;
            return true;
        }

        const auto closed = FindClosedReadoutFile(run, file, 0);
        if (!closed.has_value()) {
            return false;
        }
        run_out = closed->run;
        file_out = closed->file;
        path_out = closed->path;
        return true;
    }

    void DataMonitor::SetRunning(const bool run) {
        is_running_.store(run);
        if (!run) {
            StopContinuousLbw();
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
        if (process_num_events_ > 5000) process_num_events_ = 5000;
    }

    void DataMonitor::setEventNumber(std::vector<uint32_t> &args) {
        uint32_t event_number = args.at(2);
        event_stride_ = event_number;
        event_stride_ = event_stride_ == 0 ? event_stride_ + 1 : event_stride_;
        process_num_events_ = event_number + 1;
        if (process_num_events_ > 5000) process_num_events_ = 5000;
    }

    void DataMonitor::setFileName(std::vector<uint32_t> &args) {
        run_number_ = args.at(0);
        file_number_ = args.at(1);
        uint32_t resolved_run = run_number_;
        uint32_t resolved_file = file_number_;
        if (!ResolveMonitorTarget(run_number_, file_number_, resolved_run, resolved_file, monitor_file_)) {
            monitor_file_.clear();
            return;
        }
        run_number_ = resolved_run;
        file_number_ = resolved_file;
        std::cout << "Requested file: " << monitor_file_ << std::endl;
    }

    void DataMonitor::RunLbQueryOnCurrentTarget() {
        metric_creator_ = [this](EventStruct& evt) { this->CreateMinimalMetrics(evt); };
        update_metrics_ = [this](size_t evt_number) { this->UpdateMinimalMetrics(evt_number); };
        ProcessFile();
    }

    void DataMonitor::StopContinuousLbwLocked(const char* reason) {
        continuous_lbw_running_.store(false);
        if (reason != nullptr) {
            std::cout << "Continuous LBW stopping: " << reason << std::endl;
        }
    }

    void DataMonitor::StartContinuousLbw(const std::vector<uint32_t>& args) {
        if (args.size() < 4) {
            std::cerr << "Start continuous LBW requires 4 arguments: period_sec run file event_stride" << std::endl;
            return;
        }

        StopContinuousLbw();

        continuous_period_sec_ = args.at(0);
        if (continuous_period_sec_ < kMinPeriodSec) {
            continuous_period_sec_ = kMinPeriodSec;
        } else if (continuous_period_sec_ > kMaxPeriodSec) {
            continuous_period_sec_ = kMaxPeriodSec;
        }

        continuous_run_request_ = args.at(1);
        if (IsAutoRun(continuous_run_request_)) {
            continuous_file_request_ = kAutoFile;
        } else {
            continuous_file_request_ = args.at(2);
        }
        continuous_stride_ = args.at(3);
        if (continuous_stride_ == 0) {
            continuous_stride_ = 1;
        }

        continuous_auto_run_ = IsAutoRun(continuous_run_request_);
        continuous_auto_file_ = continuous_auto_run_ || IsAutoFile(continuous_file_request_);
        continuous_next_evt_ = 0;
        continuous_resolved_run_ = 0;
        continuous_resolved_file_ = 0;
        continuous_file_open_ = false;
        continuous_had_opened_file_ = false;
        continuous_open_path_.clear();

        continuous_lbw_running_.store(true);
        continuous_lbw_thread_ = std::thread([this]() { ContinuousLbwLoop(); });

        std::cout << "Started continuous LBW: period=" << continuous_period_sec_
                  << "s run=" << continuous_run_request_
                  << " file=" << continuous_file_request_
                  << " stride=" << continuous_stride_ << std::endl;
    }

    void DataMonitor::StopContinuousLbw() {
        if (!continuous_lbw_running_.load()) {
            return;
        }
        continuous_lbw_running_.store(false);
        if (continuous_lbw_thread_.joinable()) {
            continuous_lbw_thread_.join();
        }
        continuous_file_open_ = false;
        continuous_open_path_.clear();
        std::cout << "Stopped continuous LBW" << std::endl;
    }

    bool DataMonitor::OpenContinuousFile(const ReadoutFileCandidate& target) {
        if (continuous_file_open_ && continuous_open_path_ == target.path) {
            return true;
        }

        process_events_->CloseFile();
        if (!process_events_->OpenFile(target.path)) {
            std::cerr << "Continuous LBW: failed to open " << target.path << std::endl;
            continuous_file_open_ = false;
            return false;
        }
        process_events_->RestartFile();
        process_events_->UseEventStride(false);

        continuous_open_path_ = target.path;
        continuous_resolved_run_ = target.run;
        continuous_resolved_file_ = target.file;
        continuous_file_open_ = true;
        continuous_had_opened_file_ = true;
        continuous_next_evt_ = 0;
        run_number_ = target.run;
        file_number_ = target.file;
        monitor_file_ = target.path;

        std::cout << "Continuous LBW opened " << target.path << std::endl;
        return true;
    }

    bool DataMonitor::ProcessAndSendOneLbEvent() {
        const uint32_t target_evt = continuous_next_evt_;

        while (process_events_->GetEvent()) {
            const size_t evt_idx = process_events_->GetLastEventIndex();
            if (evt_idx != target_evt) {
                continue;
            }

            EventStruct evt_data = process_events_->GetEventStruct();
            CreateMinimalMetrics(evt_data);
            UpdateMinimalMetrics(evt_idx);
            charge_algs_.Clear();
            light_algs_.Clear();
            continuous_next_evt_ += continuous_stride_;
            return true;
        }
        return false;
    }

    bool DataMonitor::IsFixedContinuousTarget() const {
        return !continuous_auto_run_ && !continuous_auto_file_;
    }

    void DataMonitor::ReleaseContinuousFile() {
        if (continuous_file_open_) {
            process_events_->CloseFile();
        }
        continuous_file_open_ = false;
        continuous_open_path_.clear();
    }

    bool DataMonitor::TryOpenInitialContinuousFile() {
        std::optional<ReadoutFileCandidate> target;
        if (IsFixedContinuousTarget()) {
            target = FindExplicitReadoutFile(continuous_run_request_, continuous_file_request_);
        } else {
            const uint32_t run_filter = continuous_auto_run_ ? kAutoRun : continuous_run_request_;
            if (!continuous_had_opened_file_) {
                target = FindInitialClosedReadoutFile(run_filter);
            } else {
                target = FindNextClosedReadoutFile(run_filter, continuous_resolved_run_, continuous_resolved_file_);
            }
        }

        if (!target.has_value()) {
            return false;
        }
        return OpenContinuousFile(*target);
    }

    bool DataMonitor::AdvanceContinuousToNextClosedFile() {
        if (IsFixedContinuousTarget()) {
            return false;
        }

        const uint32_t run_filter = continuous_auto_run_ ? kAutoRun : continuous_run_request_;
        const auto next = FindNextClosedReadoutFile(run_filter, continuous_resolved_run_, continuous_resolved_file_);
        if (!next.has_value() || next->path == continuous_open_path_) {
            return false;
        }
        return OpenContinuousFile(*next);
    }

    void DataMonitor::ContinuousLbwLoop() {
        while (continuous_lbw_running_.load()) {
            bool waiting_for_file = false;
            {
                std::lock_guard<std::mutex> lock(process_mutex_);

                if (!continuous_file_open_) {
                    if (!TryOpenInitialContinuousFile()) {
                        if (IsFixedContinuousTarget()) {
                            StopContinuousLbwLocked("fixed run/file not available");
                            break;
                        }
                        waiting_for_file = true;
                    }
                }

                if (continuous_file_open_ && !waiting_for_file) {
                    if (ProcessAndSendOneLbEvent()) {
                        std::cout << "Continuous LBW sent run=" << continuous_resolved_run_
                                  << " file=" << continuous_resolved_file_
                                  << " evt=" << (continuous_next_evt_ >= continuous_stride_
                                                  ? continuous_next_evt_ - continuous_stride_
                                                  : 0) << std::endl;
                    } else {
                        if (IsFixedContinuousTarget()) {
                            StopContinuousLbwLocked("fixed run/file exhausted");
                            break;
                        }
                        ReleaseContinuousFile();
                        if (!AdvanceContinuousToNextClosedFile()) {
                            waiting_for_file = true;
                            std::cout << "Continuous LBW waiting for next closed readout file (run="
                                      << continuous_resolved_run_ << " file=" << continuous_resolved_file_
                                      << ")" << std::endl;
                        }
                    }
                } else if (waiting_for_file && !IsFixedContinuousTarget()) {
                    std::cout << "Continuous LBW waiting for closed readout file..." << std::endl;
                }
            }

            if (!continuous_lbw_running_.load()) {
                break;
            }

            for (uint32_t slept = 0; slept < continuous_period_sec_ && continuous_lbw_running_.load(); ++slept) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void DataMonitor:: HandleCommand(Command& cmd) {
        std::cout << "Received command: 0x" << std::hex << cmd.command << std::dec << std::endl;
        switch (cmd.command) {
            case static_cast<int>(CommunicationCodes::TPCMonitor_Query_LB_Data): {
                if (cmd.arguments.size() < 4) break;
                {
                    std::lock_guard<std::mutex> lock(process_mutex_);
                    setFileName(cmd.arguments);
                    setNumEvent(cmd.arguments);
                    RunLbQueryOnCurrentTarget();
                }
                break;
            }
            case static_cast<int>(CommunicationCodes::TPCMonitor_Query_Event_Data): {
                if (cmd.arguments.size() < 4) break;
                {
                    std::lock_guard<std::mutex> lock(process_mutex_);
                    setFileName(cmd.arguments);
                    setEventNumber(cmd.arguments);
                    choose_random_ = cmd.arguments.at(3) == 1;

                    metric_creator_ = [this](EventStruct& evt) { this->CreateEventMetrics(evt); };
                    update_metrics_ = [this](size_t evt_number) { this->UpdateEventMetrics(evt_number); };
                    ProcessFile();
                }
                break;
            }
            case static_cast<int>(CommunicationCodes::TPCMonitor_Start_Continuous_LBW): {
                StartContinuousLbw(cmd.arguments);
                break;
            }
            case static_cast<int>(CommunicationCodes::TPCMonitor_Stop_Continuous_LBW): {
                StopContinuousLbw();
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
        if (monitor_file_.empty()) {
            std::cerr << "No monitor file resolved!" << std::endl;
            return;
        }
        process_events_->CloseFile();
        if (!process_events_->OpenFile(monitor_file_)) {
            std::cerr << "Failed to load file!" << std::endl;
            return;
        }
        GetEventMetrics();
    }

    void DataMonitor::GetEventMetrics() {
        if (debug_) std::cout << "entering processing" << std::endl;
        process_events_->SetEventStride(event_stride_);

        size_t event_count = 0;
        while (process_events_->GetEvent() && (event_count < process_num_events_) && (event_count < EVENT_LOOP_MAX)) {
            if ((event_count % event_stride_) != 0 || (event_count == 0 && process_num_events_ != 1)) {
                event_count++;
                continue;
            }
            if (debug_) std::cout << "Processing event: " << event_count << std::endl;
            EventStruct evt_data = process_events_->GetEventStruct();
            metric_creator_(evt_data);
            event_count++;
        }
        update_metrics_(event_count);
    }

    void DataMonitor::SendMetric(std::vector<uint32_t> &metric_vec, uint32_t metric_id) {
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
        lbw_metrics_.setEvtNumber(static_cast<uint32_t>(evt_number));
        charge_algs_.UpdateMinimalMetrics(lbw_metrics_, metrics_);
        if (debug_) std::cout << "Updated charge.." << std::endl;
        light_algs_.UpdateMinimalMetrics(lbw_metrics_, metrics_);

        auto tmp_vec = lbw_metrics_.serialize();
        SendMetric(tmp_vec, 0x4001);

        if (debug_) std::cout << "Updated light.." << std::endl;
        if (debug_) lbw_metrics_.print();
        charge_algs_.Clear();
        light_algs_.Clear();
    }

    void DataMonitor::CreateEventMetrics(EventStruct & event) {
        charge_algs_.GetChargeEvent(event);
        if (debug_) std::cout << "Processed charge event.." << std::endl;
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
        charge_algs_.Clear();
        light_algs_.Clear();
    }

} // data_monitor
