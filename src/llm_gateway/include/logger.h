#pragma once
/**
 * logger.h — 异步日志（无锁环形缓冲 + 后台写线程）
 */
#include "common.h"
#include <string>

using gw::LogLevel;
#include <atomic>
#include <thread>
#include <fstream>
#include <array>
#include <ctime>
#include <cstdio>

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(const std::string& path, LogLevel min_level = LogLevel::INFO) {
        min_level_ = min_level;
        file_.open(path, std::ios::app);
        running_.store(true);
        writer_ = std::thread(&Logger::flush_loop, this);
    }

    void log(LogLevel lv, const std::string& msg) {
        if (lv < min_level_) return;
        std::string line = format(lv, msg);
        // 写入环形缓冲
        size_t pos = write_pos_.fetch_add(1) % kBufSize;
        buf_[pos] = std::move(line);
        count_.fetch_add(1);
    }

    void shutdown() {
        running_.store(false);
        if (writer_.joinable()) writer_.join();
        flush_all();
        file_.close();
    }

    // 便捷宏
    void info (const std::string& m) { log(LogLevel::INFO,  m); }
    void warn (const std::string& m) { log(LogLevel::WARN,  m); }
    void error(const std::string& m) { log(LogLevel::ERROR, m); }
    void debug(const std::string& m) { log(LogLevel::DEBUG, m); }

private:
    static constexpr size_t kBufSize = 4096;

    LogLevel                    min_level_ = LogLevel::INFO;
    std::ofstream               file_;
    std::array<std::string, kBufSize> buf_;
    std::atomic<size_t>         write_pos_{0};
    std::atomic<size_t>         read_pos_{0};
    std::atomic<size_t>         count_{0};
    std::atomic<bool>           running_{false};
    std::thread                 writer_;

    static std::string format(LogLevel lv, const std::string& msg) {
        time_t t = time(nullptr);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
        const char* lvstr[] = {"DEBUG","INFO ","WARN ","ERROR"};
        char buf[2048];
        snprintf(buf, sizeof(buf), "[%s][%s] %s\n",
                 ts, lvstr[static_cast<int>(lv)], msg.c_str());
        return buf;
    }

    void flush_loop() {
        while (running_.load()) {
            flush_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void flush_all() {
        while (count_.load() > 0) {
            size_t pos = read_pos_.fetch_add(1) % kBufSize;
            if (!buf_[pos].empty()) {
                file_ << buf_[pos];
                buf_[pos].clear();
            }
            count_.fetch_sub(1);
        }
        file_.flush();
    }

    Logger() = default;
    ~Logger() { if (running_) shutdown(); }
};

#define GW_LOG_INFO(msg)  Logger::instance().info(msg)
#define GW_LOG_WARN(msg)  Logger::instance().warn(msg)
#define GW_LOG_ERROR(msg) Logger::instance().error(msg)
#define GW_LOG_DEBUG(msg) Logger::instance().debug(msg)
