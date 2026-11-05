#include "stdafx.h"
#include "json.h"
#include "curl.h"
#include "logger.h"
#include "listener.h"
#include "receiver.h"
#include "sender.h"
#include "looprec.h"
#include "aws.h"

#if defined(_DEBUG) && defined(WIN32)
#include <conio.h>
#endif

//----------------------------------------------------------------------------
/// @class ReflectSender
//----------------------------------------------------------------------------
class ReflectSender : public Event {
    Sender::ptr_t sender_;
    std::string app_;
    std::string name_;
    std::string peer_;
protected:
    ReflectSender(int sfd, const SendOption& option) : Event(), sender_(Sender::Create(sfd, option)) {
        app_ = option.Get<std::string>("app");
        name_ = option.Get<std::string>("name");
        peer_ = option.Get<std::string>("peer");
    }
public:
    static Event::ptr_t Create(int sfd, const SendOption& option) {
        return Event::ptr_t(new ReflectSender(sfd, option));
    }
    ReflectSender(const ReflectSender&) = delete;
    ReflectSender& operator=(const ReflectSender&) = delete;
    virtual ~ReflectSender() {
        sender_.reset();
    }
protected:
    bool OnReceive(const ReceiveOption& option, const Event::buf_t& buf, bool discrete) override {
        if (!sender_) return false;
        if (sender_->Send(buf)) return true;
        std::string err = sender_->GetErrMsg();
        sender_.reset();
        Logger::Info(boost::format("<%s> send failed [%s] for %s : %s") % app_ % name_ % peer_ % err);
        return false;
    }
};

