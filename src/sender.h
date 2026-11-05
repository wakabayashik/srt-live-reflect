#pragma once

#include "option.h"
#include "event.h"

//----------------------------------------------------------------------------
/// @class Sender
//----------------------------------------------------------------------------
class Sender : public std::enable_shared_from_this<Sender>
{
    class Impl;
    std::unique_ptr<Impl> pimpl_;
    Sender(int sfd, const SendOption& option);
public:
    using ptr_t = std::shared_ptr<Sender>;
    static ptr_t Create(int sfd, const SendOption& option);
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    virtual ~Sender();
    virtual bool Initialize();
    virtual void Destroy();
    virtual bool Send(const Event::buf_t& buf);
    virtual bool Send(const char* buf, size_t len);
    virtual bool IsConnected() const;
    virtual const SendOption& GetOption() const;
    virtual std::string GetErrMsg(const std::string& sep = "\n") const;
    virtual std::string GetStatistics(int level = 1, const std::string& sep = "\n") const;
};
