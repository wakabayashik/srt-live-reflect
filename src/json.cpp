#include "stdafx.h"
#include "json.h"

Json::Node& Json::Node::operator=(const boost::json::value& value) {
    if (value_) {
        *value_ = value;
        return *this;
    }
    value_ = root_;
    for (path_t::iterator it = path_.begin(); value_ && it != path_.end(); ++it) {
        if (it->idx != static_cast<size_t>(-1)) {
            if (value_->is_null()) {
                *value_ = boost::json::array();
            }
            if (!value_->is_array()) {
                value_ = nullptr;
                break;
            }
            boost::json::array& arr = value_->as_array();
            while (arr.size() <= it->idx) {
                arr.push_back(boost::json::value());
            }
            value_ = arr.if_contains(it->idx);
        } else if (!it->name.empty()) {
            if (value_->is_null()) {
                *value_ = boost::json::object();
            }
            if (!value_->is_object()) {
                value_ = nullptr;
                break;
            }
            boost::json::object& obj = value_->as_object();
            value_ = obj.if_contains(it->name);
            if (!value_) {
                obj[it->name] = boost::json::value();
                value_ = obj.if_contains(it->name);
            }
        } else {
            value_ = nullptr;
            break;
        }
    }
    if (value_) {
        *value_ = value;
    }
    return *this;
}

Json::Node Json::Node::operator[](const std::string& name) {
    path_t path = path_;
    path.push_back(Path{ name, static_cast<size_t>(-1) });
    if (!value_ || !value_->is_object()) return Node(path, root_, nullptr);
    return Node(path, root_, value_->as_object().if_contains(name));
}

Json::Node Json::Node::operator[](size_t idx) {
    path_t path = path_;
    path.push_back(Path{ "", idx });
    if (!value_ || !value_->is_array()) return Node(path, root_, nullptr);
    return Node(path, root_, value_->as_array().if_contains(idx));
}

const Json::Node Json::Node::operator[](const std::string& name) const {
    if (!value_ || !value_->is_object()) return Node(path_t(), nullptr, nullptr);
    return Node(path_t(), nullptr, value_->as_object().if_contains(name));
}

const Json::Node Json::Node::operator[](size_t idx) const {
    if (!value_ || !value_->is_array()) return Node(path_t(), nullptr, nullptr);
    return Node(path_t(), nullptr, value_->as_array().if_contains(idx));
}

boost::json::value Json::Node::remove(const std::string& name) {
    if (!value_ || !value_->is_object()) return boost::json::value();
    boost::json::object& obj = value_->as_object();
    boost::json::value* value = obj.if_contains(name);
    if (!value) return boost::json::value();
    boost::json::value removed = *value;
    obj.erase(name);
    return removed;
}

boost::json::value Json::Node::remove(size_t idx) {
    if (!value_ || !value_->is_array()) return boost::json::value();
    boost::json::array& arr = value_->as_array();
    boost::json::value* value = arr.if_contains(idx);
    if (!value) return boost::json::value();
    boost::json::value removed = *value;
    arr.erase(value, value + 1);
    return removed;
}

std::string Json::Node::serialize(int32_t indent) const {
    if (!value_) return "";
    if (indent < 0) return boost::json::serialize(*value_);
    std::stringstream ss;
    pretty_print(ss, *value_, indent);
    return ss.str();
}

template <typename Type> Type Json::Node::to(const Type& defVal) const {
    if (!value_ || value_->is_null()) return defVal;
    if (value_->is_int64()) return static_cast<Type>(value_->as_int64());
    if (value_->is_uint64()) return static_cast<Type>(value_->as_uint64());
    if (value_->is_double()) return static_cast<Type>(value_->as_double());
    if (value_->is_bool()) return static_cast<Type>(value_->as_bool());
    try {
        if (value_->is_string()) return boost::lexical_cast<Type>(value_->as_string());
        return defVal;
    } catch (boost::bad_lexical_cast&) {
        return defVal;
    }
}

template <> std::string Json::Node::to(const std::string& defVal) const {
    if (!value_ || value_->is_null()) return defVal;
    if (value_->is_string()) return value_->as_string().data();
    try {
        if (value_->is_int64()) return boost::lexical_cast<std::string>(value_->as_int64());
        if (value_->is_uint64()) return boost::lexical_cast<std::string>(value_->as_uint64());
        if (value_->is_double()) return boost::lexical_cast<std::string>(value_->as_double());
        if (value_->is_bool()) return boost::lexical_cast<std::string>(value_->as_bool());
        return defVal;
    } catch (boost::bad_lexical_cast&) {
        return defVal;
    }
}

