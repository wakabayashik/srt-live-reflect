#pragma once

//----------------------------------------------------------------------------
/// @class CurlGlobal
//----------------------------------------------------------------------------
class CurlGlobal
{
    using mutex_t = boost::mutex;
    using lock_t = boost::mutex::scoped_lock;
    static mutex_t mutex_;
    static std::string cainfo_;
    static std::string ua_;
    static long timeout_;
public:
    static void SetCertificateAuthority(const char* cainfo);
    static void SetUserAgent(const char* ua);
    static void SetDefaultTimeout(long timeout);
    static std::string GetCertificateAuthority();
    static std::string GetUserAgent();
    static long GetDefaultTimeout();
};

//----------------------------------------------------------------------------
/// @class Curl
//----------------------------------------------------------------------------
class Curl
{
    std::shared_ptr<CURL> curl_;
    int keepalive_;
    int keepidle_;
    int keepintvl_;
public:
    Curl(int keepalive = -1, int keepidle = 60, int keepintvl = 60);
    Curl(const Curl& rhs);
    virtual ~Curl();
    virtual Curl& operator=(const Curl& rhs);
    virtual void Reset();
    virtual CURLcode Perform() { return curl_easy_perform(curl_.get()); }
    virtual operator CURL*() { return curl_.get(); }
protected:
    static int CallbackSockOptWrapper(void* clientp, curl_socket_t curlfd, curlsocktype purpose);
    virtual int CallbackSockOpt(curl_socket_t curlfd, curlsocktype purpose) const;
};

//----------------------------------------------------------------------------
/// @class CurlStrList
//----------------------------------------------------------------------------
class CurlStrList
{
    curl_slist* slist_;
public:
    CurlStrList() : slist_(NULL) {}
    CurlStrList(const CurlStrList&) = delete;
    CurlStrList& operator=(const CurlStrList&) = delete;
    ~CurlStrList() { if (slist_) curl_slist_free_all(slist_); }
    void Append(const char* fmt, ...);
    void AppendV(const char* fmt, va_list ap);
    const curl_slist* operator()() const { return slist_; }
};

//----------------------------------------------------------------------------
/// @class CurlStrIO
//----------------------------------------------------------------------------
class CurlStrIO
{
    Curl curl_;
    char error_[CURL_ERROR_SIZE + 1];
    std::string head_;
    std::string body_;
    std::string request_body_;
public:
    CurlStrIO(Curl curl);
    virtual ~CurlStrIO() { curl_.Reset(); }
    virtual operator CURL*() { return curl_; }
    virtual bool Reset(long timeout, const char* body);
    virtual const char* Error() { return error_; }
    virtual std::string& Head() { return head_; }
    virtual std::string& Body() { return body_; }
protected:
    static size_t CallbackHeaderWrapper(void* ptr, size_t size, size_t nmemb, void* stream);
    static size_t CallbackReadWrapper(void* ptr, size_t size, size_t nmemb, void* stream);
    static size_t CallbackWriteWrapper(void* ptr, size_t size, size_t nmemb, void* stream);
    static int CallbackSeekWrapper(void* stream, curl_off_t offset, int origin);
    virtual size_t CallbackHeader(const char* str, size_t len);
    virtual size_t CallbackRead(char* str, size_t len);
    virtual size_t CallbackWrite(const char* str, size_t len);
    virtual int CallbackSeek(curl_off_t offset, int origin);
};

//----------------------------------------------------------------------------
/// @class CurlJsonIO
//----------------------------------------------------------------------------
class CurlJsonIO : public CurlStrIO
{
    CurlStrList header_;
public:
    CurlJsonIO(Curl curl);
    virtual bool Reset(long timeout, const boost::json::value& data);
    virtual boost::json::value Json();
};
