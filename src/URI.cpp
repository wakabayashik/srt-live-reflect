#include "stdafx.h"
#include "URI.h"

//----------------------------------------------------------------------------
/// @fn URIUtil::SeekDelimiter
/// seek next delimiter
///
/// @param[in]      str
/// @param[in]      delimiters
///
/// @retval         nullptr     not found
/// @retval         other       pos of next delimiter character
//----------------------------------------------------------------------------
const char* URIUtil::SeekDelimiter(const char* str, const char* delimiters) {
    if (!str)
        return nullptr;

    while (*str != '\0') {
        if (strchr(delimiters, *str))
            return str;

        ++str;
    }
    return nullptr;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::SeekNonDelimiter
/// seek next non-delimiter
///
/// @param[in]      str
/// @param[in]      delimiters
///
/// @retval         nullptr         not found
/// @retval         other           pos of next non-delimiter character
//----------------------------------------------------------------------------
const char* URIUtil::SeekNonDelimiter(const char* str, const char* delimiters) {
    if (!str)
        return nullptr;

    while (*str != '\0') {
        if (!strchr(delimiters, *str))
            return str;

        ++str;
    }
    return nullptr;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
#define LOWALPHA        "abcdefghijklmnopqrstuvwxyz"
#define UPALPHA         "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define ALPHA           LOWALPHA UPALPHA
#define DIGIT           "0123456789"
#define GEN_DELIMS      ":/?#[]@"
#define SUB_DELIMS      "!$&'()*+,;="
#define RESERVED        GEN_DELIMS SUB_DELIMS
#define UNRESERVED      ALPHA DIGIT "-._~"

#define SCHEME_SAFE     ALPHA DIGIT "+-."
#define AUTHORITY_SAFE  UNRESERVED SUB_DELIMS ":@[]"
#define PATH_SAFE       UNRESERVED SUB_DELIMS ":@/"
#define QUERY_SAFE      UNRESERVED SUB_DELIMS ":@/?"
#define FRAGMENT_SAFE   UNRESERVED SUB_DELIMS ":@/?"
#define OPTION_SAFE     UNRESERVED "!$'()*+,;:@/?" // remove "&=" from QUERY_SAFE

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
#define USER_SAFE       UNRESERVED SUB_DELIMS
#define HOST_SAFE       UNRESERVED SUB_DELIMS ":"
#define PORT_SAFE       DIGIT

//----------------------------------------------------------------------------
/// @fn URIUtil::DecodeURI
/// decode pct-encoded
///
/// @param[in]      src
/// @param[in]      len
//----------------------------------------------------------------------------
std::string URIUtil::DecodeURI(const char* src, size_t len) {
    static const unsigned char  dec[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0,    1,    2,    3,    4,    5,    6,    7,    8,    9,    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff,  0xa,  0xb,  0xc,  0xd,  0xe,  0xf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff,  0xa,  0xb,  0xc,  0xd,  0xe,  0xf,
    };
    std::string dst;
    dst.reserve(dst.size() + len);
    for (; len; ++src, --len) {
        if (*src != '%') {
            dst.append(1, *src);
            continue;
        }
        if (len <= 2) {
            dst.append(1, *src);
            continue;
        }
        const unsigned char tmp1 = static_cast<const unsigned char>(*(++src));
        const unsigned char tmp2 = static_cast<const unsigned char>(*(++src));
        len -= 2;
        if (tmp1 >= sizeof(dec) || dec[tmp1] > 0xf || tmp2 >= sizeof(dec) || dec[tmp2] > 0xf) {
            dst.append(1, '%');
            dst.append(1, static_cast<const char>(tmp1));
            dst.append(1, static_cast<const char>(tmp2));
            continue;
        }
        dst.append(1, static_cast<const char>((dec[tmp1] << 4) | dec[tmp2]));
    }
    return dst;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::EncodeURI
/// encode pct-encoded
///
/// @param[in]      src
/// @param[in]      len
/// @param[in]      safe        characters that could be written without encoding
//----------------------------------------------------------------------------
std::string URIUtil::EncodeURI(const char* src, size_t len, const char* safe) {
    static const char enc[] = "0123456789ABCDEF";
    std::string dst;
    dst.reserve(dst.size() + len);
    for (; len; ++src, --len) {
        if (safe && strchr(safe, *src)) {
            dst.append(1, *src);
            continue;
        }
        if (dst.capacity() < dst.length() + len + 2)
            dst.reserve(dst.capacity() + len + 10);

        dst.append(1, '%');
        dst.append(1, enc[static_cast<unsigned char>(*src) >> 4]);
        dst.append(1, enc[static_cast<unsigned char>(*src) & 0x0f]);
    }
    return dst;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::SplitURI
///
/// @param[in]      uri
/// @param[out]     scheme
/// @param[out]     authority
/// @param[out]     path
/// @param[out]     query
/// @param[out]     fragment
//----------------------------------------------------------------------------
void    URIUtil::SplitURI(const char* uri, std::string* scheme, std::string* authority, std::string* path, std::string* query, std::string* fragment) {
    if (scheme)     *scheme = "";
    if (authority)  *authority = "";
    if (path)       *path = "";
    if (query)      *query = "";
    if (fragment)   *fragment = "";
    if (!uri || !*uri) return;

    const char* c = strchr(uri, ':');
    if (c && SeekNonDelimiter(uri, SCHEME_SAFE) >= c) {
        if (scheme) scheme->append(uri, c - uri);
        ++c;
    } else {
        c = uri;
    }
    if (*c == '/' && *(c + 1) == '/') {
        const char* cc = SeekDelimiter(c += 2, "/?#");
        if (!cc) {
            if (authority) authority->append(c);
            return;
        }
        if (authority) authority->append(c, cc - c);
        c = cc;
    }
    if (*c != '?' && *c != '#') {
        const char* cc = SeekDelimiter(c, "?#");
        if (!cc) {
            if (path) path->append(c);
            return;
        }
        if (path) path->append(c, cc - c);
        c = cc;
    }
    if (*c == '?') {
        const char* cc = strchr(++c, '#');
        if (!cc) {
            if (query) query->append(c);
            return;
        }
        if (query) query->append(c, cc - c);
        c = cc;
    }
    ++c;
    if (fragment) fragment->append(c);
}

//----------------------------------------------------------------------------
/// @fn URIUtil::MakeURI
///
/// @param[in]      scheme
/// @param[in]      authority
/// @param[in]      path
/// @param[in]      query
/// @param[in]      fragment
//----------------------------------------------------------------------------
std::string URIUtil::MakeURI(const char* scheme, const char* authority, const char* path, const char* query, const char* fragment) {
    size_t  scheme_len = scheme ? strlen(scheme) : 0;
    size_t  authority_len = authority ? strlen(authority) : 0;
    size_t  path_len = path ? strlen(path) : 0;
    size_t  query_len = query ? strlen(query) : 0;
    size_t  fragment_len = fragment ? strlen(fragment) : 0;

    std::string uri;
    uri.reserve(scheme_len + authority_len + path_len + query_len + fragment_len + 5);

    if (scheme_len) {
        uri.append(scheme);
        uri.append(1, ':');
    }
    if (authority) {
        // if authority is not nullptr, write "//" even if authority is empty
        uri.append(2, '/');
        uri.append(authority);
    }
    if (path_len) {
        // path should starts with '/' if authority exists
        if (authority && path[0] != '/') uri.append(1, '/');
        uri.append(path);
    }
    if (query_len) {
        uri.append(1, '?');
        uri.append(query);
    }
    if (fragment_len) {
        uri.append(1, '#');
        uri.append(fragment);
    }
    return uri;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::SplitAuthority
///
/// @param[in]      authority
/// @param[out]     user
/// @param[out]     password
/// @param[out]     host
/// @param[out]     port
//----------------------------------------------------------------------------
void    URIUtil::SplitAuthority(const char* authority, std::string* user, std::string* password, std::string* host, std::string* port) {
    if (user)       *user = "";
    if (password)   *password = "";
    if (host)       *host = "";
    if (port)       *port = "";
    if (!authority || !*authority) return;

    const char* c = strchr(authority, '@');
    if (c) {
        const char* cc = strchr(authority, ':');
        if (cc && cc < c) {
            if (user) user->append(authority, cc - authority);
            ++cc;
            if (password) password->append(cc, c - cc);
        } else {
            if (user) user->append(authority, c - authority);
        }
        ++c;
    } else {
        c = authority;
    }

    {
        const char* cc = strchr(c, ']');
        if (*c == '[' && cc) {
            ++c;
            if (host) host->append(c, cc - c);
            ++cc;
            if (*cc == '\0' || *cc != ':') return;
        } else {
            cc = strchr(c, ':');
            if (!cc) {
                if (host) host->append(c);
                return;
            }
            if (host) host->append(c, cc - c);
        }
        c = ++cc;
    }
    if (port) port->append(c);
}

//----------------------------------------------------------------------------
// @fn URIUtil::MakeAuthority
///
/// @param[in]      user
/// @param[in]      password
/// @param[in]      host
/// @param[in]      port
//----------------------------------------------------------------------------
std::string URIUtil::MakeAuthority(const char* user, const char* password, const char* host, const char* port) {
    size_t  user_len = user ? strlen(user) : 0;
    size_t  password_len = password ? strlen(password) : 0;
    size_t  host_len = host ? strlen(host) : 0;
    size_t  port_len = port ? strlen(port) : 0;

    std::string authority;
    authority.reserve(user_len + password_len + host_len + port_len + 5);

    if (user_len) {
        authority.append(user);
        if (password_len) {
            authority.append(1, ':');
            authority.append(password);
        }
        authority.append(1, '@');
    }
    if (host_len) {
        bool ipv6 = IsIPv6Addr(host);
        if (ipv6) authority.append(1, '[');
        authority.append(host);
        if (ipv6) authority.append(1, ']');
    }
    if (port_len) {
        authority.append(1, ':');
        authority.append(port);
    }
    return authority;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::IsValidAddr
///
/// @param[in]      addr
/// @param[out]     af
/// @retval         true        addr is valid IP address string
/// @retval         false
//----------------------------------------------------------------------------
bool    URIUtil::IsValidAddr(const char* addr, int* af) {
    addrinfo*   info = 0;
    addrinfo    hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags |= AI_NUMERICHOST;
    if (getaddrinfo(addr, nullptr, &hints, &info) != 0 || info == 0)
        return false;

    if (af) *af = info->ai_family;
    freeaddrinfo(info);
    return true;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::IsIPv4Addr
///
/// @param[in]      addr
/// @retval         true        addr is valid IPv4 address string
/// @retval         false
//----------------------------------------------------------------------------
bool    URIUtil::IsIPv4Addr(const char* addr) {
    int af = 0;
    return IsValidAddr(addr, &af) && af == AF_INET;
}

//----------------------------------------------------------------------------
/// @fn URIUtil::IsIPv6Addr
///
/// @param[in]      addr
/// @retval         true        addr is valid IPv6 address string
/// @retval         false
//----------------------------------------------------------------------------
bool    URIUtil::IsIPv6Addr(const char* addr) {
    int af = 0;
    return IsValidAddr(addr, &af) && af == AF_INET6;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
URI&   URI::Parse(const char* uri) {
    std::string authority;
    SplitURI(uri, &scheme, &authority, &path, &query, &fragment);
    SplitAuthority(authority.c_str(), &user, &pass, &host, &port);
    return *this;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
std::string URI::operator()() const {
    if (!Verify())
        return "";

    if (IsRelative()) {
        // path?query#fragment
        return MakeURI(nullptr, nullptr, path.c_str(), query.c_str(), fragment.c_str());
    }

    std::string authority = MakeAuthority(user.c_str(), pass.c_str(), host.c_str(), port.c_str());
    if (authority.empty() && !path.empty() && path.at(0) != '/') {
        // scheme:path?query#fragment
        return MakeURI(scheme.c_str(), nullptr, path.c_str(), query.c_str(), fragment.c_str());
    }

    // scheme://authority/path?query#fragment
    return MakeURI(scheme.c_str(), authority.c_str(), path.c_str(), query.c_str(), fragment.c_str());
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
URI&   URI::Encode() {
    //scheme = EncodeURI(scheme.c_str(), scheme.length(), SCHEME_SAFE);
    user = EncodeURI(user.c_str(), user.length(), USER_SAFE);
    pass = EncodeURI(pass.c_str(), pass.length(), USER_SAFE);
    host = EncodeURI(host.c_str(), host.length(), HOST_SAFE);
    //port = EncodeURI(port.c_str(), port.length(), PORT_SAFE);
    path = EncodeURI(path.c_str(), path.length(), PATH_SAFE);
    query = EncodeURI(query.c_str(), query.length(), QUERY_SAFE);
    fragment = EncodeURI(fragment.c_str(), fragment.length(), FRAGMENT_SAFE);
    return *this;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
URI&   URI::Decode()
{
    //scheme = DecodeURI(scheme.c_str(), scheme.length());
    user = DecodeURI(user.c_str(), user.length());
    pass = DecodeURI(pass.c_str(), pass.length());
    host = DecodeURI(host.c_str(), host.length());
    //port = DecodeURI(port.c_str(), port.length());
    path = DecodeURI(path.c_str(), path.length());
    query = DecodeURI(query.c_str(), query.length());
    fragment = DecodeURI(fragment.c_str(), fragment.length());
    return *this;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
bool    URI::Verify() const
{
    return !SeekNonDelimiter(scheme.c_str(), SCHEME_SAFE)
        && !SeekNonDelimiter(user.c_str(), USER_SAFE "%")
        && !SeekNonDelimiter(pass.c_str(), USER_SAFE "%")
        && !SeekNonDelimiter(host.c_str(), HOST_SAFE "%")
        && !SeekNonDelimiter(port.c_str(), PORT_SAFE)
        && !SeekNonDelimiter(path.c_str(), PATH_SAFE "%")
        && !SeekNonDelimiter(query.c_str(), QUERY_SAFE "%")
        && !SeekNonDelimiter(fragment.c_str(), FRAGMENT_SAFE "%");
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
const std::string& URIOption::actualKey(const std::string& key) const {
    map_t::const_iterator it = synonym_.find(key);
    if (it == synonym_.end()) {
        return key;
    } else {
        return it->second;
    }
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
std::string URIOption::operator()(char anp, char equ, bool encode, const char* safe) const {
    if (safe == nullptr) {
        safe = OPTION_SAFE;
    }
    std::string val;
    for (map_t::const_iterator it = map_.begin(); it != map_.end(); ++it) {
        if (!val.empty()) val += anp;
        if (encode) {
            val += EncodeURI(it->first, safe);
            if (it->second.empty()) continue;
            val += equ;
            val += EncodeURI(it->second, safe);
        } else {
            val += it->first;
            if (it->second.empty()) continue;
            val += equ;
            val += it->second;
        }
    }
    return val;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
URIOption& URIOption::Parse(const std::string& optstr, const std::string& anp, const std::string& equ, bool decode) {
    using sep_t = boost::char_separator<char>;
    using tokenizer_t = boost::tokenizer<sep_t, std::string::const_iterator, std::string>;
    if (!optstr.empty()) {
        sep_t sep(anp.c_str());
        tokenizer_t token(optstr, sep);
        for (tokenizer_t::const_iterator it = token.begin(); it != token.end(); ++it) {
            Add(*it, equ, decode);
        }
    }
    return *this;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
URIOption& URIOption::Add(const std::string& optstr, const std::string& equ, bool decode) {
    std::string::size_type pos = optstr.find_first_of(equ);
    std::string key, val;
    if (pos == std::string::npos) {
        key = optstr;
        val = "";
    } else {
        key = optstr.substr(0, pos);
        val = optstr.substr(pos + 1);
    }
    if (decode) {
        operator[](DecodeURI(key)) = DecodeURI(val);
    } else {
        operator[](key) = val;
    }
    return *this;
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
URIOption& URIOption::Erase(const std::string& key) {
    map_t::const_iterator it = map_.find(actualKey(key));
    if (it != map_.end()) map_.erase(it);
    return *this;
}

//----------------------------------------------------------------------------
/// @param[in]      key
/// @param[in]      except
/// @retval         true
/// @retval         false
//----------------------------------------------------------------------------
bool URIOption::HasExcept(const std::string& key, const std::string& except) const {
    map_t::const_iterator it = map_.find(actualKey(key));
    return it != map_.end() && !boost::algorithm::iequals(it->second, except);
}

//----------------------------------------------------------------------------
/// @param[in]      key
/// @param[in]      defval
//----------------------------------------------------------------------------
template <class Type> Type URIOption::Get(const std::string& key, const Type& defval) const {
    return Get(key, defval, defval);
}

//----------------------------------------------------------------------------
/// @param[in]      key
/// @param[in]      defNoEntry
/// @param[in]      defBlank
//----------------------------------------------------------------------------
template <class Type> Type URIOption::Get(const std::string& key, const Type& defNoEntry, const Type& defBlank) const {
    try {
        map_t::const_iterator it = map_.find(actualKey(key));
        return it == map_.end() ? defNoEntry : it->second.empty() ? defBlank : boost::lexical_cast<Type>(it->second);
    } catch (boost::bad_lexical_cast&) {
        return defBlank;
    }
}

//----------------------------------------------------------------------------
/// @param[in]      key
/// @param[in]      defNoEntry
/// @param[in]      defBlank
//----------------------------------------------------------------------------
template <> std::string URIOption::Get(const std::string& key, const std::string& defNoEntry, const std::string& defBlank) const {
    try {
        map_t::const_iterator it = map_.find(actualKey(key));
        return it == map_.end() ? defNoEntry : it->second.empty() ? defBlank : it->second;
    } catch (boost::bad_lexical_cast&) {
        return defBlank;
    }
}

//----------------------------------------------------------------------------
//  explicit instantiation
//----------------------------------------------------------------------------
template std::string URIOption::Get(const std::string& key, const std::string& defval) const;
template float URIOption::Get(const std::string& key, const float& defval) const;
template double URIOption::Get(const std::string& key, const double& defval) const;
template char URIOption::Get(const std::string& key, const char& defval) const;
template unsigned char URIOption::Get(const std::string& key, const unsigned char& defval) const;
template short URIOption::Get(const std::string& key, const short& defval) const;
template unsigned short URIOption::Get(const std::string& key, const unsigned short& defval) const;
template int URIOption::Get(const std::string& key, const int& defval) const;
template unsigned int URIOption::Get(const std::string& key, const unsigned int& defval) const;
template long URIOption::Get(const std::string& key, const long& defval) const;
template unsigned long URIOption::Get(const std::string& key, const unsigned long& defval) const;
template long long URIOption::Get(const std::string& key, const long long& defval) const;
template unsigned long long URIOption::Get(const std::string& key, const unsigned long long& defval) const;

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void URIOption::Synonym(const std::string& key, const std::string& synonym)
{
    synonym_[synonym] = key;
    map_t::const_iterator it = map_.find(synonym);
    if (it == map_.end()) return;
    if (map_.find(key) == map_.end()) map_[key] = it->second;
    map_.erase(it);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
bool URIOption::comp::operator()(const std::string& lhs, const std::string& rhs) const {
#if defined(WIN32) || defined(WIN64)
    return _stricmp(lhs.c_str(), rhs.c_str()) < 0;
#else
    return strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
#endif
}
