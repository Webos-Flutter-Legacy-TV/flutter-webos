// webOS 3 compatibility library (libwebos3-compat.so, COMPAT_WEBOS3=1 only).
//
// LG's embedder (libflutter_webos_media.so) and the plugin libraries are built
// against webOS 26. On webOS 3.x the firmware lacks a few functions they call;
// this library provides them. Built with the LG NDK's g++ so std::string has
// the same (C++11) ABI as LG's binaries. The C++ luna-service2 wrapper is a
// separate library (compat/webos3/luna-service2pp).
//
// Where a later firmware does have a function, the call is forwarded to it, so
// a COMPAT_WEBOS3 package also behaves normally on webOS 4+.

#include <dlfcn.h>
#include <stdio.h>

#include <map>
#include <string>

namespace {

template <typename Fn>
Fn next_symbol(const char *name)
{
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

} // namespace

// ---------------------------------------------------------------------------
// luna-service2 C API additions (newer than webOS 3's libluna-service2 3.17).
// LSError is only passed through, so it stays opaque here.
// ---------------------------------------------------------------------------
struct LSError;
struct LSMessage;

extern "C" {

// Used by LS::Error::logError() (inline in luna-service2++'s error.hpp).
__attribute__((visibility("default")))
void LSErrorLogDefault(const char *message_id, LSError *lserror)
{
    using Fn = void (*)(const char *, LSError *);
    static Fn real = next_symbol<Fn>("LSErrorLogDefault");
    if (real)
        return real(message_id, lserror);
    using PrintFn = void (*)(LSError *, FILE *);
    static PrintFn print = next_symbol<PrintFn>("LSErrorPrint");
    fprintf(stderr, "[compat] luna-service2 error %s: ", message_id ? message_id : "");
    if (print)
        print(lserror, stderr);
    else
        fputc('\n', stderr);
}

// Used by LS::SubscriptionPoint::subscribe() to skip clients that are already
// gone. Without it, assume the client is connected: a later reply to a client
// that has left fails harmlessly, and the bus's server-status signal removes
// the subscription.
__attribute__((visibility("default")))
bool LSMessageIsConnected(LSMessage *msg)
{
    using Fn = bool (*)(LSMessage *);
    static Fn real = next_symbol<Fn>("LSMessageIsConnected");
    return real ? real(msg) : true;
}

// ---------------------------------------------------------------------------
// SQLite additions (webOS 3 ships an older libsqlite3). The plugins load the
// firmware's libsqlite3; these are looked up at call time.
// ---------------------------------------------------------------------------

// sqlite 3.10: write dirty pages early. Skipping it is safe: they are written
// at commit anyway.
__attribute__((visibility("default")))
int sqlite3_db_cacheflush(void *db)
{
    using Fn = int (*)(void *);
    static Fn real = next_symbol<Fn>("sqlite3_db_cacheflush");
    return real ? real(db) : 0 /* SQLITE_OK */;
}

// sqlite 3.14: SQL text with bound parameters expanded; caller frees it with
// sqlite3_free(). Fall back to the statement's SQL text without expansion.
__attribute__((visibility("default")))
char *sqlite3_expanded_sql(void *stmt)
{
    using Fn = char *(*)(void *);
    static Fn real = next_symbol<Fn>("sqlite3_expanded_sql");
    if (real)
        return real(stmt);
    using SqlFn = const char *(*)(void *);
    using PrintfFn = char *(*)(const char *, ...);
    static SqlFn sql = reinterpret_cast<SqlFn>(dlsym(RTLD_DEFAULT, "sqlite3_sql"));
    static PrintfFn mprintf = reinterpret_cast<PrintfFn>(dlsym(RTLD_DEFAULT, "sqlite3_mprintf"));
    const char *text = sql ? sql(stmt) : nullptr;
    return text && mprintf ? mprintf("%s", text) : nullptr;
}

} // extern "C"

// ---------------------------------------------------------------------------
// ResBundle (libwebosi18n). webOS 3's library has the pre-C++11 std::string
// ABI, so the embedder's ResBundle(std::__cxx11::string x3) and
// getLocString(const std::__cxx11::string&) are missing there.
//
// The embedder uses it only for accessibility labels
// (flutter::AccessibilityLocalizationWebOS): it allocates 88 bytes, stores
// the object in a unique_ptr and deletes it through vtable slot 1 (deleting
// destructor), and reads the result of getLocString() as a reference. This
// class keeps that contract and returns the source string untranslated. It
// must stay within 88 bytes (checked below).
// ---------------------------------------------------------------------------
//
// Built with -fvisibility=hidden: only the two functions the embedder imports
// are exported, so the vtable, typeinfo and destructor do not interpose on
// webOS 3's own (old-ABI) ResBundle symbols of the same names.
#define WEBOS3_EXPORT __attribute__((visibility("default")))
class ResBundle
{
public:
    WEBOS3_EXPORT ResBundle(std::string locale, std::string file, std::string resources_path);
    virtual ~ResBundle();
    WEBOS3_EXPORT const std::string &getLocString(const std::string &source);

private:
    std::map<std::string, std::string> _strings;
};

static_assert(sizeof(ResBundle) <= 88, "LG's embedder allocates 88 bytes for ResBundle");

ResBundle::ResBundle(std::string, std::string, std::string) {}

ResBundle::~ResBundle() = default;

const std::string &ResBundle::getLocString(const std::string &source)
{
    return _strings.emplace(source, source).first->second;
}

// ---------------------------------------------------------------------------
// pbnjson C++ (libpbnjson_cpp). webOS 3 has pbnjson 2.11 with the pre-C++11
// std::string ABI, so the two functions the media plugins call with a
// std::__cxx11::string are missing there. Both are rebuilt on pbnjson's C API
// (present on webOS 3), found at call time; where the firmware has the C++11
// versions (webOS 4+) the call is forwarded to them.
//
// Layout (pbnjson's JResult has a virtual destructor and one pointer, the
// same in 2.11 and 2.15): JValue = { vptr, jerror *error, jvalue_ref m_jval },
// JSchema = { vptr, jerror *error, jschema_ref schema }.
// ---------------------------------------------------------------------------
namespace {

struct raw_buffer_compat
{
    const char *m_str;
    long m_len;
};

void *pbnjson_ref_at_8(const void *object)
{
    return *reinterpret_cast<void *const *>(static_cast<const char *>(object) + 8);
}

template <typename Fn>
Fn c_symbol(const char *name)
{
    return reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
}

} // namespace

namespace pbnjson {

class JValue
{
public:
    WEBOS3_EXPORT unsigned int asString(std::string &asStr) const;
};

class JSchema;

class JGenerator
{
public:
    WEBOS3_EXPORT bool toString(const JValue &val, const JSchema &schema, std::string &asStr);
};

// ConversionResultFlags: CONV_OK = 0, CONV_NOT_A_STRING = 0x20.
unsigned int JValue::asString(std::string &asStr) const
{
    using Fn = unsigned int (*)(const JValue *, std::string &);
    static Fn real = next_symbol<Fn>(
        "_ZNK7pbnjson6JValue8asStringERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
    if (real)
        return real(this, asStr);
    using IsStringFn = bool (*)(void *);
    using GetFn = raw_buffer_compat (*)(void *);
    static IsStringFn is_string = c_symbol<IsStringFn>("jis_string");
    static GetFn get_fast = c_symbol<GetFn>("jstring_get_fast");
    void *jval = pbnjson_ref_at_8(this);
    if (!jval || !is_string || !get_fast || !is_string(jval))
        return 0x20;
    raw_buffer_compat buf = get_fast(jval);
    asStr.assign(buf.m_str ? buf.m_str : "", buf.m_str ? static_cast<std::size_t>(buf.m_len) : 0);
    return 0;
}

bool JGenerator::toString(const JValue &val, const JSchema &schema, std::string &asStr)
{
    using Fn = bool (*)(JGenerator *, const JValue &, const JSchema &, std::string &);
    static Fn real = next_symbol<Fn>(
        "_ZN7pbnjson10JGenerator8toStringERKNS_6JValueERKNS_7JSchemaERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
    if (real)
        return real(this, val, schema, asStr);
    using ToStringFn = const char *(*)(void *, void *);
    static ToStringFn tostring = c_symbol<ToStringFn>("jvalue_tostring");
    void *jval = pbnjson_ref_at_8(&val);
    void *jschema = pbnjson_ref_at_8(&schema);
    const char *text = tostring && jval && jschema ? tostring(jval, jschema) : nullptr;
    if (!text)
        return false;
    asStr = text;
    return true;
}

} // namespace pbnjson
