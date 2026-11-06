#pragma once

#include "json.h"

class Logger
{
    static std::function<void(const std::string&)> trace_;
    static std::function<void(const std::string&)> debug_;
    static std::function<void(const std::string&)> info_;
    static std::function<void(const std::string&)> warning_;
    static std::function<void(const std::string&)> error_;
    static std::function<void(const std::string&)> fatal_;
    static int level_;
public:
    static std::string Timestamp();
    static void Init(const Json::Node& conf, const std::string& name);
    static void Term();
    static bool TraceEnabled();
    static void Trace(const std::string& msg) { trace_(msg); }
    static void Trace(const boost::format& fmt) { trace_(fmt.str()); }
    static bool DebugEnabled();
    static void Debug(const std::string& msg) { debug_(msg); }
    static void Debug(const boost::format& fmt) { debug_(fmt.str()); }
    static bool InfoEnabled();
    static void Info(const std::string& msg) { info_(msg); }
    static void Info(const boost::format& fmt) { info_(fmt.str()); }
    static bool WarningEnabled();
    static void Warning(const std::string& msg) { warning_(msg); }
    static void Warning(const boost::format& fmt) { warning_(fmt.str()); }
    static bool ErrorEnabled();
    static void Error(const std::string& msg) { error_(msg); }
    static void Error(const boost::format& fmt) { error_(fmt.str()); }
    static bool FatalEnabled();
    static void Fatal(const std::string& msg) { fatal_(msg); }
    static void Fatal(const boost::format& fmt) { fatal_(fmt.str()); }
};

class AntiRepeatLogger
{
    const std::string prefix_;
    std::string key_;
    int level_;
    int64_t repeat_;
public:
    AntiRepeatLogger(const std::string& prefix) : prefix_(prefix), key_(), level_(-1), repeat_(0) {}
    void Clear() { Reset("", -1, 0); }
    void Trace(const std::string& msg) { Trace("", msg); }
    void Trace(const boost::format& fmt) { Trace("", fmt.str()); }
    void Trace(const std::string& key, const std::string& msg);
    void Trace(const std::string& key, const boost::format& fmt) { Trace(key, fmt.str()); }
    void Debug(const std::string& msg) { Debug("", msg); }
    void Debug(const boost::format& fmt) { Debug("", fmt.str()); }
    void Debug(const std::string& key, const std::string& msg);
    void Debug(const std::string& key, const boost::format& fmt) { Debug(key, fmt.str()); }
    void Info(const std::string& msg) { Info("", msg); }
    void Info(const boost::format& fmt) { Info("", fmt.str()); }
    void Info(const std::string& key, const std::string& msg);
    void Info(const std::string& key, const boost::format& fmt) { Info(key, fmt.str()); }
    void Warning(const std::string& msg) { Warning("", msg); }
    void Warning(const boost::format& fmt) { Warning("", fmt.str()); }
    void Warning(const std::string& key, const std::string& msg);
    void Warning(const std::string& key, const boost::format& fmt) { Warning(key, fmt.str()); }
    void Error(const std::string& msg) { Error("", msg); }
    void Error(const boost::format& fmt) { Error("", fmt.str()); }
    void Error(const std::string& key, const std::string& msg);
    void Error(const std::string& key, const boost::format& fmt) { Error(key, fmt.str()); }
    void Fatal(const std::string& msg);
    void Fatal(const boost::format& fmt) { Fatal(fmt.str()); }
protected:
    void Reset(const std::string& key, int level, int64_t repeat);
};

