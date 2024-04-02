#include "stdafx.h"
#include "aws.h"

//----------------------------------------------------------------------------
/// @class DummyStream
//----------------------------------------------------------------------------
class DummyStream : public std::istream {
    class DummyBuf : public std::streambuf {};
    DummyBuf buf_;
public:
    DummyStream() : std::istream(&buf_) { this->get(); } // make it eof
};
static DummyStream dummyStream;

#if !defined(USE_AWSSDK)
bool AWS::S3Get::IsRunning() const {
    return false;
}
bool AWS::S3Get::Abort() {
    return false;
}
bool AWS::S3Get::Wait() {
    return false;
}
std::istream& AWS::S3Get::GetStream() {
    return dummyStream;
}
bool AWS::S3Put::IsRunning() const {
    return false;
}
bool AWS::S3Put::Abort() {
    return false;
}
bool AWS::S3Put::Wait() {
    return false;
}
bool AWS::S3Client::ListBuckets(std::vector<std::string>& list) {
    return false;
}
bool AWS::S3Client::CreateBucket(const std::string& bucketName) {
    return false;
}
bool AWS::S3Client::DeleteBucket(const std::string& bucketName) {
    return false;
}
bool AWS::S3Client::List(const std::string& bucketName, const std::string& marker, std::vector<std::string>& list) {
    return false;
}
bool AWS::S3Client::Delete(const std::string& bucketName, const std::string& keyName) {
    return false;
}
bool AWS::S3Client::Head(const std::string& bucketName, const std::string& keyName) {
    return false;
}
AWS::S3Get AWS::S3Client::GetAsync(const std::string& bucketName, const std::string& keyName, uint64_t offset, size_t bufSiz, const done_t& done, const fail_t& fail) {
    return S3Get();
}
AWS::S3Put AWS::S3Client::PutAsync(const std::string& bucketName, const std::string& keyName, const std::string& srcFile, const done_t& done, const fail_t& fail) {
    return S3Put();
}
bool AWS::Init(const Json& conf) {
    return false;
}
void AWS::Term() {
}
bool AWS::Test() {
    return false;
}
#else

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/core/utils/stream/ConcurrentStreamBuf.h>

#if defined(WIN32) || defined(WIN64)
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#endif

#include "logger.h"

//----------------------------------------------------------------------------
/// @class S3Async
//----------------------------------------------------------------------------
class S3Async : private boost::noncopyable {
protected:
    typedef boost::shared_ptr<Aws::S3::S3Client> client_t;
    typedef boost::mutex mutex_t;
    typedef boost::unique_lock<boost::mutex> lock_t;
    typedef boost::condition_variable cond_t;
    enum State { Failed = -1, Ready, Running, Done };
    client_t client_;
    Aws::String bucketName_;
    Aws::String keyName_;
    AWS::done_t done_;
    AWS::fail_t fail_;
    State state_;
    mutex_t mutex_;
    cond_t cond_;
    S3Async(client_t client, const std::string& bucketName, const std::string& keyName, const AWS::done_t& done, const AWS::fail_t& fail)
        : client_(client), bucketName_(bucketName), keyName_(keyName), done_(done), fail_(fail), state_(Ready), mutex_(), cond_() {
    }
public:
    virtual ~S3Async() {
    }
    virtual bool IsRunning() const {
        return state_ == Running;
    }
    virtual bool Abort() {
        lock_t lk(mutex_);
        if (!IsRunning()) return false;
        client_->DisableRequestProcessing(); // all requests sharing the s3client will be aborted
        return true;
    }
    virtual bool Wait() {
        lock_t lk(mutex_);
        if (IsRunning()) cond_.wait(lk);
        return state_ == Done;
    }
    virtual bool Begin() = 0;
protected:
    void OnFail(const std::string& name, const std::string& message) {
        state_ = Failed;
        cond_.notify_one();
        if (!fail_) return;
        AWS::fail_t fail = fail_;
        boost::thread([=]() { fail(name, message); });
    }
    void OnDone() {
        state_ = Done;
        cond_.notify_one();
        if (!done_) return;
        AWS::done_t done = done_;
        boost::thread([=]() { done(); });
    }
};

