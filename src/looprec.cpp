#include "stdafx.h"
#include "looprec.h"
#include "logger.h"
#include "sender.h"
#include "aws.h"

namespace {

static const std::string CONTINUOUS = "=";

//----------------------------------------------------------------------------
///
//----------------------------------------------------------------------------
class Speed {
    double speed_;
    bool is_normal_;
public:
    explicit Speed(const double& speed) : speed_(speed), is_normal_(std::abs(speed - 1.0) < std::numeric_limits<double>::epsilon()) {}
    template <typename Type> Type Mul(const Type& val) const { return is_normal_ ? val : static_cast<Type>(val * speed_); }
    template <typename Type> Type Div(const Type& val) const { return is_normal_ ? val : static_cast<Type>(val / speed_); }
    bool IsNormal() const { return is_normal_; }
    bool IsFast() const { return !is_normal_ && speed_ > 1; }
    bool IsSlow() const { return !is_normal_ && speed_ < 1; }
};
template <typename Type> Type operator*(const Type& val, const Speed& speed) { return speed.Mul(val); }
template <typename Type> Type operator/(const Type& val, const Speed& speed) { return speed.Div(val); }
template <typename Type> Type& operator*=(Type& val, const Speed& speed) { val = speed.Mul(val); return val; }
template <typename Type> Type& operator/=(Type& val, const Speed& speed) { val = speed.Div(val); return val; }

//----------------------------------------------------------------------------
/// @class Segment
//----------------------------------------------------------------------------
class Segment
{
    const std::string log_prefix_;
    boost::filesystem::path dat_path_;
    boost::filesystem::path idx_path_;
    bool continuous_;
    std::atomic<bool> expired_;
    std::atomic<bool> s3pushed_;
    std::string s3bucket_;
    std::string s3folder_;
    boost::filesystem::path s3key_dat_;
    boost::filesystem::path s3key_idx_;
    mutable std::mutex mutex_;
public:
    using ptr_t = std::shared_ptr<Segment>;
    using map_t = std::map<boost::posix_time::ptime, ptr_t>;
    Segment(const std::string& log_prefix, const boost::filesystem::path& path, const std::string& idx_ext
        , const std::string& s3bucket, const std::string& s3folder, const boost::filesystem::path& s3key = "")
        : log_prefix_(log_prefix), dat_path_(path), idx_path_(), continuous_(false), expired_(false)
        , s3pushed_(false), s3bucket_(s3bucket), s3folder_(s3folder), s3key_dat_(s3key), s3key_idx_(), mutex_() {
        if (!path.empty()) {
            (idx_path_ = path).replace_extension(idx_ext);
            continuous_ = boost::algorithm::ends_with(path.stem().string(), CONTINUOUS);
        }
        if (!s3bucket.empty() && !s3key.empty()) {
            s3pushed_ = true;
            (s3key_idx_ = s3key).replace_extension(idx_ext);
            if (path.empty()) continuous_ = boost::algorithm::ends_with(s3key.stem().string(), CONTINUOUS);
        }
    }
    virtual ~Segment() {
        Destroy();
    }
    virtual bool Initialize() {
        return true;
    }
    virtual void Destroy() {
        if (!expired_) {
            return;
        }
        DeleteLocal(true);
        S3Delete(true);
    }
    virtual void SetLocalPath(const boost::filesystem::path& path, const std::string& idx_ext) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (dat_path_.empty()) dat_path_ = path;
        if (idx_path_.empty()) (idx_path_ = path).replace_extension(idx_ext);
    }
    virtual void DeleteLocalIfS3Pushed() {
        if (s3pushed_) DeleteLocal(false);
    }
    virtual void DeleteLocal(bool log) {
        if (!dat_path_.empty()) {
            boost::system::error_code ec;
            if (boost::filesystem::remove(dat_path_, ec)) {
                if (log) Logger::Info(boost::format("%s : remove segment [%s]") % log_prefix_ % dat_path_.filename().string());
                std::lock_guard<std::mutex> lock(mutex_);
                dat_path_.clear();
            } else {
                Logger::Warning(boost::format("%s : failed to remove segment [%s] : %s") % log_prefix_ % dat_path_.filename().string() % ec.to_string());
            }
        }
        if (!idx_path_.empty()) {
            boost::system::error_code ec;
            if (boost::filesystem::remove(idx_path_, ec)) {
                if (log) Logger::Debug(boost::format("%s : remove segment index [%s]") % log_prefix_ % idx_path_.filename().string());
                std::lock_guard<std::mutex> lock(mutex_);
                idx_path_.clear();
            } else {
                Logger::Warning(boost::format("%s : failed to remove segment index [%s] : %s") % log_prefix_ % idx_path_.filename().string() % ec.to_string());
            }
        }
    }
    virtual void S3Delete(bool log) {
        AWS::S3Client s3client;
        if (!s3bucket_.empty() && !s3key_dat_.empty()) {
            if (s3client.Delete(s3bucket_, s3key_dat_.string())) {
                if (log) Logger::Info(boost::format("%s : remove segment [%s]") % log_prefix_ % s3key_dat_.filename().string());
                std::lock_guard<std::mutex> lock(mutex_);
                s3key_dat_.clear();
            }
        }
        if (!s3bucket_.empty() && !s3key_idx_.empty()) {
            if (s3client.Delete(s3bucket_, s3key_idx_.string())) {
                if (log) Logger::Debug(boost::format("%s : remove segment index [%s]") % log_prefix_ % s3key_idx_.filename().string());
                std::lock_guard<std::mutex> lock(mutex_);
                s3key_idx_.clear();
            }
        }
    }
    virtual boost::filesystem::path DatPath() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dat_path_;
    }
    virtual boost::filesystem::path IdxPath() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idx_path_;
    }
    virtual boost::filesystem::path S3KeyDat() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return s3key_dat_;
    }
    virtual boost::filesystem::path S3KeyIdx() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return s3key_idx_;
    }
    virtual bool Continuous() const {
        return continuous_;
    }
    virtual void SetExpired(bool expired) {
        expired_ = expired;
    }
    virtual bool S3Pushed() const {
        return s3pushed_;
    }
    virtual void S3Push() {
        if (s3bucket_.empty() || s3folder_.empty()) return;
        if (s3pushed_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (s3key_dat_.empty()) s3key_dat_ = s3folder_ + "/" + dat_path_.filename().string();
            if (s3key_idx_.empty()) s3key_idx_ = s3folder_ + "/" + idx_path_.filename().string();
        }
        try {
            std::shared_ptr<AWS::S3Client> s3client(new AWS::S3Client);
            AWS::S3Put put_idx = s3client->PutAsync(s3bucket_, s3key_idx_.string(), idx_path_.string());
            AWS::S3Put put_dat = s3client->PutAsync(s3bucket_, s3key_dat_.string(), dat_path_.string());
            bool idx_done = put_idx.Wait();
            bool dat_done = put_dat.Wait();
            if (!idx_done || !dat_done) return;
        } catch (std::exception& ex) {
            Logger::Warning(boost::format("%s : S3 upload failed : %s") % log_prefix_ % ex.what());
            return;
        } catch (...) {
            Logger::Warning(boost::format("%s : S3 upload failed : %s") % log_prefix_ % "unknown exception");
            return;
        }
        s3pushed_ = true;
        DeleteLocalIfS3Pushed();
    }
};

