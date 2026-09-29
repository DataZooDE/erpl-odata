#include "tracing.hpp"
#include <filesystem>
#include <iostream>

namespace erpl_web {

ErplTracer& ErplTracer::Instance() {
    static ErplTracer instance;
    return instance;
}

void ErplTracer::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(trace_mutex);
    this->enabled = enabled;
    
    if (enabled && !trace_file) {
        // Create trace file
        std::filesystem::path trace_path = trace_directory;
        trace_path /= "erpl_odata_trace.log";
        
        trace_file = std::make_unique<std::ofstream>(trace_path, std::ios::app);
        if (trace_file->is_open()) {
            Info("TRACER", "Tracing enabled, writing to: " + trace_path.string());
        } else {
            std::cerr << "Failed to open trace file: " << trace_path.string() << std::endl;
        }
    } else if (!enabled && trace_file) {
        if (trace_file->is_open()) {
            Info("TRACER", "Tracing disabled");
            trace_file->close();
        }
        trace_file.reset();
    }
}

void ErplTracer::SetLevel(TraceLevel level) {
    std::lock_guard<std::mutex> lock(trace_mutex);
    this->level = level;
    Info("TRACER", "Trace level set to: " + LevelToString(level));
}

void ErplTracer::SetTraceDirectory(const std::string& directory) {
    std::lock_guard<std::mutex> lock(trace_mutex);
    trace_directory = directory;
    
    // Create directory if it doesn't exist
    std::filesystem::path dir_path(directory);
    if (!std::filesystem::exists(dir_path)) {
        std::filesystem::create_directories(dir_path);
    }
    
    Info("TRACER", "Trace directory set to: " + directory);
    
    // Reopen trace file if tracing is enabled
    if (enabled && trace_file) {
        trace_file->close();
        std::filesystem::path trace_path = trace_directory;
        trace_path /= "erpl_odata_trace.log";
        trace_file->open(trace_path, std::ios::app);
    }
}

void ErplTracer::SetOutputMode(const std::string& output_mode) {
    std::lock_guard<std::mutex> lock(trace_mutex);
    this->output_mode = output_mode;
    Info("TRACER", "Trace output mode set to: " + output_mode);
}

void ErplTracer::SetMaxFileSize(int64_t max_size) {
    std::lock_guard<std::mutex> lock(trace_mutex);
    max_file_size = max_size;
    Info("TRACER", "Trace max file size set to: " + std::to_string(max_size));
}

void ErplTracer::SetRotation(bool rotation) {
    std::lock_guard<std::mutex> lock(trace_mutex);
    rotation_enabled = rotation;
    Info("TRACER", "Trace rotation " + std::string(rotation ? "enabled" : "disabled"));
}

void ErplTracer::Trace(TraceLevel msg_level, const std::string& component, const std::string& message) {
    if (!enabled || msg_level > level) {
        return;
    }
    
    // More efficient string building
    std::string log_message;
    log_message.reserve(100 + component.length() + message.length()); // Estimate size
    
    log_message += GetTimestamp();
    log_message += " [";
    log_message += LevelToString(msg_level);
    log_message += "] [";
    log_message += component;
    log_message += "] ";
    log_message += message;
    
    std::cout << log_message << std::endl;
    WriteToFile(log_message);
}

void ErplTracer::Trace(TraceLevel msg_level, const std::string& component, const std::string& message, const std::string& data) {
    if (!enabled || msg_level > level) {
        return;
    }
    
    // More efficient string building
    std::string log_message;
    log_message.reserve(100 + component.length() + message.length() + data.length()); // Estimate size
    
    log_message += GetTimestamp();
    log_message += " [";
    log_message += LevelToString(msg_level);
    log_message += "] [";
    log_message += component;
    log_message += "] ";
    log_message += message;
    
    if (!data.empty()) {
        log_message += "\nData: ";
        log_message += data;
    }
    
    std::cout << log_message << std::endl;
    WriteToFile(log_message);
}