//----------------------------------------------------------------------------
/// @class AWS::S3Get::Impl
//----------------------------------------------------------------------------
class AWS::S3Get::Impl : public S3Async {
    uint64_t offset_;
    Aws::Utils::Stream::ConcurrentStreamBuf buf_;
    std::istream istream_;
    bool abort_requested_;
public:
    Impl(client_t client, const std::string& bucketName, const std::string& keyName, uint64_t offset, size_t bufSiz, const done_t& done, const fail_t& fail)
        : S3Async(client, bucketName, keyName, done, fail), offset_(offset), buf_(bufSiz), istream_(&buf_), abort_requested_(false) {
    }
    virtual ~Impl() {
        Wait();
    }
    virtual bool Abort() override {
        lock_t lk(mutex_);
        if (!IsRunning()) return false;
        abort_requested_ = true;
        client_->DisableRequestProcessing(); // all requests sharing the s3client will be aborted
        buf_.SetEofInput();
        return true;
    }
    virtual std::istream& GetStream() {
        return istream_;
    }
    virtual bool Begin() override {
        Logger::Trace(boost::format("AWS::S3Get(%s, %s) bytes=%llu- ->") % bucketName_ % keyName_ % offset_);
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(bucketName_);
        request.SetKey(keyName_);
        if (offset_) request.SetRange((boost::format("bytes=%llu-") % offset_).str());
        request.SetResponseStreamFactory([this]() {
            return Aws::New<Aws::IOStream>("S3GetIOStreamAllocationTag", &buf_); // raw pointer, not shared_ptr
        });
        client_->GetObjectAsync(request, [this](const Aws::S3::S3Client* client,
            const Aws::S3::Model::GetObjectRequest& request, const Aws::S3::Model::GetObjectOutcome& outcome,
            const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context) {
            lock_t lk(mutex_);
            buf_.SetEofInput();
            if (!outcome.IsSuccess()) {
                const Aws::S3::S3Error& err = outcome.GetError();
                if (abort_requested_) {
                    Logger::Trace(boost::format("AWS::S3Get(%s, %s): Abort: %s: %s") % bucketName_ % keyName_ % err.GetExceptionName() % err.GetMessage());
                } else {
                    Logger::Error(boost::format("AWS::S3Get(%s, %s): Error: %s: %s") % bucketName_ % keyName_ % err.GetExceptionName() % err.GetMessage());
                }
                OnFail(err.GetExceptionName(), err.GetMessage());
            } else {
                Logger::Trace(boost::format("AWS::S3Get(%s, %s): Done") % bucketName_ % keyName_);
                OnDone();
            }

        });
        state_ = Running;
        return true;
    }
};

