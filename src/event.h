#pragma once

#include "sockaddr.h"
#include "option.h"

//----------------------------------------------------------------------------
/// @class Event
///
//----------------------------------------------------------------------------
class Event
{
    volatile bool listenerFlag_;
    volatile bool receiverFlag_;
public:
    using ptr_t = std::shared_ptr<Event>;
    using wptr_t = std::weak_ptr<Event>;
    using vector_t = std::vector<ptr_t> ;
    using pair_t = std::pair<int, wptr_t>;
    using list_t = std::list<pair_t>;
    using buf_t = std::vector<char>;

    // for internal use
    static void AddEvent(std::mutex& mutex, list_t& list, wptr_t wptr, int priority = 0) { AddEvent(mutex, list, std::make_pair(priority, wptr)); }
    static void AddEvent(std::mutex& mutex, list_t& list, pair_t pair);
    static vector_t GetEvents(std::mutex& mutex, list_t& list);

    Event() : listenerFlag_(false), receiverFlag_(false) {}
    virtual ~Event() {}

    // Listener events to be overridden
    virtual bool OnPreAccept(ListenOption& option, int sfd, const SockAddr& peer, const StreamOption& streamOption) { return false; }
    virtual bool OnAccept(const ListenOption& option, int sfd, const SockAddr& peer, const StreamOption& streamOption) { return false; }
    virtual bool OnThreadExit(const ListenOption& option) { return false; }
    virtual bool OnListenerFlag(const ListenOption& option) { return false; }
    virtual bool GetListenerFlag() const { return listenerFlag_; }
    virtual void SetListenerFlag(bool flag) { listenerFlag_ = flag; }

    // Receiver events to be overridden
    virtual bool OnReceive(const ReceiveOption& option, const buf_t& buf, bool discrete) { return false; }
    virtual bool OnDisconnected(const ReceiveOption& option) { return false; }
    virtual bool OnThreadExit(const ReceiveOption& option) { return false; }
    virtual bool OnReceiverFlag(const ReceiveOption& option) { return false; }
    virtual bool GetReceiverFlag() const { return receiverFlag_; }
    virtual void SetReceiverFlag(bool flag) { receiverFlag_ = flag; }
};
