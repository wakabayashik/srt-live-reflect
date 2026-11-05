#pragma once
#include "event.h"

//----------------------------------------------------------------------------
/// @class LoopRec
//----------------------------------------------------------------------------
class LoopRec : public Event, public std::enable_shared_from_this<LoopRec> {
    class Impl;
    std::unique_ptr<Impl> pimpl_;
    LoopRec(const Json& conf, const std::string& app, const std::string& name);
public:
    using ptr_t = std::shared_ptr<LoopRec>;
    using map_t = std::map<std::string, ptr_t>;
    static map_t Create(const Json::Node& loopRecs, const std::string& app);
    LoopRec(const LoopRec&) = delete;
    LoopRec& operator=(const LoopRec&) = delete;
    virtual ~LoopRec();
    virtual bool Initialize();
    virtual void Destroy();
    virtual bool IsAcceptable(const StreamOption& streamOption) const;
    virtual void CreateSender(int sfd, const SendOption& sendOption, const StreamOption& streamOption);
protected:
    virtual bool OnReceive(const ReceiveOption& option, const Event::buf_t& buf, bool discrete) override;
    virtual bool OnDisconnected(const ReceiveOption& option) override;
};