//----------------------------------------------------------------------------
/// @class AWS::S3Put::Impl
//----------------------------------------------------------------------------
class AWS::S3Put::Impl : public S3Async {
    Aws::String srcFile_;
public:
    Impl(client_t client, const std::string& bucketName, const std::string& keyName, const std::string& srcFile, const done_t& done, const fail_t& fail)
        : S3Async(client, bucketName, keyName, done, fail), srcFile_(srcFile) {
    }
    virtual ~Impl() {
        Wait();
    }
    virtual bool Begin() override {
        Logger::Trace(boost::format("AWS::S3Put(%s, %s, %s) ->") % bucketName_ % keyName_ % srcFile_);
        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(bucketName_);
        request.SetKey(keyName_);
        const std::shared_ptr<Aws::IOStream> infile = Aws::MakeShared<Aws::FStream>("S3PutFstreamAllocationTag", srcFile_.c_str(), std::ios_base::in | std::ios_base::binary);
        if (!*infile) {
            boost::format msg = boost::format("AWS::S3Put(%s, %s, %s): Error: unable to open file") % bucketName_ % keyName_ % srcFile_;
            Logger::Error(msg);
            OnFail("missing file", msg.str());
            return false;
        }
        request.SetBody(infile);
        client_->PutObjectAsync(request, [this](const Aws::S3::S3Client* client,
            const Aws::S3::Model::PutObjectRequest& request, const Aws::S3::Model::PutObjectOutcome& outcome,
            const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context) {
            lock_t lk(mutex_);
            if (!outcome.IsSuccess()) {
                const Aws::S3::S3Error& err = outcome.GetError();
                Logger::Error(boost::format("AWS::S3Put(%s, %s, %s): Error: %s: %s") % bucketName_ % keyName_ % srcFile_ % err.GetExceptionName() % err.GetMessage());
                OnFail(err.GetExceptionName(), err.GetMessage());
            } else {
                Logger::Trace(boost::format("AWS::S3Put(%s, %s, %s): Done") % bucketName_ % keyName_ % srcFile_);
                OnDone();
            }
        });
        state_ = Running;
        return true;
    }
};

