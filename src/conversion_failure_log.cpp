#include "conversion_failure_log.hpp"

#include "duckdb/common/printer.hpp"

#include "tracing.hpp"

#include <sstream>

namespace erpl_web {

namespace {

constexpr size_t MAX_COLUMNS_IN_SUMMARY = 8;

} // namespace

void ConversionFailureLog::SetStrictTyping(bool strict)
{
    std::lock_guard<std::mutex> lock(mutex_);
    strict_typing_ = strict;
}

bool ConversionFailureLog::IsStrictTyping() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return strict_typing_;
}

std::string ConversionFailureLog::Truncate(const std::string &value)
{
    if (value.size() <= MAX_OFFENDING_VALUE_LENGTH) {
        return value;
    }
    return value.substr(0, MAX_OFFENDING_VALUE_LENGTH) + "...";
}

void ConversionFailureLog::RecordFailure(const std::string &column_name,
                                         const std::string &offending_value,
                                         const std::string &error_message)
{
    const auto column = column_name.empty() ? std::string("<unknown column>") : column_name;
    const auto value = Truncate(offending_value);

    bool strict = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        strict = strict_typing_;
        if (!strict) {
            auto it = column_index_.find(column);
            if (it == column_index_.end()) {
                column_index_.emplace(column, failures_.size());
                failures_.push_back(ConversionFailure {column, 1, value, error_message});
            } else {
                failures_[it->second].failure_count++;
            }
        }
    }

    if (strict) {
        throw StrictTypingViolation("Failed to convert value for column '" + column + "' (value: " + value +
                                    "): " + error_message +
                                    ". Set strict_typing=false to receive NULL for this cell instead.");
    }
}

bool ConversionFailureLog::HasFailures() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !failures_.empty();
}

uint64_t ConversionFailureLog::TotalFailureCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const auto &failure : failures_) {
        total += failure.failure_count;
    }
    return total;
}

std::vector<ConversionFailure> ConversionFailureLog::Drain()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto drained = std::move(failures_);
    failures_.clear();
    column_index_.clear();
    return drained;
}

std::string ConversionFailureLog::FormatSummary(const std::vector<ConversionFailure> &failures,
                                                const std::string &source)
{
    if (failures.empty()) {
        return std::string();
    }

    uint64_t total = 0;
    for (const auto &failure : failures) {
        total += failure.failure_count;
    }

    std::ostringstream out;
    out << "erpl_odata warning: " << total << " value" << (total == 1 ? "" : "s") << " in " << failures.size()
        << " column" << (failures.size() == 1 ? "" : "s") << " could not be converted and were returned as NULL";
    if (!source.empty()) {
        out << " (" << source << ")";
    }
    out << ".";

    size_t reported = 0;
    for (const auto &failure : failures) {
        if (reported == MAX_COLUMNS_IN_SUMMARY) {
            out << "\n  ... and " << (failures.size() - reported) << " further column(s).";
            break;
        }
        out << "\n  column '" << failure.column_name << "': " << failure.failure_count << " failure"
            << (failure.failure_count == 1 ? "" : "s") << "; first offending value: " << failure.first_offending_value
            << "; first error: " << failure.first_error_message;
        reported++;
    }
    out << "\n  Pass strict_typing=true to fail the query on the first such value instead.";

    return out.str();
}

void ConversionFailureLog::ReportAndDrain(const std::string &source)
{
    auto failures = Drain();
    if (failures.empty()) {
        return;
    }

    const auto summary = FormatSummary(failures, source);
    ERPL_TRACE_WARN("CONVERSION_FAILURE", summary);
    duckdb::Printer::Print(duckdb::OutputStream::STREAM_STDERR, summary);
}

} // namespace erpl_web