//----------------------------------------------------------------------------
/// @class SegmentWriter
//----------------------------------------------------------------------------
class SegmentWriter
{
    static constexpr size_t FILE_BUFFER_SIZE = 256 * 1024;
#if defined(WIN32) || defined(WIN64)
    struct FileWriter {
        HANDLE handle_;
        size_t expand_;
        size_t pos_;
        size_t alloc_;
        Event::buf_t buf_;
        std::string errmsg_;
    public:
        FileWriter(size_t expand) : handle_(INVALID_HANDLE_VALUE), expand_(expand), pos_(0), alloc_(0), buf_(), errmsg_() {
            buf_.reserve(FILE_BUFFER_SIZE);
        }
        ~FileWriter() {
            close();
        }
        void open(const std::string& path) {
            close();
            pos_ = 0;
            alloc_ = 0;
            buf_.resize(0);
            errmsg_.clear();
            handle_ = ::CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        }
        void close() {
            if (!is_open()) return;
            flush();
            if (errmsg_.empty() && !buf_.empty()) errmsg_ = "failed to write last data";
            if (!::SetEndOfFile(handle_) && errmsg_.empty()) strerr(GetLastError(), errmsg_);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        size_t write(const char* buf, size_t siz) {
            size_t siz0 = siz;
            while (siz) {
                size_t bufpos = buf_.size();
                if (bufpos >= FILE_BUFFER_SIZE && !write_buf()) break;
                size_t written = std::min<size_t>(FILE_BUFFER_SIZE - bufpos, siz);
                buf_.resize(bufpos + written);
                ::memcpy(&buf_.at(bufpos), buf, written);
                buf += written;
                siz -= written;
            }
            if (!siz) errmsg_.clear();
            if (siz && errmsg_.empty()) errmsg_ = "partial write";
            return siz0 - siz;
        }
        void flush(bool deep = false) {
            errmsg_.clear();
            if (!write_buf() || !buf_.empty()) return;
            if (deep && !::FlushFileBuffers(handle_)) strerr(GetLastError(), errmsg_);
        }
        size_t pos() const {
            return pos_ + buf_.size();
        }
        bool is_open() const {
            return handle_ != INVALID_HANDLE_VALUE;
        }
        const std::string& errmsg() const {
            return errmsg_;
        }
    protected:
        bool write_buf() {
            if (buf_.empty()) return true;
            if (expand_ && pos_ + buf_.size() > alloc_) {
                alloc((pos_ + buf_.size()) / expand_ * expand_ + expand_);
            }
            const char* buf = &buf_.at(0);
            size_t siz = buf_.size();
            size_t siz0 = siz;
            while (siz) {
                DWORD written = 0;
                if (!::WriteFile(handle_, buf, static_cast<DWORD>(std::min<size_t>(siz, MAXDWORD)), &written, NULL)) {
                    strerr(GetLastError(), errmsg_);
                    break;
                }
                if (written == 0) break;
                buf += written;
                siz -= written;
            }
            pos_ += siz0 - siz;
            if (!siz) {
                buf_.resize(0);
                return true;
            }
            if (siz < siz0) {
                ::memmove(&buf_.at(0), buf, siz);
                buf_.resize(siz);
                return true;
            }
            if (errmsg_.empty()) errmsg_ = "failed to write";
            return false;
        }
        void alloc(size_t siz) {
            if (alloc_ >= siz) return;
            FILE_ALLOCATION_INFO fai = {0};
            fai.AllocationSize.QuadPart = siz;
            if (::SetFileInformationByHandle(handle_, FileAllocationInfo, &fai, sizeof(fai))) alloc_ = siz;
        }
        static void strerr(DWORD code, std::string& errmsg) {
            DWORD lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
	        DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	        LPVOID buf = NULL;
	        if (::FormatMessageA(flags, NULL, code, lang, reinterpret_cast<LPSTR>(&buf), 0, NULL)) errmsg = static_cast<LPCSTR>(buf);
	        if (buf) ::LocalFree(buf);
            boost::algorithm::trim_right(errmsg); // remove crlf
            if (errmsg.empty()) errmsg = (boost::format("error(%u)") % code).str();
        }
    };
#else
    struct FileWriter {
        int fd_;
        size_t expand_;
        size_t pos_;
        size_t alloc_;
        Event::buf_t buf_;
        std::string errmsg_;
    public:
        FileWriter(size_t expand) : fd_(-1), expand_(expand), pos_(0), alloc_(0), buf_(), errmsg_() {
            buf_.reserve(FILE_BUFFER_SIZE);
        }
        ~FileWriter() {
            close();
        }
        void open(const std::string& path) {
            close();
            pos_ = 0;
            alloc_ = 0;
            buf_.resize(0);
            errmsg_.clear();
            fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        }
        void close() {
            if (!is_open()) return;
            flush();
            if (errmsg_.empty() && !buf_.empty()) errmsg_ = "failed to write last data";
            if (::ftruncate(fd_, pos_) == -1 && errmsg_.empty()) strerr(errno, errmsg_);
            ::close(fd_);
            fd_ = -1;
        }
        size_t write(const char* buf, size_t siz) {
            size_t siz0 = siz;
            while (siz) {
                size_t bufpos = buf_.size();
                if (bufpos >= FILE_BUFFER_SIZE && !write_buf()) break;
                size_t written = std::min<size_t>(FILE_BUFFER_SIZE - bufpos, siz);
                buf_.resize(bufpos + written);
                ::memcpy(&buf_.at(bufpos), buf, written);
                buf += written;
                siz -= written;
            }
            if (!siz) errmsg_.clear();
            if (siz && errmsg_.empty()) errmsg_ = "partial write";
            return siz0 - siz;
        }
        void flush(bool deep = false) {
            errmsg_.clear();
            if (!write_buf() || !buf_.empty()) return;
            if (deep && ::fsync(fd_) == -1) strerr(errno, errmsg_);
        }
        size_t pos() const {
            return pos_ + buf_.size();
        }
        bool is_open() const {
            return fd_ != -1;
        }
        const std::string& errmsg() const {
            return errmsg_;
        }
    protected:
        bool write_buf() {
            if (buf_.empty()) return true;
            if (expand_ && pos_ + buf_.size() > alloc_) {
                alloc((pos_ + buf_.size()) / expand_ * expand_ + expand_);
            }
            const char* buf = &buf_.at(0);
            size_t siz = buf_.size();
            size_t siz0 = siz;
            while (siz) {
                ssize_t written = ::write(fd_, buf, siz);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    strerr(errno, errmsg_);
                    break;
                }
                if (written == 0) break;
                buf += written;
                siz -= written;
            }
            pos_ += siz0 - siz;
            if (!siz) {
                buf_.resize(0);
                return true;
            }
            if (siz < siz0) {
                ::memmove(&buf_.at(0), buf, siz);
                buf_.resize(siz);
                return true;
            }
            if (errmsg_.empty()) errmsg_ = "failed to write";
            return false;
        }
        void alloc(size_t siz) {
            if (alloc_ >= siz) return;
            if (::fallocate(fd_, FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(siz)) == 0) alloc_ = siz;
        }
        static void strerr(int err, std::string& errmsg) {
            char errbuf[256] = {'\0'};
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
            errmsg = ::strerror_r(err, errbuf, sizeof(errbuf));
#else
            errmsg = ::strerror_r(err, errbuf, sizeof(errbuf)) == 0 ? errbuf : "";
#endif
            if (errmsg.empty()) errmsg = (boost::format("error(%d)") % err).str();
        }
    };
#endif
    const std::string log_prefix_;
    AntiRepeatLogger logger_;
    const Segment::ptr_t segment_;
    FileWriter dat_file_;
    FileWriter idx_file_;
    const std::chrono::milliseconds idx_interval_;
    const std::function<std::streamoff(std::streamoff)> idx_endian_;
    std::chrono::steady_clock::time_point idx_time_;
    std::streamoff pos_;
public:
    using ptr_t = std::shared_ptr<SegmentWriter>;
    SegmentWriter(const std::string& log_prefix, Segment::ptr_t segment, size_t segment_expand, const std::chrono::milliseconds& idx_interval
        , std::function<std::streamoff(std::streamoff)> idx_endian, const std::chrono::steady_clock::time_point& idx_time)
        : log_prefix_(log_prefix), logger_(log_prefix) , segment_(segment), dat_file_(segment_expand), idx_file_(1024)
        , idx_interval_(idx_interval), idx_endian_(idx_endian), idx_time_(idx_time), pos_(0) {
    }
    virtual ~SegmentWriter() {
        Destroy();
    }
    virtual bool Initialize() {
        if (!segment_) {
            return false;
        }
        dat_file_.open(segment_->DatPath().string());
        if (dat_file_.is_open()) {
            logger_.Info(boost::format("%s : create segment data [%s]") % log_prefix_ % segment_->DatPath().filename().string());
        } else {
            logger_.Warning(boost::format("%s : failed to create segment data [%s]") % log_prefix_ % segment_->DatPath().filename().string());
        }
        idx_file_.open(segment_->IdxPath().string());
        if (idx_file_.is_open()) {
            logger_.Debug(boost::format("%s : create segment index [%s]") % log_prefix_ % segment_->IdxPath().filename().string());
        } else {
            logger_.Warning(boost::format("%s : failed to create segment index [%s]") % log_prefix_ % segment_->IdxPath().filename().string());
        }
        return WriteIndex();
    }
    virtual void Destroy() {
        Close();
    }
    virtual bool Write(const std::chrono::steady_clock::time_point& tick, const Event::buf_t& buf) {
        if (!dat_file_.is_open()) return false;
        if (!buf.empty()) {
            dat_file_.write(&buf.at(0), buf.size());
            if (!dat_file_.errmsg().empty()) {
                logger_.Warning(boost::format("%s : failed to write segment data [%s] : %s") % log_prefix_ % segment_->DatPath().filename().string() % dat_file_.errmsg());
                return false;
            }
        }
        bool flush = false;
        while (tick >= idx_time_) {
            if (!WriteIndex()) return false;
            flush = true;
        }
        if (!flush || Flush()) logger_.Clear();
        return true;
    }
    virtual void Close() {
        if (dat_file_.is_open()) {
            dat_file_.close();
            if (!dat_file_.errmsg().empty()) {
                logger_.Warning(boost::format("%s : failed to close segment data [%s] : %s") % log_prefix_ % segment_->DatPath().filename().string() % dat_file_.errmsg());
            }
        }
        if (idx_file_.is_open()) {
            idx_file_.close();
            if (!idx_file_.errmsg().empty()) {
                logger_.Warning(boost::format("%s : failed to close segment index [%s] : %s") % log_prefix_ % segment_->IdxPath().filename().string() % idx_file_.errmsg());
            }
        }
    }
    virtual bool Flush() {
        bool done = true;
        if (dat_file_.is_open()) {
            dat_file_.flush();
            if (!dat_file_.errmsg().empty()) {
                done = false;
                logger_.Warning(boost::format("%s : failed to flush segment data [%s] : %s") % log_prefix_ % segment_->DatPath().filename().string() % dat_file_.errmsg());
            }
        }
        if (idx_file_.is_open()) {
            idx_file_.flush();
            if (!idx_file_.errmsg().empty()) {
                done = false;
                logger_.Warning(boost::format("%s : failed to flush segment index [%s] : %s") % log_prefix_ % segment_->IdxPath().filename().string() % idx_file_.errmsg());
            }
        }
        return done;
    }
    Segment::ptr_t GetSegment() const {
        return segment_;
    }
protected:
    virtual bool WriteIndex() {
        if (!dat_file_.is_open() || !idx_file_.is_open()) return false;
        pos_ = std::max<std::streamoff>(pos_, dat_file_.pos());
        std::streamoff pos = idx_endian_(pos_);
        idx_file_.write(reinterpret_cast<const char*>(&pos), sizeof(std::streamoff));
        if (!idx_file_.errmsg().empty()) {
            logger_.Warning(boost::format("%s : failed to write segment index [%s] : %s") % log_prefix_ % segment_->IdxPath().filename().string() % idx_file_.errmsg());
            return false;
        }
        idx_time_ += idx_interval_;
        return true;
    }
};