//----------------------------------------------------------------------------
/// @class Reflect
//----------------------------------------------------------------------------
class Reflect : public Event, public std::enable_shared_from_this<Reflect>
{
    class Cache {
        using data_t = std::tuple<std::chrono::steady_clock::time_point, CURLcode, Json>;
        using map_t = std::map<std::string, data_t>;
        map_t map_;
    public:
        Cache() : map_() {}
        CURLcode find(boost::mutex& mutex, const std::string& key, Json& body) const {
            boost::mutex::scoped_lock lock(mutex);
            map_t::const_iterator it = map_.find(key);
            if (it == map_.end()) return CURL_LAST;
            if (std::chrono::steady_clock::now() > std::get<0>(it->second)) return CURL_LAST;
            body = std::get<2>(it->second);
            return std::get<1>(it->second);
        }
        void set(boost::mutex& mutex, const std::string& key, CURLcode code, const Json& body, int32_t age) {
            boost::mutex::scoped_lock lock(mutex);
            map_[key] = std::make_tuple(std::chrono::steady_clock::now() + std::chrono::seconds(age), code, body);
        }
    };
    const Json conf_;
    mutable Cache cache_;
    Listener::ptr_t listener_;
    Receiver::map_t receivers_;
    LoopRec::map_t loopRecs_;
    mutable boost::mutex mutex_;
    int32_t stats_;
    std::chrono::steady_clock::time_point stats_time_;
protected:
    Reflect(const Json& conf) : Event(), conf_(conf), cache_(), listener_(), receivers_(), loopRecs_(), mutex_(), stats_(0), stats_time_() {
    }
    Reflect(const Reflect&) = delete;
    Reflect& operator=(const Reflect&) = delete;
    Receiver::ptr_t FindReceiver(const std::string& name) const {
        boost::mutex::scoped_lock lock(mutex_);
        const Receiver::map_t::const_iterator it = receivers_.find(name);
        return it == receivers_.end() ? Receiver::ptr_t() : it->second;
    }
public:
    using ptr_t = std::shared_ptr<Reflect>;
    using vector_t = std::vector<ptr_t>;
    static ptr_t Create(const Json& conf) {
        return ptr_t(new Reflect(conf));
    }
    virtual ~Reflect() {
        Destroy();
    }
    virtual bool Initialize() {
        stats_ = conf_["publish"]["stats"].to<int32_t>(0);
        stats_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(stats_);
        loopRecs_ = LoopRec::Create(conf_["loopRecs"], app());
        ListenOption opt;
        opt["host"] = conf_["host"].to<std::string>();
        opt["port"] = conf_["port"].to<std::string>();
        opt["backlog"] = conf_["backlog"].to<std::string>("10");
        opt["epolltimeo"] = conf_["epolltimeo"].to<std::string>("100");
        opt.SetSockOpts(conf_["option"], ListenOption::s_sockopts_pre_bind); // "pre-bind" options
        Listener::ptr_t listener(Listener::Create(opt));
        if (!listener->Initialize()) {
            Logger::Error(boost::format("<%s> Listener::Initialize error: %s") % app() % listener->GetErrMsg());
            return false;
        }
        listener->AddEvent(shared_from_this(), 0, false);
        listener_.swap(listener);
        Logger::Info(boost::format("<%s> listen %s:%s") % app() % opt["host"] % opt["port"]);
        return true;
    }
    virtual void Destroy() {
        listener_.reset();
        receivers_.clear();
        loopRecs_.clear();
    }
    virtual std::string app() const {
        return conf_["app"].to<std::string>("live");
    }
protected:
    using res_t = std::pair<bool, Json>;
    virtual bool PatternMatch(std::string pattern, const std::string& str) const {
        try {
            static const std::string escape = "\\+.?^$-|/{}()[]";
            for (size_t i = 0, c = escape.length(); i < c; ++i) {
                std::string esc = escape.substr(i, 1);
                boost::algorithm::replace_all(pattern, esc, "\\" + esc);
            }
            boost::algorithm::replace_all(pattern, "*", ".*");
            boost::algorithm::replace_all(pattern, "%", ".");
            std::regex regex("^" + pattern + "$");
            return std::regex_match(str, regex);
        } catch (std::regex_error&) {
            return false;
        }
    }
    virtual bool AccessCheck(const Json::Node& access, const SockAddr& addr, const StreamOption& streamOption) const {
        for (size_t i = 0, c = access.size(); i < c; ++i) {
            std::string name = access[i]["name"].to<std::string>("*");
            if (!name.empty() && name != "*" && !PatternMatch(name, streamOption.ResourceName())) {
                continue;
            }
            std::string deny = access[i]["deny"].to<std::string>();
            if (!deny.empty()) {
                if (deny == "*" || boost::iequals(deny, "all") || addr.Match(deny)) {
                    if (Logger::DebugEnabled()) {
                        Logger::Debug(boost::format("<%s> access denied: [ %s ] [ %s ] : %s")
                            % app() % addr.ToString() % streamOption(',', '=') % access[i].serialize());
                    }
                    return false;
                }
            }
            std::string allow = access[i]["allow"].to<std::string>();
            if (!allow.empty()) {
                if (allow == "*" || boost::iequals(allow, "all") || addr.Match(allow)) {
                    if (Logger::TraceEnabled()) {
                        Logger::Trace(boost::format("<%s> access allowed: [ %s ] [ %s ] : %s")
                            % app() % addr.ToString() % streamOption(',', '=') % access[i].serialize());
                    }
                    return true;
                }
            }
        }
        return true;
    }
    virtual CURLcode Perform(const std::string& uri, Json& body, const SockAddr& addr, const StreamOption& streamOption) const {
        std::string key = (boost::format("%s:%s") % uri % body.serialize()).str();
        CURLcode res = cache_.find(mutex_, key, body);
        if (res != CURL_LAST) return res;
        Curl curl;
        CurlJsonIO io(curl);
        io.Reset(5, body);
        curl_easy_setopt(io, CURLOPT_URL, uri.c_str());
        res = curl_easy_perform(io);
        if (res == CURLE_OK) {
            body = io.Json();
            if (Logger::TraceEnabled()) {
                long code = 0;
                curl_easy_getinfo(io, CURLINFO_RESPONSE_CODE, &code);
                Logger::Trace(boost::format("<%s> access allowed: [ %s ] [ %s ] : [ POST %s ] => (%d) [ %s ]")
                    % app() % addr.ToString() % streamOption(',', '=') % uri % code % io.Body());
            }
        } else {
            body = Json();
            if (Logger::DebugEnabled()) {
                long code = 0;
                curl_easy_getinfo(io, CURLINFO_RESPONSE_CODE, &code);
                Logger::Debug(boost::format("<%s> access denied: [ %s ] [ %s ] : [ POST %s ] => (%d) [ %s ] : %s")
                    % app() % addr.ToString() % streamOption(',', '=') % uri % code % io.Body() % io.Error());
            }
        }
        cache_.set(mutex_, key, res, body, conf_["cacheAge"].to<int32_t>(10));
        return res;
    }
    virtual res_t Authorize(const std::string& on, const std::string& call, const SockAddr& addr, const StreamOption& streamOption) const {
        if (on == "on_pre_accept" && !AccessCheck(conf_[call]["access"], addr, streamOption)) return std::make_pair(false, Json());
        std::string uri = conf_[call][on].to<std::string>();
        if (uri.empty()) return std::make_pair(true, Json());
        Json body;
        body["app"] = app().c_str();
        body["name"] = streamOption.ResourceName().c_str();
        body["on"] = on.c_str();
        body["call"] = call.c_str();
        body["addr"] = addr.GetAddress().c_str();
        const URIOption::map_t& map = streamOption.GetMap();
        for (URIOption::map_t::const_iterator it = map.begin(); it != map.end(); ++it) {
            body["streamid"][it->first] = it->second.c_str();
        }
        CURLcode res = Perform(uri, body, addr, streamOption);
        return std::make_pair(res == CURLE_OK, body);
    }
    virtual bool OnPreAccept(ListenOption& option, int sfd, const SockAddr& peer, const StreamOption& streamOption) override {
        std::string name = streamOption.ResourceName();
        if (name.empty()) {
            return false;
        } else if (streamOption.Mode() == "bidirectional") {
            return false;
        } else if (streamOption.Mode() == "publish") {
            Receiver::ptr_t receiver = FindReceiver(name);
            if (receiver) return false; // already exists
            res_t res = Authorize("on_pre_accept", "publish", peer, streamOption);
            if (!res.first) return false;
            option.SetSockOpts(conf_["option"], ListenOption::s_sockopts_pre); // "pre" options
            option.SetSockOpts(conf_["publish"]["option"], ListenOption::s_sockopts_pre);
            option.SetSockOpts(res.second["option"], ListenOption::s_sockopts_pre);
            return true;
        } else { // "request"
            if (streamOption.HasExcept("at", "now")) {
                LoopRec::map_t::const_iterator loopRec = loopRecs_.find(name);
                if (loopRec == loopRecs_.end() || !loopRec->second) return false;
                if (!loopRec->second->IsAcceptable(streamOption)) return false;
            } else {
                Receiver::ptr_t receiver = FindReceiver(name);
                if (!receiver) return false; // not exists
            }
            res_t res = Authorize("on_pre_accept", "play", peer, streamOption);
            if (!res.first) return false;
            option.SetSockOpts(conf_["option"], ListenOption::s_sockopts_pre); // "pre" options
            option.SetSockOpts(conf_["play"]["option"], ListenOption::s_sockopts_pre);
            option.SetSockOpts(res.second["option"], ListenOption::s_sockopts_pre);
            return true;
        }
    }
    virtual bool OnAccept(const ListenOption& option, int sfd, const SockAddr& peer, const StreamOption& streamOption) override {
        std::string name = streamOption.ResourceName();
        if (name.empty()) {
            return false;
        } else if (streamOption.Mode() == "bidirectional") {
            return false;
        } else if (streamOption.Mode() == "publish") {
            Receiver::ptr_t receiver = FindReceiver(name);
            if (receiver) return false; // already exists
            ReceiveOption opt;
            opt["name"] = name;
            opt["peer"] = peer.ToString();
            res_t res = Authorize("on_accept", "publish", peer, streamOption);
            if (!res.first) return false;
            opt.SetSockOpts(conf_["option"], ReceiveOption::s_sockopts); // "post" options
            opt.SetSockOpts(conf_["publish"]["option"], ReceiveOption::s_sockopts);
            opt.SetSockOpts(res.second["option"], ReceiveOption::s_sockopts);
            receiver = Receiver::Create(sfd, opt);
            if (!receiver->Initialize()) {
                Logger::Error(boost::format("<%s> Receiver::Initialize error: %s") % app() % receiver->GetErrMsg());
                return false;
            }
            receiver->AddEvent(shared_from_this(), 0);
            LoopRec::map_t::const_iterator loopRec = loopRecs_.find(name);
            if (loopRec != loopRecs_.end() && loopRec->second) receiver->AddEvent(loopRec->second, -1);
            boost::mutex::scoped_lock lock(mutex_);
            receivers_[name] = receiver;
            Logger::Info(boost::format("<%s> accept publish [ %s ] from %s") % app() % name % opt["peer"]);
            return true;
        } else { // "request"
            SendOption opt;
            opt["app"] = app();
            opt["name"] = name;
            opt["peer"] = peer.ToString();
            res_t res = Authorize("on_accept", "play", peer, streamOption);
            if (!res.first) return false;
            opt.SetSockOpts(conf_["option"], SendOption::s_sockopts); // "post" options
            opt.SetSockOpts(conf_["play"]["option"], SendOption::s_sockopts);
            opt.SetSockOpts(res.second["option"], SendOption::s_sockopts);
            if (streamOption.HasExcept("at", "now")) {
                LoopRec::map_t::const_iterator loopRec = loopRecs_.find(name);
                if (loopRec == loopRecs_.end() || !loopRec->second) return false;
                loopRec->second->CreateSender(sfd, opt, streamOption);
            } else {
                Receiver::ptr_t receiver = FindReceiver(name);
                if (!receiver) return false; // not exists
                Event::ptr_t sender(ReflectSender::Create(sfd, opt));
                receiver->AddEvent(sender, 0, true);
            }
            Logger::Info(boost::format("<%s> accept request [ %s ] from %s") % app() % name % opt["peer"]);
            return true;
        }
    }
    virtual bool OnListenerFlag(const ListenOption& option) override {
        boost::mutex::scoped_lock lock(mutex_);
        for (Receiver::map_t::const_iterator it = receivers_.begin(); it != receivers_.end(); ++it) {
            Receiver::ptr_t receiver = it->second;
            if (!it->second) continue;
            std::string name = receiver->GetOption().Get<std::string>("name");
            std::string stats = receiver->GetStatistics(1, ", ");
            Logger::Info(boost::format("<%s> stats receive [ %s ] : %s") % app() % name % stats);
        }
        stats_time_ += std::chrono::seconds(stats_);
        return false;
    }
    virtual bool GetListenerFlag() const override {
        return stats_ > 0 && std::chrono::steady_clock::now() > stats_time_;
    }
    virtual bool OnDisconnected(const ReceiveOption& option) override {
        std::string name = option.Get<std::string>("name");
        std::string peer = option.Get<std::string>("peer");
        Logger::Info(boost::format("<%s> disconnected [ %s ] from %s") % app() % name % peer);
        boost::mutex::scoped_lock lock(mutex_);
        Receiver::map_t::iterator it = receivers_.find(name);;
        if (it != receivers_.end() && it->second) {
            boost::thread([](Receiver::ptr_t receiver) { receiver.reset(); }, it->second);
            it->second.reset();
        }
        return true;
    }
};