//----------------------------------------------------------------------------
/// @class AWS::S3Client::Impl
//----------------------------------------------------------------------------
class AWS::S3Client::Impl {
    Aws::String region_;
    boost::shared_ptr<Aws::S3::S3Client> client_;
public:
    Impl(const Aws::Client::ClientConfiguration& clientConfig)
        : region_(clientConfig.region), client_(new Aws::S3::S3Client(clientConfig)) {
    }
    virtual ~Impl() {
        client_.reset();
    }
    virtual bool ListBuckets(std::vector<std::string>& list) const {
        Logger::Trace(boost::format("AWS::S3Client::ListBuckets() ->"));
        Aws::S3::Model::ListBucketsOutcome outcome = client_->ListBuckets();
        list.clear();
        if (!outcome.IsSuccess()) {
            const Aws::S3::S3Error& err = outcome.GetError();
            Logger::Error(boost::format("AWS::S3Client::ListBuckets(): Error: %s: %s") % err.GetExceptionName() % err.GetMessage());
            return false;
        } else {
            Aws::Vector<Aws::S3::Model::Bucket> buckets = outcome.GetResult().GetBuckets();
            Logger::Trace(boost::format("AWS::S3Client::ListBuckets(): Found: %d") % outcome.GetResult().GetBuckets().size());
            for (Aws::S3::Model::Bucket& b : buckets) list.push_back(b.GetName());
            return true;
        }
    }
    virtual bool CreateBucket(const Aws::String& bucketName) {
        Logger::Trace(boost::format("AWS::S3Client::CreateBucket(%s) ->") % bucketName);
        Aws::S3::Model::CreateBucketRequest request;
        request.SetBucket(bucketName);
        Aws::S3::Model::CreateBucketConfiguration createBucketConfig;
        if (region_ != "us-east-1") {
            createBucketConfig.SetLocationConstraint(Aws::S3::Model::BucketLocationConstraintMapper::GetBucketLocationConstraintForName(region_));
        }
        request.SetCreateBucketConfiguration(createBucketConfig);
        Aws::S3::Model::CreateBucketOutcome outcome = client_->CreateBucket(request);
        if (!outcome.IsSuccess()) {
            const Aws::S3::S3Error& err = outcome.GetError();
            Logger::Error(boost::format("AWS::S3Client::CreateBucket(%s): Error: %s: %s") % bucketName % err.GetExceptionName() % err.GetMessage());
            return false;
        } else {
            Logger::Trace(boost::format("AWS::S3Client::CreateBucket(%s): Done") % bucketName);
            return true;
        }
    }
    virtual bool DeleteBucket(const Aws::String& bucketName) {
        Logger::Trace(boost::format("AWS::S3Client::DeleteBucket(%s) ->") % bucketName);
        Aws::S3::Model::DeleteBucketRequest request;
        request.SetBucket(bucketName);
        Aws::S3::Model::DeleteBucketOutcome outcome = client_->DeleteBucket(request);
        if (!outcome.IsSuccess()) {
            const Aws::S3::S3Error& err = outcome.GetError();
            Logger::Error(boost::format("AWS::S3Client::DeleteBucket(%s): Error: %s: %s") % bucketName % err.GetExceptionName() % err.GetMessage());
            return false;
        } else {
            Logger::Trace(boost::format("AWS::S3Client::DeleteBucket(%s): Done") % bucketName);
            return true;
        }
    }
    virtual bool List(const std::string& bucketName, const std::string& marker0, std::vector<std::string>& list) const {
        Logger::Trace(boost::format("AWS::S3Client::List(%s, %s) ->") % bucketName % marker0);
        list.clear();
        std::string marker = marker0;
        std::string key = boost::trim_copy_if(marker0, boost::is_any_of(" ./\\"));
        if (!key.empty()) key += "/";
        for (;;) {
            Aws::S3::Model::ListObjectsRequest request;
            request.SetBucket(bucketName);
            request.SetMaxKeys(1000);
            if (!marker.empty()) request.SetMarker(marker);
            Aws::S3::Model::ListObjectsOutcome outcome = client_->ListObjects(request);
            if (!outcome.IsSuccess()) {
                const Aws::S3::S3Error& err = outcome.GetError();
                Logger::Error(boost::format("AWS::S3Client::List(%s, %s): Error: %s: %s") % bucketName % marker %  err.GetExceptionName() % err.GetMessage());
                return false;
            } else {
                Aws::Vector<Aws::S3::Model::Object> objects = outcome.GetResult().GetContents();
                Logger::Trace(boost::format("AWS::S3Client::List(%s, %s): Found: %u") % bucketName % marker % objects.size());
                marker = "";
                for (Aws::S3::Model::Object& object : objects) {
                    marker = object.GetKey();
                    if (!key.empty()) {
                        if (!boost::istarts_with(marker, key)) continue; // skip contents not in the key folder
                        if (marker.find('/', key.size()) != std::string::npos) continue; // skip contents in descendant folder of the key folder
                    }
                    list.push_back(object.GetKey());
                }
                if (objects.size() < static_cast<size_t>(request.GetMaxKeys()) || marker.empty()) return true;
            }
        }
    }
    virtual bool Delete(const Aws::String& bucketName, const Aws::String& keyName) {
        Logger::Trace(boost::format("AWS::S3Client::Delete(%s, %s) ->") % bucketName % keyName);
        Aws::S3::Model::DeleteObjectRequest request;
        request.SetBucket(bucketName);
        request.SetKey(keyName);
        Aws::S3::Model::DeleteObjectOutcome outcome = client_->DeleteObject(request);
        if (!outcome.IsSuccess()) {
            const Aws::S3::S3Error& err = outcome.GetError();
            Logger::Error(boost::format("AWS::S3Client::Delete(%s, %s): Error: %s: %s") % bucketName % keyName %  err.GetExceptionName() % err.GetMessage());
            return false;
        } else {
            Logger::Trace(boost::format("AWS::S3Client::Delete(%s, %s): Done") % bucketName % keyName);
            return true;
        }
    }
    virtual bool Head(const Aws::String& bucketName, const Aws::String& keyName) {
        Logger::Trace(boost::format("AWS::S3Client::Head(%s, %s) ->") % bucketName % keyName);
        Aws::S3::Model::HeadObjectRequest request;
        request.SetBucket(bucketName);
        request.SetKey(keyName);
        Aws::S3::Model::HeadObjectOutcome outcome = client_->HeadObject(request);
        if (!outcome.IsSuccess()) {
            const Aws::S3::S3Error& err = outcome.GetError();
            Logger::Error(boost::format("AWS::S3Client::Head(%s, %s): Error: %s: %s") % bucketName % keyName %  err.GetExceptionName() % err.GetMessage());
            return false;
        } else {
            Aws::S3::Model::HeadObjectResult& result = outcome.GetResult();
            const Aws::Utils::DateTime& mod = result.GetLastModified();
            Logger::Trace(boost::format("AWS::S3Client::Head(%s, %s): Done : %lld[bytes], %s") % bucketName % keyName % result.GetContentLength() % mod.ToLocalTimeString("%Y-%m-%dT%H:%M:%S"));
            return true;
        }
    }
    virtual S3Get GetAsync(const std::string& bucketName, const std::string& keyName, uint64_t offset, size_t bufSiz, const done_t& done, const fail_t& fail) {
        S3Get::pimpl_t impl(new S3Get::Impl(client_, bucketName, keyName, offset, bufSiz, done, fail));
        return impl->Begin() ? S3Get(impl) : S3Get();
    }
    virtual S3Put PutAsync(const std::string& bucketName, const std::string& keyName, const std::string& srcFile, const done_t& done, const fail_t& fail) {
        S3Put::pimpl_t impl(new S3Put::Impl(client_, bucketName, keyName, srcFile, done, fail));
        return impl->Begin() ? S3Put(impl) : S3Put();
    }
};

