#pragma once

#include "json.h"

class AWS {
    class Config;
    using pconf_t = std::unique_ptr<Config>;
    static pconf_t pconf_;
public:
    using done_t = std::function<void()>;
    using fail_t = std::function<void(const std::string& name, const std::string& message)>;
    class S3Get {
        friend class AWS;
        class Impl;
        using pimpl_t = std::shared_ptr<Impl>;
        pimpl_t pimpl_;
    public:
        S3Get(pimpl_t pimpl = pimpl_t()) : pimpl_(pimpl) {}
        virtual ~S3Get() {}
        virtual bool IsRunning() const;
        virtual bool Abort();
        virtual bool Wait();
        virtual std::istream& GetStream();
    };
    class S3Put {
        friend class AWS;
        class Impl;
        using pimpl_t = std::shared_ptr<Impl>;
        pimpl_t pimpl_;
    public:
        S3Put(pimpl_t pimpl = pimpl_t()) : pimpl_(pimpl) {}
        virtual ~S3Put() {}
        virtual bool IsRunning() const;
        virtual bool Abort();
        virtual bool Wait();
    };
    class S3Client {
        class Impl;
        using pimpl_t = std::shared_ptr<Impl>;
        pimpl_t pimpl_;
    public:
        S3Client(pimpl_t pimpl = pimpl_t()) : pimpl_(pimpl) {}
        virtual ~S3Client() {}
        virtual bool ListBuckets(std::vector<std::string>& list);
        virtual bool CreateBucket(const std::string& bucketName);
        virtual bool DeleteBucket(const std::string& bucketName);
        virtual bool List(const std::string& bucketName, const std::string& marker, std::vector<std::string>& list);
        virtual bool Delete(const std::string& bucketName, const std::string& keyName);
        virtual bool Head(const std::string& bucketName, const std::string& keyName);
        virtual S3Get GetAsync(const std::string& bucketName, const std::string& keyName, uint64_t offset = 0, size_t bufSiz = 188 * 50, const done_t& done = nullptr, const fail_t& fail = nullptr);
        virtual S3Put PutAsync(const std::string& bucketName, const std::string& keyName, const std::string& srcFile, const done_t& done = nullptr, const fail_t& fail = nullptr);
    };
public:
    static bool Init(const Json& conf);
    static void Term();
    static bool Test();
};