void ErplTracer::Error(const std::string& component, const std::string& message) {
    Trace(TraceLevel::ERROR, component, message);
}

void ErplTracer::Error(const std::string& component, const std::string& message, const std::string& data) {
    Trace(TraceLevel::ERROR, component, message, data);
}

void ErplTracer::Warn(const std::string& component, const std::string& message) {
    Trace(TraceLevel::WARN, component, message);
}

void ErplTracer::Warn(const std::string& component, const std::string& message, const std::string& data) {
    Trace(TraceLevel::WARN, component, message, data);
}

void ErplTracer::Info(const std::string& component, const std::string& message) {
    Trace(TraceLevel::INFO, component, message);
}

void ErplTracer::Info(const std::string& component, const std::string& message, const std::string& data) {
    Trace(TraceLevel::INFO, component, message, data);
}

void ErplTracer::Debug(const std::string& component, const std::string& message) {
    Trace(TraceLevel::DEBUG_LEVEL, component, message);
}

void ErplTracer::Debug(const std::string& component, const std::string& message, const std::string& data) {
    Trace(TraceLevel::DEBUG_LEVEL, component, message, data);
}

void ErplTracer::Trace(const std::string& component, const std::string& message) {
    Trace(TraceLevel::TRACE, component, message);
}

void ErplTracer::Trace(const std::string& component, const std::string& message, const std::string& data) {
    Trace(TraceLevel::TRACE, component, message, data);
}

void ErplTracer::WriteToFile(const std::string& message) {
    if (trace_file && trace_file->is_open()) {
        *trace_file << message << std::endl;
        trace_file->flush();
    }
}

std::string ErplTracer::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    // More efficient timestamp generation
    std::string timestamp;
    timestamp.reserve(24); // YYYY-MM-DD HH:MM:SS.mmm format
    
    char time_buffer[32];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
    timestamp = time_buffer;
    timestamp += ".";
    timestamp += std::to_string(ms.count());
    
    return timestamp;
}

std::string ErplTracer::LevelToString(TraceLevel level) {
    switch (level) {
        case TraceLevel::NONE: return "NONE";
        case TraceLevel::ERROR: return "ERROR";
        case TraceLevel::WARN: return "WARN";
        case TraceLevel::INFO: return "INFO";
        case TraceLevel::DEBUG_LEVEL: return "DEBUG";
        case TraceLevel::TRACE: return "TRACE";
        default: return "UNKNOWN";
    }
}

} // namespace erpl_web

// C-style lightweight wrappers to ensure macros don't depend on class symbol
void erpl_trace_error(const std::string &component, const std::string &message) {
    erpl_web::ErplTracer::Instance().Error(component, message);
}

void erpl_trace_error_data(const std::string &component, const std::string &message, const std::string &data) {
    erpl_web::ErplTracer::Instance().Error(component, message, data);
}

void erpl_trace_warn(const std::string &component, const std::string &message) {
    erpl_web::ErplTracer::Instance().Warn(component, message);
}

void erpl_trace_warn_data(const std::string &component, const std::string &message, const std::string &data) {
    erpl_web::ErplTracer::Instance().Warn(component, message, data);
}

void erpl_trace_info(const std::string &component, const std::string &message) {
    erpl_web::ErplTracer::Instance().Info(component, message);
}

void erpl_trace_info_data(const std::string &component, const std::string &message, const std::string &data) {
    erpl_web::ErplTracer::Instance().Info(component, message, data);
}

void erpl_trace_debug(const std::string &component, const std::string &message) {
    erpl_web::ErplTracer::Instance().Debug(component, message);
}

void erpl_trace_debug_data(const std::string &component, const std::string &message, const std::string &data) {
    erpl_web::ErplTracer::Instance().Debug(component, message, data);
}

void erpl_trace_trace(const std::string &component, const std::string &message) {
    erpl_web::ErplTracer::Instance().Trace(component, message);
}

void erpl_trace_trace_data(const std::string &component, const std::string &message, const std::string &data) {
    erpl_web::ErplTracer::Instance().Trace(component, message, data);
}
