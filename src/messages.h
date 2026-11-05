#pragma once

//----------------------------------------------------------------------------
/// @class Messages
/// keep number of messages
//----------------------------------------------------------------------------
class Messages
{
    using msgs_t = std::list<std::string>;
    size_t keep_;
    msgs_t msgs_;
public:
    class Ctrl
    {
    public:
        virtual Messages& operator()(Messages& msgs) = 0;
    };
    class Clear : public Ctrl
    {
    public:
        virtual Messages& operator()(Messages& msgs); // clear messages
    };
    class Resize : public Ctrl
    {
        size_t resize_;
    public:
        Resize(size_t resize) : resize_(resize) {}
        virtual Messages& operator()(Messages& msgs); // modify number of messages to keep
    };
    Messages(size_t keep = 5) : keep_(keep), msgs_() {}
    virtual Messages& operator<<(const std::string& str);
    virtual Messages& operator<<(const boost::format& fmt) { return operator<<(fmt.str()); }
    virtual Messages& operator<<(Ctrl& ctrl) { return ctrl(*this); }
    virtual std::string operator()(const std::string& sep = "\n") const;
};

//----------------------------------------------------------------------------
/// @class SafeMessages
/// keep number of messages (multi-threading)
//----------------------------------------------------------------------------
class SafeMessages : public Messages
{
    mutable std::mutex mutex_;
public:
    SafeMessages(size_t keep = 5) : Messages(keep), mutex_() {}
    virtual Messages& operator<<(const std::string& str) { std::lock_guard<std::mutex> lk(mutex_); return Messages::operator<<(str); }
    virtual Messages& operator<<(const boost::format& fmt) { std::lock_guard<std::mutex> lk(mutex_); return Messages::operator<<(fmt.str()); }
    virtual Messages& operator<<(Ctrl& ctrl) { std::lock_guard<std::mutex> lk(mutex_); return Messages::operator<<(ctrl); }
    virtual std::string operator()(const std::string& sep = "\n") const { std::lock_guard<std::mutex> lk(mutex_); return Messages::operator()(sep); }
};