//----------------------------------------------------------------------------
/// @class AwsLogAdapter
//----------------------------------------------------------------------------
class AwsLogAdapter : public Aws::Utils::Logging::LogSystemInterface {
    Aws::Utils::Logging::LogLevel logLevel_;
    std::string prefix_;
public:
    AwsLogAdapter(Aws::Utils::Logging::LogLevel logLevel, const std::string& prefix) : logLevel_(logLevel), prefix_(prefix) {}
    virtual Aws::Utils::Logging::LogLevel GetLogLevel(void) const override {
        return logLevel_;
    }
    virtual void Log(Aws::Utils::Logging::LogLevel logLevel, const char* tag, const char* formatStr, ...) override {
        va_list ap;
        va_start(ap, formatStr);
#if defined(WIN32) || defined(WIN64)
        size_t len = _vscprintf(formatStr, ap);
        std::vector<char> buf(len + 1, '\0');
        buf.resize(_vsnprintf(buf.data(), len, formatStr, ap) + 1);
#else
        va_list ap2;
        va_copy(ap2, ap);
        size_t len = vsnprintf(nullptr, 0, formatStr, ap2);
        std::vector<char> buf(len + 1, '\0');
        buf.resize(vsnprintf(buf.data(), len + 1, formatStr, ap) + 1);
#endif
        va_end(ap);
        Aws::OStringStream ss;
        ss << &buf.at(0);
        LogStream(logLevel, tag, ss);
    }
    virtual void LogStream(Aws::Utils::Logging::LogLevel logLevel, const char* tag, const Aws::OStringStream &messageStream) override {
        if (logLevel == Aws::Utils::Logging::LogLevel::Error && boost::equals(tag, "AWSXmlClient")) {
            static const char* ignores[] = {
                "Request processing disabled or continuation cancelled by user's continuation handler.",
                "curlCode: 23, Failed writing received data to disk/application",
                nullptr,
            };
            for (int i = 0; ignores[i]; ++i) {
                if (boost::contains(messageStream.str(), ignores[i])) {
                    //std::cout << "ignore: " << messageStream.str() << std::endl;
                    return;
                }
            }
        }
        if (logLevel == Aws::Utils::Logging::LogLevel::Error && boost::equals(tag, "CurlHttpClient")) {
            static const char* ignores[] = {
                "Curl returned error code 23 - Failed writing received data to disk/application",
                nullptr,
            };
            for (int i = 0; ignores[i]; ++i) {
                if (boost::contains(messageStream.str(), ignores[i])) {
                    //std::cout << "ignore: " << messageStream.str() << std::endl;
                    return;
                }
            }
        }
        switch (logLevel) {
            case Aws::Utils::Logging::LogLevel::Trace: Logger::Trace(boost::format("%s|%s : %s") % prefix_ % tag % messageStream.str()); break;
            case Aws::Utils::Logging::LogLevel::Debug: Logger::Debug(boost::format("%s|%s : %s") % prefix_ % tag % messageStream.str()); break;
            case Aws::Utils::Logging::LogLevel::Info: Logger::Info(boost::format("%s|%s : %s") % prefix_ % tag % messageStream.str()); break;
            case Aws::Utils::Logging::LogLevel::Warn: Logger::Warning(boost::format("%s|%s : %s") % prefix_ % tag % messageStream.str()); break;
            case Aws::Utils::Logging::LogLevel::Error: Logger::Error(boost::format("%s|%s : %s") % prefix_ % tag % messageStream.str()); break;
            case Aws::Utils::Logging::LogLevel::Fatal: Logger::Fatal(boost::format("%s|%s : %s") % prefix_ % tag % messageStream.str()); break;
            case Aws::Utils::Logging::LogLevel::Off: default: break;
        }
    }
    virtual void Flush() override {
    }
};