//----------------------------------------------------------------------------
/// @class App
//----------------------------------------------------------------------------
class App
{
    static boost::mutex mutex_;
    static boost::condition_variable cond_;
    std::string name_;
    std::string version_;
    Json conf_;
    Reflect::vector_t reflects_;
public:
    App(const std::string& version) : name_("srt-live-reflect"), version_(version), conf_(), reflects_() {
    }
    virtual ~App() {
        Destroy();
    }
    virtual bool Initialize(const std::string& conf_file) {
        try {
            conf_ = Json::load(conf_file);
        } catch (const std::exception& ex) {
            Logger::Info(boost::format("%s version %s (srt:%s) : started") % name_ % version_ % SRT_VERSION_STRING);
            Logger::Fatal(boost::format("ERROR: failed to read conf [ %s ] : %s") % conf_file % ex.what());
            return false;
        }
        name_ = conf_["name"].to<std::string>("srt-live-reflect");
        try {
            Logger::Init(conf_["logger"], name_.empty() ? "srt-live-reflect" : name_);
            Logger::Info(boost::format("%s version %s (srt:%s) : started") % name_ % version_ % SRT_VERSION_STRING);
        } catch (const std::exception& ex) {
            Logger::Info(boost::format("%s version %s (srt:%s) : started") % name_ % version_ % SRT_VERSION_STRING);
            Logger::Warning(boost::format("WARNING: failed to initialize logger : %s") % ex.what());
        }
        Logger::Debug(boost::format("conf: %s") % conf_.serialize(2));
        CurlGlobal::SetUserAgent(name_.c_str());
        CurlGlobal::SetCertificateAuthority(conf_["cainfo"].to<boost::filesystem::path>().string().c_str());
        if (srt_startup() == SRT_ERROR) {
            Logger::Fatal(boost::format("ERROR: srt_startup failed : %s") % srt_getlasterror_str());
            return false;
        }
        std::string srtloglevel = conf_["srtloglevel"].to<std::string>("error");
        if (boost::istarts_with(srtloglevel, "t") || boost::istarts_with(srtloglevel, "d")) {
            srt_setloglevel(srt_logging::LogLevel::debug); // trace, debug
        } else if (boost::istarts_with(srtloglevel, "n") || boost::istarts_with(srtloglevel, "i")) {
            srt_setloglevel(srt_logging::LogLevel::note); // note, info
        } else if (boost::istarts_with(srtloglevel, "w")) {
            srt_setloglevel(srt_logging::LogLevel::warning);
        } else if (boost::istarts_with(srtloglevel, "f")) {
            srt_setloglevel(srt_logging::LogLevel::fatal);
        } else {
            srt_setloglevel(srt_logging::LogLevel::error); // as default
        }
        srt_setloghandler(this, &App::logHandler);
        if (conf_["aws"]["enabled"].to<int>(0)) {
#if !defined(USE_AWSSDK)
            Logger::Fatal(boost::format("ERROR: AWSSDK is not configured"));
            return false;
#endif
            if (!AWS::Init(conf_)) {
                Logger::Fatal(boost::format("ERROR: AWS::Init failed"));
                return false;
            }
            //AWS::Test();
            //return false;
        }
        return true;
    }
    virtual void Destroy() {
        reflects_.clear();
        srt_cleanup();
        if (conf_["aws"]["enabled"].to<int>(0)) {
            AWS::Term();
        }
        Logger::Info(boost::format("%s version %s : stopped") % name_ % version_);
        Logger::Term();
    }
    virtual int Run() {
        Json::Node reflects = conf_["reflects"];
        for (size_t i = 0, c = reflects.size(); i < c; ++i) {
            Json conf = reflects[i];
            Json::keys_t keys = conf.to<Json::keys_t>();
            for (Json::keys_t::const_iterator it = keys.begin(); it != keys.end(); ++it) {
                if (!boost::iequals(*it, "uri")) continue;
                URI uri = conf[*it].to<std::string>();
                if (!boost::iequals(uri.scheme, "srt")) continue;
                URIOption query = uri.query;
                uri.Decode();
                conf["host"] = uri.host.c_str();
                conf["port"] = uri.port.c_str();
                const URIOption::map_t& map = query.GetMap();
                for (URIOption::map_t::const_iterator it = map.begin(); it != map.end(); ++it) {
                    conf["option"][it->first] = it->second.c_str();
                }
            }
            Reflect::ptr_t reflect = Reflect::Create(conf);
            if (reflect->Initialize()) reflects_.push_back(reflect);
        }
        if (reflects_.empty()) {
            Logger::Fatal("ERROR: there are no reflection entry");
            return -2;
        }
#ifdef SIGBREAK
        std::signal(SIGBREAK, &App::signalHandler);
#endif
        std::signal(SIGINT, &App::signalHandler);
        std::signal(SIGTERM, &App::signalHandler);
        boost::unique_lock<boost::mutex> lk(mutex_);
#if defined(_DEBUG) && defined(WIN32)
        while (!cond_.timed_wait(lk, boost::posix_time::milliseconds(100)) && (!_kbhit() || _getche() != 'q'));
#else
        cond_.wait(lk);
#endif
        return 0;
    }
protected:
    static void signalHandler(int signum) {
        Logger::Info(boost::format("signal : %d") % signum);
        boost::unique_lock<boost::mutex> lk(mutex_);
        cond_.notify_all();
    }
    static void logHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message) {
        switch (level) {
            case srt_logging::LogLevel::fatal: {
                Logger::Fatal(boost::format("%s(%d): %s : %s : %s") % file % line % area % "fatal" % message);
                break;
            }
            case srt_logging::LogLevel::error: {
                Logger::Error(boost::format("%s(%d): %s : %s : %s") % file % line % area % "error" % message);
                break;
            }
            case srt_logging::LogLevel::warning: {
                Logger::Warning(boost::format("%s(%d): %s : %s : %s") % file % line % area % "warn" % message);
                break;
            }
            case srt_logging::LogLevel::note: {
                Logger::Info(boost::format("%s(%d): %s : %s : %s") % file % line % area % "note" % message);
                break;
            }
            case srt_logging::LogLevel::debug: {
                Logger::Debug(boost::format("%s(%d): %s : %s : %s") % file % line % area % "debug" % message);
                break;
            }
        }
    }
};

boost::mutex App::mutex_;
boost::condition_variable App::cond_;

#define MAKE_VERSION(MAJOR, MINOR, PATCH) #MAJOR "." #MINOR "." #PATCH
#define VERSION MAKE_VERSION(0, 3, 1)

//----------------------------------------------------------------------------
/// @fn main
//----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    try {
        std::string conf_file;
        for (int i = 1; i < argc; ++i) {
            if (boost::istarts_with(argv[i], "conf=")) {
                conf_file = std::string(argv[i] + 5);
            }
        }
        if (conf_file.empty()) {
            conf_file = "./srt-live-reflect.conf";
        }
        App app(VERSION);
        if (!app.Initialize(conf_file)) {
            return -1;
        }
        return app.Run();
    } catch (std::exception& ex) {
        Logger::Fatal(boost::format("exception : %s") % ex.what());
        return -3;
    }
}