//----------------------------------------------------------------------------
/// @class SegmentReader
//----------------------------------------------------------------------------
class SegmentReader
{
    const std::string log_prefix_;
    const Segment::ptr_t segment_;
    const Speed speed_;
    std::ifstream dat_file_;
    std::ifstream idx_file_;
    const std::chrono::milliseconds idx_interval_;
    const std::function<std::streamoff(std::streamoff)> idx_endian_;
    const std::chrono::steady_clock::time_point base_time_;
    std::streamoff pos_;
    std::streamoff next_;
    int64_t read_;
    int64_t pos_ns_;
    bool reached_idx_end_;
    AWS::S3Get s3get_dat_;
    AWS::S3Get s3get_idx_;
    std::istream* dat_stream_;
    std::istream* idx_stream_;
    bool burst_;
    bool burst_logged_;
    int64_t max_send_delay_;
    std::chrono::steady_clock::time_point suppress_log_send_delay_;
public:
    using ptr_t = std::unique_ptr<SegmentReader>;
    SegmentReader(const std::string& log_prefix, Segment::ptr_t segment, const Speed& speed, const std::chrono::milliseconds& idx_interval
        , std::function<std::streamoff(std::streamoff)> idx_endian, const std::chrono::steady_clock::time_point& base_time)
        : log_prefix_(log_prefix), segment_(segment), speed_(speed), dat_file_(), idx_file_()
        , idx_interval_(idx_interval), idx_endian_(idx_endian), base_time_(base_time), pos_(0), next_(0), read_(0), pos_ns_(0), reached_idx_end_(false)
        , s3get_dat_(), s3get_idx_(), dat_stream_(nullptr), idx_stream_(nullptr), burst_(false), burst_logged_(false)
        , max_send_delay_(0), suppress_log_send_delay_(base_time)
    {
    }
    virtual ~SegmentReader() {
        Destroy();
    }
    virtual bool Initialize(int64_t offset_ms, const std::string& s3bucket, size_t s3bufsiz) {
        if (!segment_) {
            return false;
        }
        if (segment_->S3Pushed() && !s3bucket.empty()) {
            AWS::S3Client s3client;
            if (!s3client.Head(s3bucket, segment_->S3KeyIdx().string())) {
                Logger::Warning(boost::format("%s : failed to open segment index [%s]") % log_prefix_ % segment_->S3KeyIdx().filename().string());
                return false;
            }
            if (!s3client.Head(s3bucket, segment_->S3KeyDat().string())) {
                Logger::Warning(boost::format("%s : failed to open segment [%s]") % log_prefix_ % segment_->S3KeyDat().filename().string());
                return false;
            }
            int64_t offset = offset_ms / idx_interval_.count();
            s3get_idx_ = s3client.GetAsync(s3bucket, segment_->S3KeyIdx().string(), offset * sizeof(std::streamoff));
            if (s3get_idx_.GetStream().read(reinterpret_cast<char*>(&pos_), sizeof(std::streamoff)).gcount() < static_cast<std::streamsize>(sizeof(std::streamoff))) {
                Logger::Trace(boost::format("%s : failed to read segment index (%s[ms]) [%s]") % log_prefix_ % offset_ms % segment_->S3KeyIdx().filename().string());
                reached_idx_end_ = true;
                return false;
            }
            pos_ = idx_endian_(pos_);
            if (s3get_idx_.GetStream().read(reinterpret_cast<char*>(&next_), sizeof(std::streamoff)).gcount() < static_cast<std::streamsize>(sizeof(std::streamoff))) {
                Logger::Trace(boost::format("%s : failed to read segment index (%s[ms] next) [%s]") % log_prefix_ % offset_ms % segment_->S3KeyIdx().filename().string());
                reached_idx_end_ = true;
                return false;
            }
            next_ = idx_endian_(next_);
            s3get_dat_ = AWS::S3Client().GetAsync(s3bucket, segment_->S3KeyDat().string(), pos_, s3bufsiz); // another S3Client for segment data
            Logger::Debug(boost::format("%s : open segment [%s]") % log_prefix_ % segment_->S3KeyDat().filename().string());
            read_ = 0;
            pos_ns_ = offset * 1000ll * 1000 * idx_interval_.count(); // millisec to nanosec
            dat_stream_ = &s3get_dat_.GetStream();
            idx_stream_ = &s3get_idx_.GetStream();
        } else {
            idx_file_.open(segment_->IdxPath().string(), std::ios::in | std::ios::binary);
            if (!idx_file_.is_open()) {
                Logger::Warning(boost::format("%s : failed to open segment index [%s]") % log_prefix_ % segment_->IdxPath().filename().string());
                return false;
            }
            int64_t offset = offset_ms / idx_interval_.count();
            idx_file_.seekg(offset * sizeof(std::streamoff));
            if (idx_file_.read(reinterpret_cast<char*>(&pos_), sizeof(std::streamoff)).gcount() < static_cast<std::streamsize>(sizeof(std::streamoff))) {
                Logger::Trace(boost::format("%s : failed to read segment index (%s[ms]) [%s]") % log_prefix_ % offset_ms % segment_->IdxPath().filename().string());
                reached_idx_end_ = true;
                return false;
            }
            pos_ = idx_endian_(pos_);
            if (idx_file_.read(reinterpret_cast<char*>(&next_), sizeof(std::streamoff)).gcount() < static_cast<std::streamsize>(sizeof(std::streamoff))) {
                Logger::Trace(boost::format("%s : failed to read segment index (%s[ms] next) [%s]") % log_prefix_ % offset_ms % segment_->IdxPath().filename().string());
                reached_idx_end_ = true;
                return false;
            }
            next_ = idx_endian_(next_);
            dat_file_.open(segment_->DatPath().string(), std::ios::in | std::ios::binary);
            if (!dat_file_.is_open()) {
                Logger::Warning(boost::format("%s : failed to open segment [%s]") % log_prefix_ % segment_->DatPath().filename().string());
                return false;
            }
            Logger::Debug(boost::format("%s : open segment [%s]") % log_prefix_ % segment_->DatPath().filename().string());
            dat_file_.seekg(pos_);
            read_ = 0;
            pos_ns_ = offset * 1000ll * 1000 * idx_interval_.count(); // millisec to nanosec
            dat_stream_ = &dat_file_;
            idx_stream_ = &idx_file_;
        }
        return true;
    }
    virtual void Destroy() {
        std::string filename;
        if (dat_stream_ == &dat_file_) {
            filename = segment_->DatPath().filename().string();
        } else if (dat_stream_ == &s3get_dat_.GetStream()) {
            filename = segment_->S3KeyDat().filename().string();
        }
        dat_stream_ = nullptr;
        idx_stream_ = nullptr;
        dat_file_.close();
        idx_file_.close();
        s3get_dat_.Abort();
        s3get_idx_.Abort();
        s3get_dat_ = AWS::S3Get();
        s3get_idx_ = AWS::S3Get();
        if (!filename.empty() && segment_) {
            Logger::Debug(boost::format("%s : close segment [%s]") % log_prefix_ % filename);
        }
    }
    virtual bool Read(const std::chrono::steady_clock::time_point& tick, Event::buf_t& buf, bool latest_segment) {
        if (!dat_stream_ || !idx_stream_) return false;
        int64_t elapsed_ns = (tick - base_time_).count();
        if (elapsed_ns < 0) {
            buf.resize(0);
            std::this_thread::sleep_for(std::chrono::nanoseconds(-elapsed_ns));
            return true;
        }
        if (latest_segment && reached_idx_end_) {
            ReadIndex(true);
            return false;
        }
        if (next_ > pos_) {
            buf.resize(std::min<size_t>(buf.size(), next_ - pos_ - read_));
        }
        if (buf.empty()) {
            return false;
        }
        std::chrono::steady_clock::time_point s0 = std::chrono::steady_clock::now();
        buf.resize(static_cast<size_t>(dat_stream_->read(&buf.at(0), buf.size()).gcount()));
        int64_t e0 = (std::chrono::steady_clock::now() - s0).count();
        if (e0 >= 1000ll * 1000 * 30) {
            Logger::Debug(boost::format("%s : it took %lf[ms] to read the data") % log_prefix_ % (static_cast<double>(e0) / 1000.0 / 1000.0));
        }
        if (buf.empty()) {
            return false;
        }
        if (!reached_idx_end_ && next_ > pos_) {
            int64_t reference_ns = (pos_ns_ + 1000ll * 1000 * idx_interval_.count() * read_ / (next_ - pos_)) / speed_;
            if (reference_ns > elapsed_ns) {
                if (reference_ns - elapsed_ns > 1000ll * 1000 * 300) {
                    Logger::Warning(boost::format("%s : too long wait : %lld[ms]") % log_prefix_ % ((reference_ns - elapsed_ns) / 1000 / 1000));
                }
                std::this_thread::sleep_for(std::chrono::nanoseconds(reference_ns - elapsed_ns));
                if (burst_) {
                    burst_ = false;
                    if (burst_logged_) {
                        Logger::Trace(boost::format("%s : burst send done") % log_prefix_);
                        burst_logged_ = false;
                    }
                }
            } else if (elapsed_ns - reference_ns > 1000ll * 1000 * 300) {
                if (burst_) {
                    if (!burst_logged_) {
                        Logger::Trace(boost::format("%s : burst send : %lld[ms]") % log_prefix_ % ((elapsed_ns - reference_ns) / 1000 / 1000));
                        burst_logged_ = true;
                    }
                } else {
                    max_send_delay_ = std::max<int64_t>(max_send_delay_, elapsed_ns - reference_ns);
                    if (tick > suppress_log_send_delay_) {
                        Logger::Warning(boost::format("%s : send delay : %lld[ms]") % log_prefix_ % (max_send_delay_ / 1000 / 1000));
                        max_send_delay_ = 0;
                        suppress_log_send_delay_ = tick + std::chrono::seconds(3);
                    }
                }
            }
        }
        read_ += buf.size();
        ReadIndex(latest_segment);
        return true;
    }
    const std::chrono::steady_clock::time_point& BaseTime() const {
        return base_time_;
    }
    bool ReachedIdxEnd() const {
        return reached_idx_end_;
    }
    int64_t PosNs() const {
        return pos_ns_;
    }
    void SetBurst() {
        burst_ = true;
    }
    bool IsBurst() const {
        return burst_;
    }
    Segment::ptr_t GetSegment() const {
        return segment_;
    }
 protected:
     void ReadIndex(bool latest_segment) {
        while (pos_ + read_ >= next_) {
            std::streamoff idx_pos = static_cast<std::streamoff>(idx_stream_->tellg());
            std::streamoff next = 0;
            std::chrono::steady_clock::time_point s1 = std::chrono::steady_clock::now();
            std::streamsize idx_read = idx_stream_->read(reinterpret_cast<char*>(&next), sizeof(std::streamoff)).gcount();
            int64_t e1 = (std::chrono::steady_clock::now() - s1).count();
            if (e1 >= 1000ll * 1000 * 30) {
                Logger::Debug(boost::format("%s : it took %lf[ms] to read the index") % log_prefix_ % (static_cast<double>(e1) / 1000.0 / 1000.0));
            }
            if (idx_read < static_cast<std::streamsize>(sizeof(std::streamoff))) {
                if (idx_pos >= 0) idx_stream_->seekg(idx_pos);
                reached_idx_end_ = true;
                break;
            }
            reached_idx_end_ = false;
            if (next_ > pos_) {
                read_ -= next_ - pos_;
            }
            pos_ns_ += 1000ll * 1000 * idx_interval_.count();
            pos_ = next_;
            next_ = idx_endian_(next);
        }
        if (latest_segment) {
            // clear flags to continuie reading
            dat_file_.clear();
            idx_file_.clear();
        }
     }
};

} // namespace {