//----------------------------------------------------------------------------
/// @class AWS::Config
//----------------------------------------------------------------------------
class AWS::Config {
    Aws::SDKOptions options_;
    boost::scoped_ptr<Aws::Client::ClientConfiguration> clientConfig_;
public:
    Config() : options_(), clientConfig_() {
    }
    virtual ~Config() {
        Term();
    }
    virtual bool Initialize(const Json& conf) {
        const Json::Node& awsConf = conf["aws"];
        std::string level = awsConf["loglevel"].to<std::string>();
        if (level.empty()) conf["logger"]["level"].to<std::string>();
        if (boost::istarts_with(level, "t")) {
            options_.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Trace;
        } else if (boost::istarts_with(level, "d")) {
            options_.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Debug;
        } else if (boost::istarts_with(level, "w")) {
            options_.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Warn;
        } else if (boost::istarts_with(level, "e")) {
            options_.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Error;
        } else if (boost::istarts_with(level, "f")) {
            options_.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Fatal;
        } else {
            options_.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
        }
        options_.loggingOptions.logger_create_fn = [&]() {
            return Aws::MakeShared<AwsLogAdapter>("AwsLogAdapterAllocationTag", options_.loggingOptions.logLevel, awsConf["logprefix"].to<std::string>("AWSSDK"));
        };
        Aws::InitAPI(options_);
        clientConfig_.reset(new Aws::Client::ClientConfiguration);
        clientConfig_->region = awsConf["region"].to<std::string>("");
        clientConfig_->scheme = boost::iequals(awsConf["scheme"].to<std::string>(""), "http") ? Aws::Http::Scheme::HTTP : Aws::Http::Scheme::HTTPS;
        clientConfig_->useDualStack = awsConf["useDualStack"].to<int>(0) ? true : false;
        clientConfig_->maxConnections = awsConf["maxConnections"].to<unsigned int>(25);
        clientConfig_->requestTimeoutMs = awsConf["requestTimeoutMs"].to<long>(3000);
        clientConfig_->connectTimeoutMs = awsConf["connectTimeoutMs"].to<long>(1000);
        clientConfig_->enableTcpKeepAlive = awsConf["enableTcpKeepAlive"].to<int>(1) ? true : false;
        clientConfig_->tcpKeepAliveIntervalMs = awsConf["tcpKeepAliveIntervalMs"].to<unsigned long>(30000);
        clientConfig_->tcpKeepAliveIntervalMs = awsConf["tcpKeepAliveIntervalMs"].to<unsigned long>(30000);
        clientConfig_->lowSpeedLimit = awsConf["lowSpeedLimit"].to<unsigned long>(1);
        //clientConfig_->retryStrategy = ... // Not Supported
        clientConfig_->endpointOverride = awsConf["endpointOverride"].to<std::string>("");
        clientConfig_->proxyScheme = boost::iequals(awsConf["proxyScheme"].to<std::string>(""), "https") ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
        clientConfig_->proxyHost = awsConf["proxyHost"].to<std::string>("");
        clientConfig_->proxyPort = awsConf["proxyPort"].to<unsigned int>(0);
        clientConfig_->proxyUserName = awsConf["proxyUserName"].to<std::string>("");
        clientConfig_->proxyPassword = awsConf["proxyPassword"].to<std::string>("");
        //clientConfig_->executor = ... // Not Supported
        clientConfig_->verifySSL = awsConf["verifySSL"].to<int>(1) ? true : false;
        clientConfig_->caPath = awsConf["caPath"].to<std::string>("");
        clientConfig_->caFile = awsConf["caFile"].to<std::string>(conf["cainfo"].to<boost::filesystem::path>().string().c_str());
        //clientConfig_->writeRateLimiter = ... // Not Supported
        //clientConfig_->readRateLimiter = ... // Not Supported
        std::string httpLibOverride = awsConf["httpLibOverride"].to<std::string>("");
        if (boost::iequals(httpLibOverride, "DEFAULT_CLIENT")) clientConfig_->httpLibOverride = Aws::Http::TransferLibType::DEFAULT_CLIENT;
        else if (boost::iequals(httpLibOverride, "CURL_CLIENT")) clientConfig_->httpLibOverride = Aws::Http::TransferLibType::CURL_CLIENT;
        else if (boost::iequals(httpLibOverride, "WIN_INET_CLIENT")) clientConfig_->httpLibOverride = Aws::Http::TransferLibType::WIN_INET_CLIENT;
        else if (boost::iequals(httpLibOverride, "WIN_HTTP_CLIENT")) clientConfig_->httpLibOverride = Aws::Http::TransferLibType::WIN_HTTP_CLIENT;
        std::string followRedirects = awsConf["followRedirects"].to<std::string>("");
        if (boost::iequals(followRedirects, "DEFAULT")) clientConfig_->followRedirects = Aws::Client::FollowRedirectsPolicy::DEFAULT;
        else if (boost::iequals(followRedirects, "ALWAYS")) clientConfig_->followRedirects = Aws::Client::FollowRedirectsPolicy::ALWAYS;
        else if (boost::iequals(followRedirects, "NEVER")) clientConfig_->followRedirects = Aws::Client::FollowRedirectsPolicy::NEVER;
        clientConfig_->disableExpectHeader = awsConf["disableExpectHeader"].to<int>(0) ? true : false;
        clientConfig_->enableClockSkewAdjustment = awsConf["enableClockSkewAdjustment"].to<int>(1) ? true : false;
        //clientConfig_->enableHostPrefixInjection = ... // Deprecated
        //clientConfig_->enableEndpointDiscovery = ... // Deprecated
        return true;
    }
    virtual void Term() {
        Aws::ShutdownAPI(options_);
    }
    virtual const Aws::Client::ClientConfiguration& ClientConfig() const {
        return *clientConfig_;
    }
};

