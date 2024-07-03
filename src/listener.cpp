#include "stdafx.h"
#include "listener.h"
#include "messages.h"

//----------------------------------------------------------------------------
/// @class Listener::Impl
//----------------------------------------------------------------------------
class Listener::Impl
{
    Listener* owner_;
    const ListenOption option_;
    std::vector<SRTSOCKET> sfds_;
    int eid_;
    boost::thread thread_;
    boost::mutex mutex_;
    Event::list_t events_;
    Event::vector_t own_events_;
    SafeMessages errmsgs_;
public:
    Impl(Listener* owner, const ListenOption& option)
        : owner_(owner), option_(option), sfds_(), eid_(-1), thread_(), mutex_(), events_(), own_events_(), errmsgs_() {
    }
    virtual ~Impl() {
        Destroy();
    }
    virtual bool Initialize() {
        Destroy();
        eid_ = srt_epoll_create();
        if (eid_ < 0) {
            errmsgs_ << boost::format("failed srt_epoll_create(): %s") % srt_getlasterror_str();
            return false;
        }
        std::string host = option_.Get<std::string>("host", "");
        std::string port = option_.Get<std::string>("port", "");
        addrinfo* res0 = nullptr;
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = AI_PASSIVE;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        if (!host.empty()) {
            SockAddr sa(host.c_str(), nullptr);
            hints.ai_family = sa.ss_family;
        }
        int errcode = getaddrinfo(host.empty() ? nullptr : host.c_str(), port.c_str(), &hints, &res0);
        if (errcode) {
            boost::mutex::scoped_lock lk(mutex_);
            errmsgs_ << boost::format("failed getaddrinfo(): %s") % gai_strerror(errcode);
            return false;
        }
        for (addrinfo* res = res0; res; res = res->ai_next) {
            SRTSOCKET sfd = srt_create_socket();
            if (sfd == SRT_INVALID_SOCK) {
                errmsgs_ << boost::format("failed srt_create_socket(): %s") % srt_getlasterror_str();
                continue;
            }
            if (res->ai_family == AF_INET6) {
                int32_t ipv6Only = host.empty() ? 0 : 1;
                if (srt_setsockflag(sfd, SRTO_IPV6ONLY, &ipv6Only, sizeof(ipv6Only)) == SRT_ERROR) {
                    errmsgs_ << boost::format("failed srt_setsockflag(SRTO_IPV6ONLY) [ %d ]; %s") % ipv6Only % srt_getlasterror_str();
                    return false;
                }
            } else if (host.empty() && (!sfds_.empty() || res->ai_next)) {
                // ignore IPv4 because IPv4-mapped IPv6 would work
                srt_close(sfd);
                continue;
            }
            if (!SetSockFlags(sfd, option_, errmsgs_)) {
                srt_close(sfd);
                continue;
            }
            if (srt_bind(sfd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SRT_ERROR) {
                errmsgs_ << boost::format("failed srt_bind() [ %s ]; %s") % SockAddr(res->ai_addr, res->ai_addrlen).ToString() % srt_getlasterror_str();
                srt_close(sfd);
                continue;
            }
            if (srt_listen(sfd, option_.Get<int>("backlog", 10)) == SRT_ERROR) {
                errmsgs_ << boost::format("failed srt_listen(); %s") % srt_getlasterror_str();
                srt_close(sfd);
                continue;
            }
            if (srt_listen_callback(sfd, ListenCallbackWrapper, this) == SRT_ERROR) {
                errmsgs_ << boost::format("failed srt_listen_callback(); %s") % srt_getlasterror_str();
                srt_close(sfd);
                continue;
            }
            int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
            if (srt_epoll_add_usock(eid_, sfd, &events) == SRT_ERROR) {
                errmsgs_ << boost::format("failed srt_epoll_add_usock(); %s") % srt_getlasterror_str();
                srt_close(sfd);
                continue;
            }
            //TRACE(_T("MESRT::Listener::Impl::Initialize (%s) Listen %s\n"), A4T(host).c_str(), A4T(SockAddr(res->ai_addr, res->ai_addrlen).ToString()).c_str());
            sfds_.push_back(sfd);
            //break;
        }
        freeaddrinfo(res0);
        if (sfds_.empty()) {
            errmsgs_ << "there is no interface to start listening.";
            return false;
        }
        thread_ = boost::thread(&Impl::Thread, this);
        return true;
    }
    virtual void Destroy() {
        if (eid_ >= 0) {
            int eid = eid_;
            eid_ = -1;
            srt_epoll_release(eid);
        }
        if (thread_.joinable()) {
            thread_.join();
            //TRACE(_T("MESRT::Listener::Impl::Destroy (%s)\n"), A4T(option_.Get<std::string>("host", "")).c_str());
        }
        for (std::vector<SRTSOCKET>::const_iterator it = sfds_.begin(); it != sfds_.end(); ++it) {
            if (*it != SRT_INVALID_SOCK) {
                srt_close(*it);
            }
        }
        sfds_.clear();
        own_events_.clear();
    }
    virtual void AddEvent(Event::wptr_t ev, int priority, bool own) {
        Event::AddEvent(mutex_, events_, ev, priority);
        if (!own) return;
        Event::ptr_t p = ev.lock();
        if (!p) return;
        boost::mutex::scoped_lock lk(mutex_);
        own_events_.push_back(p);
    }
    virtual Event::vector_t GetEvents() {
        return Event::GetEvents(mutex_, events_);
    }
    virtual const ListenOption& GetOption() const {
        return option_;
    }
    virtual std::string GetErrMsg(const std::string& sep) const {
        return errmsgs_(sep);
    }
protected:
    static bool SetSockFlags(SRTSOCKET sfd, const ListenOption& option, SafeMessages& errmsgs) {
        if (option.Has("udpsndbuf")) {
            int udpsndbuf = option.Get<int>("udpsndbuf", 65536);
            if (srt_setsockflag(sfd, SRTO_UDP_SNDBUF, &udpsndbuf, sizeof(udpsndbuf)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_UDP_SNDBUF) [ %d ]; %s") % udpsndbuf % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("udprcvbuf")) {
            int udprcvbuf = option.Get<int>("udprcvbuf", 65536);
            if (srt_setsockflag(sfd, SRTO_UDP_RCVBUF, &udprcvbuf, sizeof(udprcvbuf)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_UDP_RCVBUF) [ %d ]; %s") % udprcvbuf % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("transtype")) {
            SRT_TRANSTYPE transtype = static_cast<SRT_TRANSTYPE>(option.Get<int>("transtype", SRTT_LIVE));
            if (srt_setsockflag(sfd, SRTO_TRANSTYPE, &transtype, sizeof(transtype)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_TRANSTYPE) [ %d ]; %s") % static_cast<int>(transtype) % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("pbkeylen")) {
            int32_t pbkeylen = option.Get<int32_t>("pbkeylen", 0);
            if (pbkeylen > 0 && srt_setsockflag(sfd, SRTO_PBKEYLEN, &pbkeylen, sizeof(pbkeylen)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_PBKEYLEN) [ %d ]; %s") % pbkeylen % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("passphrase")) {
            std::string passphrase = option.Get<std::string>("passphrase", "");
            if (!passphrase.empty() && srt_setsockflag(sfd, SRTO_PASSPHRASE, passphrase.data(), static_cast<int>(passphrase.size())) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_PASSPHRASE) [ %s ]; %s") % passphrase % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("mss")) {
            int mss = option.Get<int>("mss", 1500);
            if (srt_setsockflag(sfd, SRTO_MSS, &mss, sizeof(mss)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_MSS) [ %d ]; %s") % mss % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("fc")) {
            int fc = option.Get<int>("fc", 25600);
            if (srt_setsockflag(sfd, SRTO_FC, &fc, sizeof(fc)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_FC) [ %d ]; %s") % fc % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("sndbuf")) {
            int sndbuf = option.Get<int>("sndbuf", 8192 * (1500 - 28));
            if (srt_setsockflag(sfd, SRTO_SNDBUF, &sndbuf, sizeof(sndbuf)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_SNDBUF) [ %d ]; %s") % sndbuf % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("rcvbuf")) {
            int rcvbuf = option.Get<int>("rcvbuf", 8192 * (1500 - 28));
            if (srt_setsockflag(sfd, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_RCVBUF) [ %d ]; %s") % rcvbuf % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("rcvsyn")) {
            bool rcvsyn = option.Get<int>("rcvsyn", 0) > 0 ? true : false;
            if (srt_setsockflag(sfd, SRTO_RCVSYN, &rcvsyn, sizeof(rcvsyn)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_RCVSYN) [ %s ]; %s") % (rcvsyn ? "true" : "false") % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("linger")) {
            linger l = { 0, 0 };
            l.l_linger = option.Get<u_short>("linger", 180);
            if (l.l_linger > 0) l.l_onoff = 1;
            if (srt_setsockflag(sfd, SRTO_LINGER, &l, sizeof(l)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_LINGER) [ %s(%d) ]; %s") % (l.l_onoff ? "on" : "off") % l.l_linger % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("ipttl")) {
            int32_t ipttl = option.Get<int32_t>("ipttl", 64);
            if (ipttl > 0 && srt_setsockflag(sfd, SRTO_IPTTL, &ipttl, sizeof(ipttl)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_IPTTL) [ %d ]; %s") % ipttl % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("iptos")) {
            int32_t iptos = option.Get<int32_t>("iptos", 0xB8);
            if (srt_setsockflag(sfd, SRTO_IPTOS, &iptos, sizeof(iptos)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_IPTOS) [ %d ]; %s") % iptos % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("latency")) {
            int32_t latency = option.Get<int32_t>("latency", 0);
            if (srt_setsockflag(sfd, SRTO_LATENCY, &latency, sizeof(latency)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_LATENCY) [ %d ]; %s") % latency % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("tsbpdmode")) {
            int32_t tsbpdmode = option.Get<int32_t>("tsbpdmode", 1);
            if (srt_setsockflag(sfd, SRTO_TSBPDMODE, &tsbpdmode, sizeof(tsbpdmode)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_TSBPDMODE) [ %d ]; %s") % tsbpdmode % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("tlpktdrop")) {
            int32_t tlpktdrop = option.Get<int32_t>("tlpktdrop", 1);
            if (srt_setsockflag(sfd, SRTO_TLPKTDROP, &tlpktdrop, sizeof(tlpktdrop)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_TLPKTDROP) [ %d ]; %s") % tlpktdrop % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("snddropdelay")) {
            int snddropdelay = option.Get<int>("snddropdelay", 0);
            if (srt_setsockflag(sfd, SRTO_SNDDROPDELAY, &snddropdelay, sizeof(snddropdelay)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_SNDDROPDELAY) [ %d ]; %s") % snddropdelay % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("nakreport")) {
            bool nakreport = option.Get<int>("nakreport", 1) > 0 ? true : false;
            if (srt_setsockflag(sfd, SRTO_NAKREPORT, &nakreport, sizeof(nakreport)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_NAKREPORT) [ %s ]; %s") % (nakreport ? "true" : "false") % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("conntimeo")) {
            int conntimeo = option.Get<int>("conntimeo", 3000);
            if (srt_setsockflag(sfd, SRTO_CONNTIMEO, &conntimeo, sizeof(conntimeo)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_CONNTIMEO) [ %d ]; %s") % conntimeo % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("lossmaxttl")) {
            int lossmaxttl = option.Get<int>("lossmaxttl", 0);
            if (srt_setsockflag(sfd, SRTO_LOSSMAXTTL, &lossmaxttl, sizeof(lossmaxttl)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_LOSSMAXTTL) [ %d ]; %s") % lossmaxttl % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("rcvlatency")) {
            int32_t rcvlatency = option.Get<int32_t>("rcvlatency", 120);
            if (srt_setsockflag(sfd, SRTO_RCVLATENCY, &rcvlatency, sizeof(rcvlatency)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_RCVLATENCY) [ %d ]; %s") % rcvlatency % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("peerlatency")) {
            int32_t peerlatency = option.Get<int32_t>("peerlatency", 0);
            if (srt_setsockflag(sfd, SRTO_PEERLATENCY, &peerlatency, sizeof(peerlatency)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_PEERLATENCY) [ %d ]; %s") % peerlatency % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("minversion")) {
            int32_t minversion = option.Get<int32_t>("minversion", 0);
            if (srt_setsockflag(sfd, SRTO_MINVERSION, &minversion, sizeof(minversion)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_MINVERSION) [ %d ]; %s") % minversion % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("streamid")) {
            std::string streamid = option.Get<std::string>("streamid", "");
            if (srt_setsockflag(sfd, SRTO_STREAMID, streamid.data(), static_cast<int>(streamid.size())) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_STREAMID) [ %s ]; %s") % streamid % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("congestion")) {
            std::string congestion = option.Get<std::string>("congestion", "live");
            if (srt_setsockflag(sfd, SRTO_CONGESTION, congestion.data(), static_cast<int>(congestion.size())) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_CONGESTION) [ %s ]; %s") % congestion % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("messageapi")) {
            bool messageapi = option.Get<int>("messageapi", 1) > 0 ? true : false;
            if (srt_setsockflag(sfd, SRTO_MESSAGEAPI, &messageapi, sizeof(messageapi)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_MESSAGEAPI) [ %s ]; %s") % (messageapi ? "true" : "false") % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("payloadsize")) {
            int payloadsize = option.Get<int>("payloadsize", 1316);
            if (srt_setsockflag(sfd, SRTO_PAYLOADSIZE, &payloadsize, sizeof(payloadsize)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_PAYLOADSIZE) [ %d ]; %s") % payloadsize % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("kmrefreshrate")) {
            int32_t kmrefreshrate = option.Get<int32_t>("kmrefreshrate", 0x1000000);
            if (srt_setsockflag(sfd, SRTO_KMREFRESHRATE, &kmrefreshrate, sizeof(kmrefreshrate)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_KMREFRESHRATE) [ %d ]; %s") % kmrefreshrate % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("kmpreannounce")) {
            int32_t kmpreannounce = option.Get<int32_t>("kmpreannounce", 0x1000);
            if (srt_setsockflag(sfd, SRTO_KMPREANNOUNCE, &kmpreannounce, sizeof(kmpreannounce)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_KMPREANNOUNCE) [ %d ]; %s") % kmpreannounce % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("enforcedencryption")) {
            int enforcedencryption = option.Get<int>("enforcedencryption", 1);
            if (srt_setsockflag(sfd, SRTO_ENFORCEDENCRYPTION, &enforcedencryption, sizeof(enforcedencryption)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_ENFORCEDENCRYPTION) [ %d ]; %s") % enforcedencryption % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("peeridletimeo")) {
            int32_t peeridletimeo = option.Get<int32_t>("peeridletimeo", 5000);
            if (srt_setsockflag(sfd, SRTO_PEERIDLETIMEO, &peeridletimeo, sizeof(peeridletimeo)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_PEERIDLETIMEO) [ %d ]; %s") % peeridletimeo % srt_getlasterror_str();
                return false;
            }
        }
        if (option.Has("packetfilter")) {
            std::string packetfilter = option.Get<std::string>("packetfilter", "");
            if (srt_setsockflag(sfd, SRTO_PACKETFILTER, packetfilter.data(), static_cast<int>(packetfilter.size())) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_PACKETFILTER) [ %s ]; %s") % packetfilter % srt_getlasterror_str();
                return false;
            }
        }
#if SRT_VERSION_VALUE >= SRT_MAKE_VERSION_VALUE(1,4,2)
        if (option.Has("retransmitalgo")) {
            int32_t retransmitalgo = option.Get<int32_t>("retransmitalgo", 1);
            if (srt_setsockflag(sfd, SRTO_RETRANSMITALGO, &retransmitalgo, sizeof(retransmitalgo)) == SRT_ERROR) {
                errmsgs << boost::format("failed srt_setsockflag(SRTO_RETRANSMITALGO) [ %d ]; %s") % retransmitalgo % srt_getlasterror_str();
                return false;
            }
        }
#endif
        return true;
    }
    virtual void Thread() {
        try {
            Poll();
        } catch (boost::thread_interrupted&) {
            errmsgs_ << "the thread has been interrupted.";
        } catch (std::exception& ev) {
            errmsgs_ << boost::format("an unexpected exception occurred: %s") % ev.what();
        }
        ThreadExit();
    }
    virtual void Poll() {
        int msTimeout = option_.Get<int>("epolltimeo", 100);
        std::vector<SRTSOCKET> srtrfds(sfds_.size(), SRT_INVALID_SOCK);
        for (; eid_ >= 0; CheckFlag(), boost::this_thread::interruption_point()) {
            int srtrfdslen = static_cast<int>(srtrfds.size());
            int n = srt_epoll_wait(eid_, &srtrfds.at(0), &srtrfdslen, 0, 0, msTimeout, 0, 0, 0, 0);
            for (int i = 0; i < n; ++i) {
                SRTSOCKET sfd = srtrfds[i];
                SRT_SOCKSTATUS status = srt_getsockstate(sfd);
                if (status == SRTS_LISTENING) {
                    Accept(sfd);
                }
            }
        }
    }
    virtual void CheckFlag() {
        Event::vector_t events = Event::GetEvents(mutex_, events_);
        for (Event::vector_t::iterator it = events.begin(); it != events.end(); ++it) {
            Event::ptr_t ev = *it;
            if (ev->GetListenerFlag()) {
                if (!ev->OnListenerFlag(option_)) {
                    ev->SetListenerFlag(false);
                }
            }
        }
    }
    virtual void ThreadExit() {
        Event::vector_t events = Event::GetEvents(mutex_, events_);
        for (Event::vector_t::iterator it = events.begin(); it != events.end(); ++it) {
            Event::ptr_t ev = *it;
            ev->OnThreadExit(option_);
        }
    }
    virtual void Accept(SRTSOCKET listen) {
        int len = sizeof(sockaddr_storage);
        SockAddr peer;
        SRTSOCKET sfd = srt_accept(listen, reinterpret_cast<sockaddr*>(&peer), &len);
        if (sfd == SRT_INVALID_SOCK) {
            errmsgs_ << boost::format("failed srt_accept(): %s") % srt_getlasterror_str();
            return;
        }
        peer.ConvertV4MappedV6ToV4();
        char optbuf[512 + 1] = { '\0' };
        int optlen = 512;
        srt_getsockflag(sfd, SRTO_STREAMID, optbuf, &optlen);
        StreamOption streamOption(optbuf);
        Event::vector_t events = GetEvents();
        for (Event::vector_t::const_iterator it = events.begin(); it != events.end(); ++it) {
            Event::ptr_t ev = *it;
            if (ev->OnAccept(option_, static_cast<int>(sfd), peer, streamOption)) {
                return;
            } else {
                // remove if owned event listener returns false
                boost::mutex::scoped_lock lk(mutex_);
                boost::range::remove_erase(own_events_, ev);
            }
        }
        srt_close(sfd);
        errmsgs_ << boost::format("the connection from [ %s ] is not accepted(2); %s") % peer.ToString() % streamOption();
    }
    virtual int ListenCallback(SRTSOCKET ns, int hsversion, const struct sockaddr* peeraddr, const char* streamid) {
        // called from thread of srt core when new connection arrived
        // "Pre" options could be set in this context like SRTO_PASSPHRASE
        SockAddr peer(peeraddr);
        peer.ConvertV4MappedV6ToV4();
        StreamOption streamOption(streamid);
        ListenOption option; // "pre-bind" options could not be set in this context
        Event::vector_t events = GetEvents();
        for (Event::vector_t::const_iterator it = events.begin(); it != events.end(); ++it) {
            Event::ptr_t ev = *it;
            if (ev && ev->OnPreAccept(option, ns, peer, streamOption)) {
                if (!SetSockFlags(ns, option, errmsgs_)) {
                    return -1;
                }
                return 0; // 0 to Accept from srt_epoll_wait
            }
        }
        errmsgs_ << boost::format("the connection from [ %s ] is not accepted(1); %s") % peer.ToString() % streamOption();
        return -1; // -1 to reject connection attempts
    }
    static int ListenCallbackWrapper(void* opaq, SRTSOCKET ns, int hsversion, const struct sockaddr* peeraddr, const char* streamid) {
        return static_cast<Impl*>(opaq)->ListenCallback(ns, hsversion, peeraddr, streamid);
    }
};

Listener::ptr_t Listener::Create(const ListenOption& listenOption) {
    return Listener::ptr_t(new Listener(listenOption));
}
Listener::Listener(const ListenOption& listenOption)
    : pimpl_(new Impl(this, listenOption)) {
}
Listener::~Listener() {
    Destroy();
    pimpl_.reset();
}
bool Listener::Initialize() {
    return pimpl_->Initialize();
}
void Listener::Destroy() {
    pimpl_->Destroy();
}
void Listener::AddEvent(Event::wptr_t ev, int priority, bool own) {
    pimpl_->AddEvent(ev, priority, own);
}
const ListenOption& Listener::GetOption() const {
    return pimpl_->GetOption();
}
std::string Listener::GetErrMsg(const std::string& sep) const {
    return pimpl_->GetErrMsg(sep);
}