//----------------------------------------------------------------------------
/// @class LoopRec::Impl
//----------------------------------------------------------------------------
class LoopRec::Impl
{
    class SenderRunner;
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::Queue
    //----------------------------------------------------------------------------
    class Queue {
    public:
        class Task {
        public:
            Task() {}
            virtual ~Task() {}
            virtual void Run() = 0;
        };
        using task_t = std::shared_ptr<Task>;
        using queue_t = std::deque<task_t>;
    private:
        std::string log_prefix_;
        std::atomic<bool> running_;
        queue_t queue_;
        std::thread thread_;
        mutable std::mutex mutex_;
        std::condition_variable cond_;
    public:
        Queue(const std::string& log_prefix) : log_prefix_(log_prefix), running_(false), queue_(), thread_(), mutex_(), cond_() {
        }
        virtual ~Queue() {
            Destroy();
        }
        virtual bool Initialize() {
            running_ = true;
            thread_ = std::thread(&Queue::Thread, this);
            return true;
        }
        virtual void Destroy() {
            running_ = false;
            cond_.notify_one();
            if (thread_.joinable()) thread_.join();
        }
        virtual bool Push(task_t task) {
            return PushIf(0, task);
        }
        virtual bool PushIf(size_t limit, task_t task) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return false;
            if (limit > 0 && queue_.size() >= limit) return false;
            queue_.push_back(task);
            cond_.notify_one();
            return true;
        }
        virtual void Clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        template<typename Type> std::shared_ptr<const Type> GetFirstOf() const {
            std::lock_guard<std::mutex> lock(mutex_);
            for (queue_t::const_iterator it = queue_.begin(); it != queue_.end(); ++it) {
                task_t ptr = *it;
                if (!ptr || typeid(*ptr) != typeid(Type)) continue;
                return std::dynamic_pointer_cast<const Type>(ptr);
            }
            return std::shared_ptr<const Type>();
        }
    protected:
        virtual task_t Pop() {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this]() { return !queue_.empty() || !running_; });
            if (queue_.empty()) return nullptr;
            task_t task = queue_.front();
            queue_.pop_front();
            return task;
        }
        virtual void Thread() {
            std::string last_err;
            for (;;) {
                std::string err;
                try {
                     task_t task = Pop();
                     if (task) {
                         task->Run();
                     } else {
                         std::lock_guard<std::mutex> lk(mutex_);
                         if (queue_.empty() && !running_) break;
                     }
                } catch (std::exception& ex) {
                    err = ex.what();
                } catch (...) {
                    err = "unknown exception";
                }
                if (err.empty() || err == last_err) continue;
                last_err = err;
                Logger::Warning(boost::format("%s : an unexpected exception occurred : %s") % log_prefix_ % err);
            }
        }
    };
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::WriteTask
    //----------------------------------------------------------------------------
    class WriteTask : public Queue::Task {
        LoopRec::Impl* pimpl_;
        Event::buf_t buf_;
        std::chrono::steady_clock::time_point tick_;
    public:
        WriteTask(LoopRec::Impl* pimpl, const Event::buf_t& buf, const std::chrono::steady_clock::time_point& tick)
            : pimpl_(pimpl), buf_(buf), tick_(tick) {
        }
        virtual void Run() override {
            pimpl_->Write(buf_, tick_);
        }
        const std::chrono::steady_clock::time_point& Tick() const {
            return tick_;
        }
    };
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::CloseWriterTask
    //----------------------------------------------------------------------------
    class CloseWriterTask : public Queue::Task {
        LoopRec::Impl* pimpl_;
        std::chrono::steady_clock::time_point tick_;
    public:
        CloseWriterTask(LoopRec::Impl* pimpl, const std::chrono::steady_clock::time_point& tick)
            : pimpl_(pimpl), tick_(tick) {
        }
        virtual void Run() override {
            pimpl_->CloseWriter(tick_);
        }
    };
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::SegmentUploadTask
    //----------------------------------------------------------------------------
    class SegmentUploadTask : public Queue::Task {
        Segment::ptr_t segment_;
    public:
        SegmentUploadTask(const Segment::ptr_t segment) : segment_(segment) {
        }
        virtual void Run() override {
            if (!segment_) return;
            segment_->S3Push();
        }
    };
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::RemoveExpiredSegmentsTask
    //----------------------------------------------------------------------------
    class RemoveExpiredSegmentsTask : public Queue::Task {
        LoopRec::Impl* pimpl_;
        boost::posix_time::ptime utc_;
    public:
        RemoveExpiredSegmentsTask(LoopRec::Impl* pimpl, boost::posix_time::ptime utc)
            : pimpl_(pimpl), utc_(utc) {
        }
        virtual void Run() override {
            pimpl_->RemoveExpiredSegments(utc_);
        }
    };
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::RemoveSenderRunnerTask
    //----------------------------------------------------------------------------
    class RemoveSenderRunnerTask : public Queue::Task {
        LoopRec::Impl* pimpl_;
        std::shared_ptr<SenderRunner> sender_runner_;
    public:
        RemoveSenderRunnerTask(LoopRec::Impl* pimpl, std::shared_ptr<SenderRunner> sender_runner)
            : pimpl_(pimpl), sender_runner_(sender_runner) {
        }
        virtual void Run() override {
            if (!sender_runner_) return;
            pimpl_->RemoveSender(sender_runner_);
        }
    };
    //----------------------------------------------------------------------------
    /// @class LoopRec::Impl::SenderRunner
    //----------------------------------------------------------------------------
    class SenderRunner : public std::enable_shared_from_this<SenderRunner> {
        LoopRec::Impl* pimpl_;
        Sender::ptr_t sender_;
        StreamOption option_;
        std::thread thread_;
        std::atomic<bool> destroyed_;
        using weak_t = std::weak_ptr<SenderRunner>;
    public:
        using ptr_t = std::shared_ptr<SenderRunner>;
        using vector_t = std::vector<ptr_t>;
        SenderRunner(LoopRec::Impl* pimpl, int sfd, const SendOption& sendOption, const StreamOption& streamOption)
            : pimpl_(pimpl), sender_(Sender::Create(sfd, sendOption)), option_(streamOption), thread_(), destroyed_(false) {
            option_.Synonym("speed", "x");
        }
        SenderRunner(const SenderRunner&) = delete;
        SenderRunner& operator=(const SenderRunner&) = delete;
        virtual ~SenderRunner() {
            Destroy();
        }
        virtual bool Initialize() {
            weak_t weak = shared_from_this();
            thread_ = std::thread(&SenderRunner::Thread, this, weak, sender_);
            return true;
        }
        virtual void Destroy() {
            destroyed_ = true;
            if (sender_) {
                sender_->Destroy();
                sender_.reset();
            }
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    protected:
        virtual void Thread(weak_t weak, Sender::ptr_t sender) {
            try {
                pimpl_->Send(sender, option_);
            } catch (std::exception& ex) {
                Logger::Warning(boost::format("%s : sender thread failed : %s") % pimpl_->log_prefix_ % ex.what());
            } catch (...) {
                Logger::Warning(boost::format("%s : sender thread failed : %s") % pimpl_->log_prefix_ % "unknown exception");
            }
            ptr_t sender_runner = weak.lock();
            if (destroyed_) return;
            pimpl_->queue2_.Push(Queue::task_t(new RemoveSenderRunnerTask(pimpl_, sender_runner)));
        }
    };
    LoopRec* owner_;
    const Json conf_;
    const std::string app_;
    const std::string name_;
    const std::string log_prefix_;
    Segment::map_t segments_;
    SegmentWriter::ptr_t writer_;
    boost::filesystem::path dir_;
    std::string s3bucket_;
    std::string s3folder_;
    size_t s3bufsiz_;
    std::string dat_ext_;
    std::string idx_ext_;
    size_t segment_expand_;
    std::chrono::seconds segment_duration_;
    std::chrono::seconds total_duration_;
    std::chrono::milliseconds idx_interval_;
    std::function<std::streamoff(std::streamoff)> idx_endian_;
    uint32_t prefetch_ms_;
    std::chrono::steady_clock::time_point segment_time_;
    mutable std::mutex mutex_;
    SenderRunner::vector_t sender_runners_;
    int queue_limit_;
    Queue queue_;
    Queue queue2_;
public:
    Impl(LoopRec* owner, const Json& conf, const std::string& app, const std::string& name)
        : owner_(owner), conf_(conf), app_(app), name_(name), log_prefix_((boost::format("<%s> loopRec [ %s ]") % app % name).str())
        , segments_(), writer_(), dir_(), s3bucket_(), s3folder_(), s3bufsiz_(0), dat_ext_(".dat"), idx_ext_(".idx")
        , segment_duration_(600), total_duration_(3600), idx_interval_(100), idx_endian_(), prefetch_ms_(0), segment_time_()
        , mutex_(), sender_runners_(), queue_limit_(0), queue_(log_prefix_ + " (queue)"), queue2_(log_prefix_ + " (queue2)")
        , OnReceive(), OnDisconnected() {
    }
    virtual ~Impl() {
        Destroy();
    }
    virtual bool Initialize() {
        queue2_.Initialize();
        try {
            dir_ = conf_["dir"].to<boost::filesystem::path>();
            s3bucket_ = boost::trim_copy_if(conf_["s3"]["bucket"].to<std::string>(), boost::is_any_of(" \t\v"));
            s3folder_ = boost::trim_copy_if(conf_["s3"]["folder"].to<std::string>(), boost::is_any_of(" \t\v./\\"));
            s3bufsiz_ = conf_["s3"]["bufsiz"].to<size_t>(188 * 100);
            dat_ext_ = "." + boost::trim_left_copy_if(conf_["data_extension"].to<std::string>("dat"), boost::is_any_of("."));
            idx_ext_ = "." + boost::trim_left_copy_if(conf_["index_extension"].to<std::string>("idx"), boost::is_any_of("."));
            if (dat_ext_ == idx_ext_) idx_ext_ += "_idx";
            segment_expand_ = std::min<size_t>(conf_["segment_expand"].to<size_t>(1024 * 1024 * 100), 1024 * 1024 * 1000);
            uint32_t segdur = std::max<uint32_t>(conf_["segment_duration"].to<uint32_t>(600), 10);
            segment_duration_ = std::chrono::seconds(segdur);
            total_duration_ = std::chrono::seconds(std::max<uint32_t>(conf_["total_duration"].to<uint32_t>(3600), segdur));
            idx_interval_ = std::chrono::milliseconds(std::max<uint32_t>(conf_["index_interval"].to<uint32_t>(100), 1));
            std::string idx_endian = conf_["index_endian"].to<std::string>("");
            if (boost::iequals(idx_endian, "big")) {
                idx_endian_ = boost::endian::big_to_native<std::streamoff>;
            } else if (boost::iequals(idx_endian, "little")) {
                idx_endian_ = boost::endian::little_to_native<std::streamoff>;
            } else {
                idx_endian_ = [](std::streamoff v) { return v; };
            }
            prefetch_ms_ = conf_["prefetch"].to<uint32_t>(1000);
            if (!s3bucket_.empty()) {
                if (s3folder_.empty()) {
                    char hostname[256] = {'\0'};
                    s3folder_ = (gethostname(hostname, 255) < 0) ? name_ : (boost::format("%s/%s") % hostname % name_).str();
                }
                AWS::S3Client s3client;
                std::vector<std::string> list;
                if (s3client.List(s3bucket_, s3folder_, list)) {
                    for (std::vector<std::string>::const_iterator it = list.begin(); it != list.end(); ++it) {
                        const boost::filesystem::path path(*it);
                        std::string ext = path.extension().string();
                        if (ext != dat_ext_) continue;
                        try {
                            std::string fname = path.filename().string();
                            boost::posix_time::ptime utc = boost::posix_time::from_iso_string(fname);
                            if (utc.is_special()) continue;
                            Segment::ptr_t segment(new Segment(log_prefix_, "", idx_ext_, s3bucket_, s3folder_, path));
                            if (segment->Initialize()) {
                                segments_[utc] = segment;
                            }
                        } catch (boost::bad_lexical_cast&) {
                            continue;
                        }
                    }
                }
            }
            if (dir_.empty()) dir_ = "./" + name_;
            boost::filesystem::create_directories(dir_);
            for (boost::filesystem::directory_iterator it(dir_), end; it != end; ++it) {
                const boost::filesystem::path path(*it);
                std::string ext = path.extension().string();
                if (ext != dat_ext_) continue;
                try {
                    std::string fname = path.filename().string();
                    boost::posix_time::ptime utc = boost::posix_time::from_iso_string(fname);
                    if (utc.is_special()) continue;
                    Segment::map_t::iterator it = segments_.find(utc);
                    if (it != segments_.end()) {
                        it->second->SetLocalPath(path, idx_ext_);
                        continue;
                    }
                    Segment::ptr_t segment(new Segment(log_prefix_, path, idx_ext_, s3bucket_, s3folder_));
                    if (segment->Initialize()) {
                        segments_[utc] = segment;
                        queue2_.Push(Queue::task_t(new SegmentUploadTask(segment)));
                    }
                } catch (boost::bad_lexical_cast&) {
                    continue;
                }
            }
            queue2_.Push(Queue::task_t(new RemoveExpiredSegmentsTask(this, boost::posix_time::microsec_clock::universal_time())));
        } catch (boost::filesystem::filesystem_error& ex) {
            Logger::Warning(boost::format("<%s> an error occured while initializing loopRec [ %s ] : %s") % app_ % name_ % ex.what());
            return false;
        } catch (std::exception& ex) {
            Logger::Warning(boost::format("<%s> an error occured while initializing loopRec [ %s ] : %s") % app_ % name_ % ex.what());
            return false;
        }
        queue_limit_ = conf_["queue"].to<int>(0);
        if (queue_limit_ > 0) {
            queue_limit_ = std::max<int>(queue_limit_, conf_["queue_limit_min"].to<int>(100));
            queue_limit_ = std::min<int>(queue_limit_, conf_["queue_limit_max"].to<int>(100000));
        }
        if (queue_limit_ == 0) {
            // write data without queue
            OnReceive = [this](const ReceiveOption& option, const Event::buf_t& buf, bool discrete) {
                std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
                return Write(buf, tick);
            };
            OnDisconnected = [this](const ReceiveOption& option) {
                std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
                return CloseWriter(tick);
            };
        } else {
            // write data via the queue
            OnReceive = [this](const ReceiveOption& option, const Event::buf_t& buf, bool discrete) {
                std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
                if (queue_limit_ <= 0) {
                    // unlimited queuing
                    return queue_.Push(Queue::task_t(new WriteTask(this, buf, tick)));
                }
                // time limited queuing in milliseconds
                std::shared_ptr<const WriteTask> first = queue_.GetFirstOf<WriteTask>();
                if (first && first->Tick() + std::chrono::milliseconds(queue_limit_) < tick) {
                    Logger::Warning(boost::format("%s : queue overflowed : %lld[ms]") % log_prefix_ % ((tick - first->Tick()).count() / 1000 / 1000));
                    queue_.Clear();
                    queue_.Push(Queue::task_t(new CloseWriterTask(this, tick)));
                }
                return queue_.Push(Queue::task_t(new WriteTask(this, buf, tick)));
            };
            OnDisconnected = [this](const ReceiveOption& option) {
                std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
                return queue_.Push(Queue::task_t(new CloseWriterTask(this, tick)));
            };
            queue_.Initialize();
        }
        return true;
    }
    virtual void Destroy() {
        queue_.Destroy();
        queue2_.Destroy();
        SenderRunner::vector_t runners;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runners.swap(sender_runners_);
        }
        for (SenderRunner::ptr_t sender_runner : runners)
            sender_runner->Destroy();
        runners.clear();
        writer_.reset();
        segments_.clear();
    }
    virtual bool IsAcceptable(const StreamOption& streamOption) const {
        const boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();
        const boost::posix_time::ptime at = GetStartedAt(streamOption.Get<std::string>("at"), now);
        if (at.is_special()) return false;
        if (at + boost::posix_time::seconds(total_duration_.count()) < now || now < at) return false;
        return true;
    }
    virtual void CreateSender(int sfd, const SendOption& sendOption, const StreamOption& streamOption) {
        SenderRunner::ptr_t sender_runner(new SenderRunner(this, sfd, sendOption, streamOption));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sender_runners_.push_back(sender_runner);
        }
        sender_runner->Initialize();
    }
    std::function<bool(const ReceiveOption& option, const Event::buf_t& buf, bool discrete)> OnReceive;
    std::function<bool(const ReceiveOption& option)> OnDisconnected;