AWS::pconf_t AWS::pconf_;

bool AWS::S3Get::IsRunning() const {
    return pimpl_ ? pimpl_->IsRunning() : false;
}

bool AWS::S3Get::Abort() {
    return pimpl_ ? pimpl_->Abort() : false;
}

bool AWS::S3Get::Wait() {
    return pimpl_ ? pimpl_->Wait() : false;
}

std::istream& AWS::S3Get::GetStream() {
    return pimpl_ ? pimpl_->GetStream() : dummyStream;
}

bool AWS::S3Put::IsRunning() const {
    return pimpl_ ? pimpl_->IsRunning() : false;
}

bool AWS::S3Put::Abort() {
    return pimpl_ ? pimpl_->Abort() : false;
}

bool AWS::S3Put::Wait() {
    return pimpl_ ? pimpl_->Wait() : false;
}

bool AWS::S3Client::ListBuckets(std::vector<std::string>& list) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->ListBuckets(list) : false;
}

bool AWS::S3Client::CreateBucket(const std::string& bucketName) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->CreateBucket(bucketName) : false;
}

bool AWS::S3Client::DeleteBucket(const std::string& bucketName) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->DeleteBucket(bucketName) : false;
}

bool AWS::S3Client::List(const std::string& bucketName, const std::string& marker, std::vector<std::string>& list) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->List(bucketName, marker, list) : false;
}

