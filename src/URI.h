#pragma once

//----------------------------------------------------------------------------
/// @class URIUtil
//----------------------------------------------------------------------------
class URIUtil
{
public:
    static const char* SeekDelimiter(const char* str, const char* delimiters);
    static const char* SeekNonDelimiter(const char* str, const char* delimiters);

    static bool IsValidAddr(const char* addr, int* af);
    static bool IsIPv4Addr(const char* addr);
    static bool IsIPv6Addr(const char* addr);

    static std::string DecodeURI(const char* src, size_t len);
    static std::string DecodeURI(const std::string& str) { return DecodeURI(str.c_str(), str.size()); }
    static std::string EncodeURI(const char* src, size_t len, const char* safe);
    static std::string EncodeURI(const std::string& str, const char* safe) { return EncodeURI(str.c_str(), str.size(), safe); }

    static void SplitURI(const char* uri, std::string* scheme, std::string* authority, std::string* path, std::string* query, std::string* fragment);
    static std::string MakeURI(const char* scheme, const char* authority, const char* path, const char* query, const char* fragment);

    static void SplitAuthority(const char* authority, std::string* user, std::string* password, std::string* host, std::string* port);
    static std::string MakeAuthority(const char* user, const char* password, const char* host, const char* port);
};

//----------------------------------------------------------------------------
/// @class URI
//----------------------------------------------------------------------------
class URI : public URIUtil
{
public:
    URI(const char* uri = nullptr) { Parse(uri); }
    URI(const std::string& uri) { Parse(uri); }
    virtual ~URI() {}
    virtual URI& Parse(const char* uri);
    virtual URI& Parse(const std::string& uri) { return Parse(uri.c_str()); }
    virtual bool IsRelative() const { return scheme.empty() && host.empty(); }
    virtual URI Copy() const { return URI(*this); }
    virtual std::string operator()() const;
    virtual URI& Encode();
    virtual URI& Decode();
    virtual bool Verify() const;

public:
    std::string scheme;
    std::string user;
    std::string pass;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    std::string fragment;
};

//----------------------------------------------------------------------------
/// @class URIOption
//----------------------------------------------------------------------------
class URIOption : public URIUtil
{
public:
    struct comp { bool operator()(const std::string& l, const std::string& r) const; };
    using map_t = std::map<std::string, std::string, comp>;
protected:
    map_t map_;
    map_t synonym_;
    const std::string& actualKey(const std::string& key) const;
public:
    URIOption(const std::string& optstr = "", const std::string& anp = "&", const std::string& equ = "=", bool decode = true) : map_(), synonym_() { Parse(optstr, anp, equ, decode); }
    virtual ~URIOption() {}
    virtual std::string operator()(char anp = '&', char equ = L'=', bool encode = true, const char* safe = nullptr) const;
    virtual URIOption& Parse(const std::string& optstr, const std::string& anp = "&", const std::string& equ = "=", bool decode = true);
    virtual URIOption& Add(const std::string& optstr, const std::string& equ = "=", bool decode = true);
    virtual URIOption& Erase(const std::string& key);
    virtual URIOption& Clear() { map_.clear(); return *this; }
    virtual std::string& operator[](const std::string& key) { return map_[actualKey(key)]; }
    virtual bool operator==(const URIOption& rhs) const { return map_ == rhs.map_; }
    virtual bool operator!=(const URIOption& rhs) const { return !operator==(rhs); }
    virtual bool Has(const std::string& key) const { return map_.find(actualKey(key)) != map_.end(); }
    virtual bool HasExcept(const std::string& key, const std::string& except = "false") const;
    template <class Type> Type Get(const std::string& key, const Type& defval = Type()) const;
    template <class Type> Type Get(const std::string& key, const Type& defNoEntry, const Type& defBlank) const;
    virtual void Synonym(const std::string& key, const std::string& synonym);
    virtual const map_t& GetMap() const { return map_; }
};
