// Minimal stand-in for luna-service2's private LS::JSONPayload (legacy-webos,
// webOS 3 build). The SubscriptionPoint code only builds {"serviceName": "..."}
// and reads the boolean "connected" from a server-status reply, so this needs
// no pbnjson (webOS 3's libpbnjson_cpp has an older C++ ABI).
#pragma once

#include <cstring>
#include <string>

namespace LS {

class JSONPayload
{
public:
    JSONPayload() : _valid{true} {}

    explicit JSONPayload(const std::string &payload)
        : _text{payload}, _valid{!payload.empty() && payload.find('{') != std::string::npos} {}

    JSONPayload(const JSONPayload &) = delete;
    JSONPayload &operator=(const JSONPayload &) = delete;

    bool isValid() { return _valid; }

    bool set(const std::string &name, const char *value)
    {
        return set(name, std::string{value ? value : ""});
    }

    bool set(const std::string &name, const std::string &value)
    {
        if (!_members.empty())
            _members += ',';
        _members += quote(name) + ':' + quote(value);
        return true;
    }

    // Finds "name" : true|false at any nesting level; enough for
    // {"serviceName": "...", "connected": false}.
    bool get(const std::string &name, bool &value) const
    {
        const std::string key = quote(name);
        for (std::size_t at = _text.find(key); at != std::string::npos;
             at = _text.find(key, at + 1))
        {
            std::size_t p = _text.find_first_not_of(" \t\r\n", at + key.size());
            if (p == std::string::npos || _text[p] != ':')
                continue;
            p = _text.find_first_not_of(" \t\r\n", p + 1);
            if (p == std::string::npos)
                return false;
            if (_text.compare(p, 4, "true") == 0) { value = true; return true; }
            if (_text.compare(p, 5, "false") == 0) { value = false; return true; }
            return false;
        }
        return false;
    }

    std::string getJSONString() const { return '{' + _members + '}'; }

private:
    static std::string quote(const std::string &s)
    {
        std::string out{"\""};
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                }
                else
                    out += static_cast<char>(c);
            }
        }
        return out + '"';
    }

    std::string _text;
    std::string _members;
    bool _valid;
};

} // namespace LS