template <> boost::filesystem::path Json::Node::to(const boost::filesystem::path& defVal) const {
    std::string s = to<std::string>();
    if (s.empty()) return defVal;
#if defined(WIN32) || defined(WIN64)
    struct TO_W {
        std::wstring operator()(const char* src, int cp) {
            int len = MultiByteToWideChar(cp, 0, src, -1, NULL, 0);
            if (!len) return L"";
            std::wstring dst(len, L'\0');
            MultiByteToWideChar(cp, 0, src, -1, &dst[0], len);
            if (dst[len - 1] == L'\0') dst.resize(--len);
            return dst;
        }
    } to_w;
    struct TO_A {
        std::string operator()(const wchar_t* src, int cp) {
            int len = WideCharToMultiByte(cp, 0, src, -1, NULL, 0, NULL, NULL);
            if (!len) return "";
            std::string dst(len, '\0');
            WideCharToMultiByte(cp, 0, src, -1, &dst[0], len, NULL, NULL);
            if (dst[len - 1] == '\0') dst.resize(--len);
            return dst;
        }
    } to_a;
    return boost::filesystem::path(to_a(to_w(s.c_str(), CP_UTF8).c_str(), CP_ACP));
#else
    return boost::filesystem::path(s);
#endif
}

template <> Json::keys_t Json::Node::to(const keys_t& defVal) const {
    if (!value_ || !value_->is_object()) return defVal;
    const boost::json::object& obj = value_->as_object();
    keys_t keys;
    for (boost::json::object::const_iterator it = obj.begin(); it != obj.end(); ++it) {
        keys.push_back(it->key());
    }
    return keys;
}

template char Json::Node::to(const char&) const;
template unsigned char Json::Node::to(const unsigned char&) const;
template short Json::Node::to(const short&) const;
template unsigned short Json::Node::to(const unsigned short&) const;
template int Json::Node::to(const int&) const;
template unsigned int Json::Node::to(const unsigned int&) const;
template long Json::Node::to(const long&) const;
template unsigned long Json::Node::to(const unsigned long&) const;
template long long Json::Node::to(const long long&) const;
template unsigned long long Json::Node::to(const unsigned long long&) const;
template double Json::Node::to(const double&) const;

Json Json::parse(const std::string& str) {
    boost::json::parse_options opt;
    opt.allow_comments = true;
    opt.allow_trailing_commas = true;
    opt.allow_invalid_utf8 = true;
    return Json(boost::json::parse(str, boost::json::storage_ptr(), opt));
}

Json Json::load(const std::string& path) {
    boost::json::parse_options opt;
    opt.allow_comments = true;
    opt.allow_trailing_commas = true;
    opt.allow_invalid_utf8 = true;
    boost::json::stream_parser parser(boost::json::storage_ptr(), opt);
    std::ifstream file(path);
    std::string line;
    bool first = true;
    while (std::getline(file, line)) {
        if (first) {
            first = false;
            if (line.length() >= 3 && static_cast<uint8_t>(line[0]) == 0xef && static_cast<uint8_t>(line[1]) == 0xbb && static_cast<uint8_t>(line[2]) == 0xbf) {
                line = line.substr(3); // remove utf-8 BOM
            }
        }
        parser.write(line + "\n");
    }
    parser.finish();
    return Json(parser.release());
}

void Json::pretty_print(std::ostream& os, const boost::json::value& jv, int32_t indent, int32_t max_depth, int32_t depth) {
    if (indent < 0) indent = 0;
    if (depth < 0) depth = 0;
    switch (jv.kind()) {
        case boost::json::kind::object: {
            const boost::json::object& obj = jv.get_object();
            if (obj.empty() || (0 <= max_depth && max_depth <= depth)) {
                os << "{}";
                break;
            }
            os << "{\n";
            std::string prefix = std::string(static_cast<std::string::size_type>(++depth) * indent, ' ');
            for (boost::json::object::const_iterator it = obj.begin(), end = obj.end();;) {
                os << prefix << boost::json::serialize(it->key()) << ": ";
                pretty_print(os, it->value(), indent, max_depth, depth);
                if (++it == end) break;
                os << ",\n";
            }
            os << "\n" << std::string(static_cast<std::string::size_type>(--depth) * indent, ' ') << "}";
            break;
        }
        case boost::json::kind::array: {
            const boost::json::array& arr = jv.get_array();
            if (!arr.size() || (0 <= max_depth && max_depth <= depth)) {
                os << "[]";
                break;
            }
            os << "[\n";
            std::string prefix = std::string(static_cast<std::string::size_type>(++depth) * indent, ' ');
            for (boost::json::array::const_iterator it = arr.begin(), end = arr.end();;) {
                os << prefix;
                pretty_print(os, *it, indent, max_depth, depth);
                if (++it == end) break;
                os << ",\n";
            }
            os << "\n" << std::string(static_cast<std::string::size_type>(--depth) * indent, ' ') << "]";
            break;
        }
        case boost::json::kind::string: {
            os << boost::json::serialize(jv.get_string());
            break;
        }
        case boost::json::kind::uint64: {
            os << jv.get_uint64();
            break;
        }
        case boost::json::kind::int64: {
            os << jv.get_int64();
            break;
        }
        case boost::json::kind::double_: {
            os << jv.get_double();
            break;
        }
        case boost::json::kind::bool_: {
            os << (jv.get_bool() ? "true" : "false");
            break;
        }
        case boost::json::kind::null: {
            os << "null";
            break;
        }
    }
}
