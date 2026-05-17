#pragma once

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace vortex {

class Status {
public:
    enum Code : uint8_t {
        kOk = 0,
        kInvalidArgument,
        kIOError,
        kChecksumMismatch,
        kCorruptIndex,
        kOutOfMemory,
        kInternal,
    };

    Status() : code_(kOk) {}

    bool ok() const { return code_ == kOk; }
    Code code() const { return code_; }
    std::string_view message() const { return msg_; }

    static Status OK() { return {kOk, {}}; }
    static Status InvalidArgument(std::string msg) { return {kInvalidArgument, std::move(msg)}; }
    static Status IOError(std::string msg) { return {kIOError, std::move(msg)}; }
    static Status ChecksumMismatch(std::string msg) { return {kChecksumMismatch, std::move(msg)}; }
    static Status CorruptIndex(std::string msg) { return {kCorruptIndex, std::move(msg)}; }
    static Status OutOfMemory(std::string msg) { return {kOutOfMemory, std::move(msg)}; }
    static Status Internal(std::string msg) { return {kInternal, std::move(msg)}; }

private:
    Status(Code c, std::string msg) : code_(c), msg_(std::move(msg)) {}

    Code code_;
    std::string msg_;
};

template <typename T>
class Result {
public:
    static Result Ok(T value) {
        Result r;
        r.value_ = std::move(value);
        r.status_ = Status::OK();
        return r;
    }

    static Result Err(Status status) {
        assert(!status.ok());
        Result r;
        r.status_ = std::move(status);
        return r;
    }

    bool ok() const { return status_.ok(); }
    const Status& status() const { return status_; }

    T& value() {
        assert(ok());
        return value_;
    }
    const T& value() const {
        assert(ok());
        return value_;
    }
    T&& move_value() {
        assert(ok());
        return std::move(value_);
    }

private:
    Result() = default;
    T value_{};
    Status status_;
};

}  // namespace vortex

#define VORTEX_PANIC(msg)                                              \
    do {                                                                \
        std::cerr << "[PANIC] " << __FILE__ << ":" << __LINE__ << " — " \
                  << (msg) << std::endl;                                \
        std::abort();                                                   \
    } while (0)