protected:
    using time_segment_t = std::pair<boost::posix_time::ptime, Segment::ptr_t>;
    virtual bool Write(const Event::buf_t& buf, const std::chrono::steady_clock::time_point& tick) {
        std::string suffix = "Z"; // UTC
        if (writer_ && tick >= segment_time_) {
            CloseWriter(tick);
            suffix += CONTINUOUS; // '=' means continuous data from previous segment
        }
        if (!writer_) {
            boost::posix_time::ptime utc = boost::posix_time::microsec_clock::universal_time();
            boost::filesystem::path path = dir_ / (boost::posix_time::to_iso_string(utc) + suffix + dat_ext_);
            Segment::ptr_t segment(new Segment(log_prefix_, path, idx_ext_, s3bucket_, s3folder_));
            SegmentWriter::ptr_t writer(new SegmentWriter(log_prefix_, segment, segment_expand_, idx_interval_, idx_endian_, tick));
            if (segment->Initialize() && writer->Initialize()) {
                std::lock_guard<std::mutex> lock(mutex_);
                segments_[utc] = segment;
                writer_ = writer;
                if (suffix.length() == 2) { // CONTINUOUS
                    segment_time_ += segment_duration_;
                } else {
                    segment_time_ = tick + segment_duration_;
                }
            }
            queue2_.Push(Queue::task_t(new RemoveExpiredSegmentsTask(this, utc)));
        }
        if (writer_) {
            writer_->Write(tick, buf);
        }
        return true;
    }
    virtual bool CloseWriter(const std::chrono::steady_clock::time_point& tick) {
        SegmentWriter::ptr_t writer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writer_.swap(writer);
        }
        if (!writer) return true;
        buf_t zero;
        writer->Write(tick, zero); // write last index
        writer->Close();
        if (!s3folder_.empty()) queue2_.Push(Queue::task_t(new SegmentUploadTask(writer->GetSegment())));
        return true;
    }
    virtual void RemoveExpiredSegments(const boost::posix_time::ptime& utc) {
        boost::posix_time::seconds dur((total_duration_ + segment_duration_).count());
        std::lock_guard<std::mutex> lock(mutex_);
        Segment::map_t::iterator it = segments_.begin();
        for (; it != segments_.end(); ++it) {
            if (it->first + dur > utc) break;
            it->second->SetExpired(true);
        }
        segments_.erase(segments_.begin(), it);
        for (it = segments_.begin(); it != segments_.end(); ++it) {
            it->second->DeleteLocalIfS3Pushed();
        }
    }
    virtual void RemoveSender(SenderRunner::ptr_t sender_runner) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sender_runners_.erase(std::remove(sender_runners_.begin(), sender_runners_.end(), sender_runner), sender_runners_.end());
        }
        if (sender_runner) sender_runner->Destroy();
    }
    virtual time_segment_t GetSegment(const boost::posix_time::ptime& utc, bool next = false) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (segments_.empty()) {
            return std::make_pair(boost::posix_time::ptime(), Segment::ptr_t());
        }
        Segment::map_t::const_iterator it = segments_.upper_bound(utc);
        if (!next && it != segments_.begin()) {
            --it;
        }
        if (it == segments_.end()) {
            return std::make_pair(boost::posix_time::ptime(), Segment::ptr_t());
        }
        return std::make_pair(it->first, it->second);
    }
    virtual void Send(Sender::ptr_t sender, const StreamOption& option) const {
        if (!sender) return;
        const std::string log_prefix = (boost::format("%s > [ %s ]") % log_prefix_ % sender->GetOption().Get<std::string>("peer")).str();
        const boost::posix_time::ptime startedAt = GetStartedAt(option.Get<std::string>("at"));
        if (startedAt.is_special()) {
            return;
        }
        const int32_t bufsiz = std::max<int32_t>(1, std::min<int32_t>(option.Get<int32_t>("bufsiz", 188 * 7), 1456));
        const std::string gap = option.Get<std::string>("gap", "skip");
        const Speed speed(std::max<double>(option.Get<double>("speed", 1), 0.1));
        int32_t burst_ms = option.Get<int32_t>("burst", 0);
        Logger::Info(boost::format("%s : started : %s") % log_prefix % option(','));
        Event::buf_t buf(bufsiz);
        SegmentReader::ptr_t reader;
        SegmentReader::ptr_t next_reader;
        time_segment_t segment;
        time_segment_t next_segment;
        boost::filesystem::path last_segment_path;
        std::mutex prefetch_mutex;
        std::thread prefetch_thread;
        std::chrono::steady_clock::time_point base_time = std::chrono::steady_clock::now();
        while (sender->IsConnected()) {
            std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
            if (!reader) {
                boost::posix_time::ptime at = startedAt + boost::posix_time::microseconds((tick - base_time).count() * speed / 1000); // nanosec to microsec
                if (!CheckPlaybackPosition(at, speed, log_prefix)) {
                    break;
                }
                if (!segment.second) {
                    segment = GetSegment(at);
                    if (segment.second && segment.second->DatPath() == last_segment_path) {
                        segment.second.reset();
                    }
                }
                if (segment.first.is_special() || !segment.second) {
                    if (gap == "break") {
                        break;
                    }
                    Logger::Trace(boost::format("%s : missing segment") % log_prefix);
                    segment.second.reset();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (segment.first > at) {
                    if (gap == "break") {
                        break;
                    }
                    int64_t gap_ns = (segment.first - at).total_nanoseconds();
                    if (gap == "wait") {
                        segment.second.reset();
                        Logger::Trace(boost::format("%s : waiting next segment : %lld[ms]") % log_prefix % (gap_ns / 1000 / 1000));
                        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(gap_ns, 1000ll * 1000 * 100)));
                        continue;
                    }
                    Logger::Debug(boost::format("%s : skip : %lld[ms]") % log_prefix % (gap_ns / 1000 / 1000));
                    base_time -= std::chrono::nanoseconds(gap_ns / speed);
                    continue;
                }
                int64_t offset_ns = (at - segment.first).total_nanoseconds();
                if (std::chrono::nanoseconds(offset_ns) < segment_duration_) {
                    std::chrono::steady_clock::time_point baseTime = tick - std::chrono::nanoseconds(offset_ns / speed);
                    bool burst = false;
                    if (burst_ms > 0) {
                        // slide the base time to make a burst start
                        base_time -= std::chrono::milliseconds(burst_ms);
                        baseTime -= std::chrono::milliseconds(burst_ms);
                        burst_ms = 0;
                        burst = true;
                    }
                    reader.reset(new SegmentReader(log_prefix, segment.second, speed, idx_interval_, idx_endian_, baseTime));
                    if (burst) reader->SetBurst();
                }
                if (!reader || !reader->Initialize(offset_ns / 1000 / 1000, s3bucket_, s3bufsiz_)) {
                    reader.reset();
                    if (gap == "break") {
                        break;
                    }
                    if (gap == "wait") {
                        segment.second.reset();
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    segment = GetSegment(segment.first, true);
                    continue;
                }
                segment.second.reset();
            }
            bool read = false;
            while (sender->IsConnected()) {
                std::unique_lock<std::mutex> lock(mutex_);
                bool latest_segment = writer_ && writer_->GetSegment() == reader->GetSegment();
                lock.unlock();
                buf.resize(bufsiz);
                read = reader->Read(tick, buf, latest_segment);
                if (read || !latest_segment || speed.IsFast()) {
                    break; // read or reached end of segment
                }
                // incase of latest segment, continue reading
                assert(!reader->GetSegment()->S3Pushed());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                base_time += std::chrono::nanoseconds(100);
                continue;
            }
            if (!read) {
                last_segment_path = reader->GetSegment()->DatPath();
                boost::posix_time::ptime at = startedAt + boost::posix_time::microseconds((tick - base_time).count() * speed / 1000); // nanosec to microsec
                if (!CheckPlaybackPosition(at, speed, log_prefix)) {
                    break;
                }
                std::chrono::steady_clock::time_point baseTime = reader->BaseTime() + std::chrono::nanoseconds(segment_duration_.count() * 1000ll * 1000 * 1000 / speed);
                if (prefetch_ms_ > 0 && next_segment.second && next_segment.second->Continuous()) {
                    std::lock_guard<std::mutex> lk(prefetch_mutex);
                    bool burst = reader->IsBurst();
                    reader.swap(next_reader);
                    next_reader.reset();
                    next_segment.second.reset();
                    if (reader) {
                        if (burst) reader->SetBurst();
                        segment = next_segment;
                        continue;
                    }
                }
                segment = GetSegment(segment.first, true); // switch to next segment
                if (segment.second && segment.second->Continuous()) {
                    reader.reset(new SegmentReader(log_prefix, segment.second, speed, idx_interval_, idx_endian_, baseTime));
                    segment.second.reset();
                    if (!reader->Initialize(0, s3bucket_, s3bufsiz_)) {
                        reader.reset();
                    }
                } else {
                    reader.reset();
                    segment.second.reset();
                    if (gap == "break") {
                        break;
                    }
                }
                continue;
            } else if (speed.IsFast() && reader->ReachedIdxEnd()) {
                boost::posix_time::ptime at = startedAt + boost::posix_time::microseconds((tick - base_time).count() * speed / 1000); // nanosec to microsec
                if (!CheckPlaybackPosition(at, speed, log_prefix)) {
                    break;
                }
            }
            if (prefetch_ms_ > 0 && !next_segment.second) {
                int64_t remain_ms = segment_duration_.count() * 1000 - reader->PosNs() / 1000 / 1000;
                if (10 < remain_ms && remain_ms <= prefetch_ms_ * speed) {
                    next_segment = GetSegment(segment.first, true); // prefetch next segment
                    if (next_segment.second && next_segment.second->Continuous()) {
                        std::chrono::steady_clock::time_point baseTime = reader->BaseTime() + std::chrono::nanoseconds(segment_duration_.count() * 1000ll * 1000 * 1000 / speed);
                        if (prefetch_thread.joinable()) {
                            prefetch_thread.join();
                        }
                        prefetch_thread = std::thread([&, baseTime]() {
                            std::lock_guard<std::mutex> lk(prefetch_mutex);
                            if (!next_segment.second || !next_segment.second->Continuous()) return;
                            try {
                                next_reader.reset(new SegmentReader(log_prefix, next_segment.second, speed, idx_interval_, idx_endian_, baseTime));
                                if (next_reader->Initialize(0, s3bucket_, s3bufsiz_)) return;
                            } catch (std::exception& ex) {
                                Logger::Warning(boost::format("%s : prefetch failed : %s") % log_prefix_ % ex.what());
                            } catch (...) {
                                Logger::Warning(boost::format("%s : prefetch failed : %s") % log_prefix_ % "unknown exception");
                            }
                            next_reader.reset();
                        });
                    }
                }
            }
            if (buf.empty() || sender->Send(buf)) {
                continue;
            }
            std::string err = sender->GetErrMsg();
            Logger::Info(boost::format("%s : %s") % log_prefix % err);
            break;
        }
        if (prefetch_thread.joinable()) {
            prefetch_thread.join();
        }
        Logger::Info(boost::format("%s : done : %s") % log_prefix % option(','));
    }
    bool CheckPlaybackPosition(const boost::posix_time::ptime& at, const Speed& speed, const std::string& log_prefix) const {
        if (speed.IsNormal()) {
            return true;
        }
        const boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();
        if (speed.IsSlow() && at + boost::posix_time::seconds(total_duration_.count()) < now) {
            Logger::Warning(boost::format("%s : got out of the loop recording range") % log_prefix);
            return false;
        }
        if (speed.IsFast() && now < at) {
            Logger::Warning(boost::format("%s : reached the live position") % log_prefix);
            return false;
        }
        return true;
    }
    static boost::posix_time::ptime GetStartedAt(std::string str, const boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time()) {
        if (boost::algorithm::istarts_with(str, "now-")) {
            try {
                double delaySec = boost::lexical_cast<double>(str.substr(4));
                boost::posix_time::microseconds delay(static_cast<long long>(delaySec * 1000 * 1000));
                return now - delay;
            } catch (boost::bad_lexical_cast&) {
                return boost::posix_time::ptime();
            }
        }
        std::string tzd = "";
        std::string::size_type pos = str.find_last_of("TtZz+-");
        if (pos != std::string::npos && str[pos] != 'T' && str[pos] != 't') {
            tzd = str.substr(pos);
            str = str.substr(0, pos);
        }
        boost::posix_time::ptime time;
        bool extended = false;
        try {
            time = boost::posix_time::from_iso_string(str);
        } catch (boost::bad_lexical_cast&) {
        }
        if (time.is_special()) {
            try {
                time = boost::posix_time::from_iso_extended_string(str);
                extended = true;
            } catch (boost::bad_lexical_cast&) {
            }
        }
        if (time.is_special() || tzd[0] == 'z' || tzd[0] == 'Z') {
            return time;
        } else if (tzd[0] == '+' || tzd[0] == '-') {
            try {
                int32_t hours = 0, minutes = 0;
                if (extended) {
                    std::vector<std::string> tz;
                    boost::algorithm::split(tz, tzd, boost::algorithm::is_any_of(":"));
                    if (tz.size() >= 1) hours = boost::lexical_cast<int32_t>(tz[0]);
                    if (tz.size() >= 2) minutes = boost::lexical_cast<int32_t>(tz[1]);
                } else {
                    hours = boost::lexical_cast<int32_t>(tzd.substr(0, 3));
                    minutes = boost::lexical_cast<int32_t>(tzd.substr(3));
                }
                if (hours >= 0) {
                    return time - boost::posix_time::time_duration(hours, minutes, 0);
                } else {
                    return time + boost::posix_time::time_duration(-hours, minutes, 0);
                }
            } catch (boost::bad_lexical_cast&) {
            }
        }
        const boost::posix_time::ptime local = boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(now);
        const boost::posix_time::posix_time_system::time_duration_type td = local - now;
        return time - td;
    }
};

