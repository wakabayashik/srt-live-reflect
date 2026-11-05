#include "stdafx.h"
#include "curl.h"

#if defined(WIN32) || defined(WIN64)
#include <MSTcpIp.h> // tcp_keepalive
#else
#include <netinet/tcp.h> // for TCP_KEEPIDLE, TCP_KEEPINTVL
#endif

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
std::mutex CurlGlobal::mutex_;
std::string CurlGlobal::cainfo_;
std::string CurlGlobal::ua_;
long CurlGlobal::timeout_ = 30;

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void    CurlGlobal::SetCertificateAuthority(const char* cainfo) {
    lock_t lk(mutex_);
    cainfo_ = cainfo ? cainfo : "";
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void    CurlGlobal::SetUserAgent(const char* ua) {
    lock_t lk(mutex_);
    ua_ = ua ? ua : "";
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void    CurlGlobal::SetDefaultTimeout(long timeout) {
    lock_t lk(mutex_);
    timeout_ = timeout;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
std::string CurlGlobal::GetCertificateAuthority() {
    lock_t lk(mutex_);
    return cainfo_;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
std::string CurlGlobal::GetUserAgent() {
    lock_t lk(mutex_);
    return ua_;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
long CurlGlobal::GetDefaultTimeout() {
    lock_t lk(mutex_);
    return timeout_;
}

//----------------------------------------------------------------------------
/// @param[in]      keepalive           -1:設定しない(default) 0:キープアライブを無効 1:キープアライブを有効
/// @param[in]      keepidle            最初に tcp_probe を送信するまでの時間[sec]  (default:7200)
/// @param[in]      keepintvl           tcp_probe 失敗時の再送間隔[sec] (linux_default:75 windows_default:1)
/// @param[in]      keepcnt             tcp_probe 失敗時の再送回数 (linux_default:9 windows_default:5 or 10)
//----------------------------------------------------------------------------
Curl::Curl(int keepalive, int keepidle, int keepintvl)
    : curl_(curl_easy_init(), curl_easy_cleanup)
    , keepalive_(keepalive)
    , keepidle_(keepidle)
    , keepintvl_(keepintvl)
{}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
Curl::Curl(const Curl& rhs)
    : curl_(rhs.curl_)
    , keepalive_(rhs.keepalive_)
    , keepidle_(rhs.keepidle_)
    , keepintvl_(rhs.keepintvl_)
{
    curl_ = rhs.curl_;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
Curl::~Curl() {
    curl_.reset();
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
Curl&  Curl::operator=(const Curl& rhs) {
    if (&rhs != this) {
        curl_ = rhs.curl_;
        keepalive_ = rhs.keepalive_;
        keepidle_ = rhs.keepidle_;
        keepintvl_ = rhs.keepintvl_;
    }
    return *this;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void    Curl::Reset() {
    curl_easy_reset(curl_.get());
    if (keepalive_ >= 0) {
        curl_easy_setopt(curl_.get(), CURLOPT_SOCKOPTFUNCTION, CallbackSockOptWrapper);
        curl_easy_setopt(curl_.get(), CURLOPT_SOCKOPTDATA, this);
    }
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
int     Curl::CallbackSockOptWrapper(void* clientp, curl_socket_t curlfd, curlsocktype purpose) {
    return static_cast<Curl*>(clientp)->CallbackSockOpt(curlfd, purpose);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
int     Curl::CallbackSockOpt(curl_socket_t curlfd, curlsocktype purpose) const {
    switch (purpose) {
        case CURLSOCKTYPE_IPCXN: {
#if defined(WIN32) || defined(WIN64)
            {
                tcp_keepalive   t;
                t.onoff = keepalive_;
                t.keepalivetime = static_cast<ULONG>(keepidle_) * 1000;    // sec -> millisec
                t.keepaliveinterval = static_cast<ULONG>(keepintvl_) * 1000;   // sec -> millisec
                DWORD   d = 0;
                if (WSAIoctl(curlfd, SIO_KEEPALIVE_VALS, &t, sizeof(t), nullptr, 0, &d, nullptr, nullptr) >= 0)
                    break; // done
            }
#endif
            if (setsockopt(curlfd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive_), sizeof(keepalive_)) >= 0) {
#ifdef TCP_KEEPIDLE
                setsockopt(curlfd, IPPROTO_TCP, TCP_KEEPIDLE, reinterpret_cast<const char*>(&keepidle_), sizeof(keepidle_));
#endif
#ifdef TCP_KEEPINTVL
                setsockopt(curlfd, IPPROTO_TCP, TCP_KEEPINTVL, reinterpret_cast<const char*>(&keepintvl_), sizeof(keepintvl_));
#endif
//#ifdef TCP_KEEPCNT
//                setsockopt(curlfd, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char*>(&keepcnt_), sizeof(keepcnt_));
//#endif
            }
            break;
        }
        default: {
            break;
        }
    }
    return 0;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
static size_t   NoOperation(void* ptr, size_t size, size_t nmemb, void* stream) {
    return size * nmemb;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void    CurlStrList::Append(const char* fmt, ...) {
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    AppendV(fmt, ap);
    va_end(ap);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void    CurlStrList::AppendV(const char* fmt, va_list ap) {
    if (!fmt) return;
#if defined(WIN32) || defined(WIN64)
    size_t len = _vscprintf(fmt, ap);
    std::vector<char> buf(len + 1, '\0');
    buf.resize(_vsnprintf(buf.data(), len, fmt, ap) + 1);
#else
    va_list ap2;
    va_copy(ap2, ap);
    size_t len = vsnprintf(nullptr, 0, fmt, ap2);
    std::vector<char> buf(len + 1, '\0');
    buf.resize(vsnprintf(buf.data(), len + 1, fmt, ap) + 1);
    va_end(ap2);
#endif
    slist_ = curl_slist_append(slist_, buf.data());
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
CurlStrIO::CurlStrIO(Curl curl)
    : curl_(curl)
    , head_()
    , body_()
    , request_body_()
{}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
bool    CurlStrIO::Reset(long timeout, const char* body) {
    memset(error_, 0, sizeof(error_));
    body_.clear();
    head_.clear();
    if (!curl_) return false;
    curl_.Reset();
//#ifdef _DEBUG
//    curl_easy_setopt(*this, CURLOPT_VERBOSE, 1);
//#endif
    timeout = (timeout > 0) ? timeout : CurlGlobal::GetDefaultTimeout();
    curl_easy_setopt(*this, CURLOPT_TIMEOUT, 0);
    curl_easy_setopt(*this, CURLOPT_CONNECTTIMEOUT, timeout);
    curl_easy_setopt(*this, CURLOPT_FTP_RESPONSE_TIMEOUT, timeout);
    curl_easy_setopt(*this, CURLOPT_ERRORBUFFER, error_);
    curl_easy_setopt(*this, CURLOPT_HEADERFUNCTION, CallbackHeaderWrapper);
    curl_easy_setopt(*this, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(*this, CURLOPT_WRITEFUNCTION, CallbackWriteWrapper);
    curl_easy_setopt(*this, CURLOPT_WRITEDATA, this);
    if (body && *body) {
        request_body_ = body_ = body;
        curl_easy_setopt(*this, CURLOPT_READFUNCTION, CallbackReadWrapper);
        curl_easy_setopt(*this, CURLOPT_READDATA, this);
        curl_easy_setopt(*this, CURLOPT_SEEKFUNCTION, CallbackSeekWrapper);
        curl_easy_setopt(*this, CURLOPT_SEEKDATA, this);
        curl_easy_setopt(*this, CURLOPT_UPLOAD, 1);
        curl_easy_setopt(*this, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(body_.length()));
    } else {
        curl_easy_setopt(*this, CURLOPT_READFUNCTION, NoOperation);
        curl_easy_setopt(*this, CURLOPT_UPLOAD, 0);
        curl_easy_setopt(*this, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(*this, CURLOPT_MAXREDIRS, 5);
    }
    std::string ua = CurlGlobal::GetUserAgent();
    if (!ua.empty()) curl_easy_setopt(*this, CURLOPT_USERAGENT, ua.c_str());
    // set certificate authority for SSL
    std::string cainfo = CurlGlobal::GetCertificateAuthority();
    curl_easy_setopt(*this, CURLOPT_SSL_VERIFYPEER, cainfo.empty() ? 0 : 1);
    curl_easy_setopt(*this, CURLOPT_SSL_VERIFYHOST, cainfo.empty() ? 0 : 2);
    curl_easy_setopt(*this, CURLOPT_CAINFO, cainfo.c_str());
    return true;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
size_t  CurlStrIO::CallbackHeaderWrapper(void* ptr, size_t size, size_t nmemb, void* stream) {
    return static_cast<CurlStrIO*>(stream)->CallbackHeader(static_cast<const char*>(ptr), size * nmemb);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
size_t  CurlStrIO::CallbackReadWrapper(void* ptr, size_t size, size_t nmemb, void* stream) {
    return static_cast<CurlStrIO*>(stream)->CallbackRead(static_cast<char*>(ptr), size * nmemb);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
size_t  CurlStrIO::CallbackWriteWrapper(void* ptr, size_t size, size_t nmemb, void* stream) {
    return static_cast<CurlStrIO*>(stream)->CallbackWrite(static_cast<const char*>(ptr), size * nmemb);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
int     CurlStrIO::CallbackSeekWrapper(void* stream, curl_off_t offset, int origin) {
    return static_cast<CurlStrIO*>(stream)->CallbackSeek(offset, origin);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
size_t  CurlStrIO::CallbackHeader(const char* str, size_t len) {
    head_.append(str, len);
    return len;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
size_t  CurlStrIO::CallbackRead(char* str, size_t len) {
    len = std::min<size_t>(len, body_.length());
    strncpy(str, body_.c_str(), len);
    body_.erase(0, len);
    return len;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
size_t  CurlStrIO::CallbackWrite(const char* str, size_t len) {
    body_.append(str, len);
    return len;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
int     CurlStrIO::CallbackSeek(curl_off_t offset, int origin) {
    switch (origin) {
        // fall through
        case SEEK_CUR: offset -= body_.length();
        case SEEK_END: offset += request_body_.length();
        case SEEK_SET: break;
        default: return -1;
    }
    if (offset < 0 || static_cast<curl_off_t>(request_body_.length()) < offset) {
        return -1;
    }
    body_ = request_body_.substr(static_cast<unsigned>(offset));
    return 0;
}


//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
CurlJsonIO::CurlJsonIO(Curl curl)
    : CurlStrIO(curl)
    , header_()
{
    header_.Append("Expect:"); // disable "Expect: 100-continue"
    header_.Append("Accept: application/json");
    header_.Append("Content-Type: application/json");
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
bool CurlJsonIO::Reset(long timeout, const boost::json::value& data) {
    std::string body  = boost::json::serialize(data);
    if (!CurlStrIO::Reset(timeout, body.c_str())) {
        return false;
    }
    curl_easy_setopt(*this, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(*this, CURLOPT_HTTPHEADER, header_());
    curl_easy_setopt(*this, CURLOPT_FAILONERROR, 1);
    return true;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
boost::json::value CurlJsonIO::Json() {
    try {
        return boost::json::parse(Body());
    } catch (boost::system::system_error&) {
        return boost::json::value();
    }
}
