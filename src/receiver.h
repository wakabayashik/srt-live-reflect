#pragma once

#include "option.h"
#include "event.h"

//----------------------------------------------------------------------------
/// @class CReceiver
//----------------------------------------------------------------------------
class Receiver : public std::enable_shared_from_this<Receiver>
{
    class Impl;
    std::unique_ptr<Impl> pimpl_;
    Receiver(int sfd, const ReceiveOption& receiveOption);
public:
    using ptr_t = std::shared_ptr<Receiver>;
    using map_t = std::map<std::string, ptr_t>;
    static ptr_t Create(int sfd, const ReceiveOption& receiveOption);
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    virtual ~Receiver();
    virtual bool Initialize();
    virtual void Destroy();
    virtual void AddEvent(Event::wptr_t ev, int priority, bool own = false);
    virtual const ReceiveOption& GetOption() const;
    virtual std::string GetErrMsg(const std::string& sep = "\n") const;
    virtual std::string GetStatistics(int level = 1, const std::string& sep = "\n") const;
};