LoopRec::map_t LoopRec::Create(const Json::Node& loopRecs, const std::string& app) {
    map_t map;
    for (size_t i = 0, c = loopRecs.size(); i < c; ++i) {
        const Json conf = loopRecs[i];
        std::string name = conf["name"].to<std::string>("");
        if (name.empty()) continue;
        ptr_t loopRec(new LoopRec(conf, app, name));
        if (loopRec->Initialize()) map[name] = loopRec;
    }
    return map;
}
LoopRec::LoopRec(const Json& conf, const std::string& app, const std::string& name)
    : pimpl_(new Impl(this, conf, app, name)) {
}
LoopRec::~LoopRec() {
    pimpl_.reset();
}
bool LoopRec::Initialize() {
    return pimpl_->Initialize();
}
void LoopRec::Destroy() {
    return pimpl_->Destroy();
}
bool LoopRec::IsAcceptable(const StreamOption& streamOption) const {
    return pimpl_->IsAcceptable(streamOption);
}
void LoopRec::CreateSender(int sfd, const SendOption& sendOption, const StreamOption& streamOption) {
    return pimpl_->CreateSender(sfd, sendOption, streamOption);
}
bool LoopRec::OnReceive(const ReceiveOption& option, const Event::buf_t& buf, bool discrete) {
    return pimpl_->OnReceive(option, buf, discrete);
}
bool LoopRec::OnDisconnected(const ReceiveOption& option) {
    return pimpl_->OnDisconnected(option);
}
