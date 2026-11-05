#pragma once

#include "option.h"
#include "event.h"

//----------------------------------------------------------------------------
/// @class Listener
//----------------------------------------------------------------------------
class Listener : public std::enable_shared_from_this<Listener>
{
    class Impl;
    std::unique_ptr<Impl> pimpl_;
    Listener(const ListenOption& listenOption);
public:
    using ptr_t = std::shared_ptr<Listener>;
    static ptr_t Create(const ListenOption& listenOption);
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    virtual ~Listener(void);
    virtual bool Initialize();
    virtual void Destroy();
    virtual void AddEvent(Event::wptr_t ev, int priority, bool own = false);
    virtual const ListenOption& GetOption() const;
    virtual std::string GetErrMsg(const std::string& sep = "\n") const;
};