bool AWS::S3Client::Delete(const std::string& bucketName, const std::string& keyName) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->Delete(bucketName, keyName) : false;
}

bool AWS::S3Client::Head(const std::string& bucketName, const std::string& keyName) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->Head(bucketName, keyName) : false;
}

AWS::S3Get AWS::S3Client::GetAsync(const std::string& bucketName, const std::string& keyName, uint64_t offset, size_t bufSiz, const done_t& done, const fail_t& fail) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->GetAsync(bucketName, keyName, offset, bufSiz, done, fail) : S3Get();
}

AWS::S3Put AWS::S3Client::PutAsync(const std::string& bucketName, const std::string& keyName, const std::string& srcFile, const done_t& done, const fail_t& fail) {
    if (!pimpl_ && pconf_) pimpl_.reset(new Impl(pconf_->ClientConfig()));
    return pimpl_ ? pimpl_->PutAsync(bucketName, keyName, srcFile, done, fail) : S3Put();
}

bool AWS::Init(const Json& conf) {
    if (pconf_) return true;
    pconf_.reset(new Config());
    if (pconf_->Initialize(conf)) return true;
    pconf_.reset();
    return false;
}

void AWS::Term() {
    pconf_.reset();
}

bool AWS::Test() {
    Aws::String bucketName = "wakabayashik-test-c7510592-5e75-475c-b841-34cc6f8190e8";
    std::vector<std::string> list;
    S3Client s3client;
    s3client.CreateBucket(bucketName + "x");
    s3client.ListBuckets(list);
    for (std::string& b : list) {
        std::cout << " - bucket: " << b << std::endl;
    }
    s3client.List(bucketName, "test", list);
    for (std::string& o : list) {
        std::cout << " - object: " << o << std::endl;
    }
    if (1) {
        std::string keyName = "test/ResponseStream.h";
        std::string srcFile = "D:\\Projects\\reference\\vcpkg\\installed\\x64-windows-static\\include\\aws\\core\\utils\\stream\\ResponseStream.h";
        S3Put put = s3client.PutAsync(bucketName, keyName, srcFile, [&s3client, bucketName, keyName, srcFile]() {
            std::cout << "Put DONE" << std::endl;
            //S3Put put = s3client.PutAsync(bucketName, keyName + "2", srcFile);
        }, [](const std::string& name, const std::string& message) {
            std::cout << "Put FAIL:" << name << ": " << message << std::endl;
        });
        //put.Abort();
        put.Wait();
    }
    if (1) {
        std::string keyName = "test/ResponseStream.h";
        s3client.Head(bucketName, keyName);
        S3Get get = s3client.GetAsync(bucketName, keyName, 100, 188, []() {
            std::cout << "Get DONE" << std::endl;
        }, [](const std::string& name, const std::string& message) {
            std::cout << "Get FAIL:" << name << ": " << message << std::endl;
        });
        std::stringstream ss;
        std::vector<char> buf;
        for (int i = 0; ; ++i) {
            //if (i >= 3) get.Abort();
            buf.resize(10);
            std::streamsize read = get.GetStream().read(&buf.at(0), buf.size()).gcount();
            if (read <= 0) break;
            std::cout << "read: " << read << std::endl;
            ss << std::string(&buf.at(0), static_cast<size_t>(read));
        }
        std::string str = ss.str();
        std::cout << "total read: " << str.length() << std::endl;
        std::cout << str << std::endl;
        get.Wait();
    }
    s3client.Delete(bucketName, "test/ResponseStream.h");
    s3client.DeleteBucket(bucketName + "x");
    return true;
}

#endif // !defined(USE_AWSSDK)
