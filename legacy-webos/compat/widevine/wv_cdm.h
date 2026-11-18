// Minimal host for the Widevine CE CDM (v16) shipped on LG webOS TVs as
// /usr/lib/libwidevine_ce_cdm_shared.so. Opt-in, software security level
// (L3): content is decrypted into normal memory.
//
// The CDM is a C++ library with a vtable interface. Only what this host needs
// is declared here: the plain structs passed by value/reference, the four
// callback interfaces the host implements (their virtual order must match the
// library), and the vtable slots of the methods called. Slot numbers and
// layouts were verified on webOS 6.5.3 (CDM 16.3.0).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wvcompat {

// ---- values the CDM uses ---------------------------------------------------
enum Status : int32_t {
  kOk = 0,
  kNeedsDeviceCertificate = 101,
  kSessionNotFound = 102,
  kNoKey = 104,
};
enum : int32_t { kSessionTemporary = 0 };
enum : int32_t { kInitCenc = 0 };
enum : int32_t { kMsgLicenseRequest = 0 };
enum : int32_t { kSecureOutputNone = 2 };
enum : int32_t { kLogErrors = 0 };
enum : int32_t { kSchemeClear = 0, kSchemeCtr = 1, kSchemeCbc = 2 };

// ---- structs (layout fixed by the library) --------------------------------
struct ClientInfo {          // all six must be non-empty for initialize()
  std::string product_name, company_name, device_name, model_name, arch_name, build_info;
};
struct Subsample { uint32_t clear_bytes, protected_bytes; };
struct Sample {              // 36 bytes on 32-bit ARM
  const uint8_t *iv; uint32_t iv_length;           // iv_length 16 when anything is protected
  const uint8_t *data; uint32_t data_length;
  const Subsample *subsamples; uint32_t subsamples_length;
  uint8_t *output; uint32_t output_offset, output_length;
};
struct DecryptionBatch {     // 36 bytes on 32-bit ARM
  const Sample *samples; uint32_t samples_length;
  const uint8_t *key_id; uint32_t key_id_length;
  uint32_t pattern_encrypted, pattern_clear;       // 0/0 for 'cenc'
  bool is_secure;                                  // false: kNoSecureOutput
  int32_t scheme;                                  // kSchemeCtr / kSchemeCbc
  bool is_video;
};

// ---- callback interfaces the host implements (virtual order matters) -----
class TimerClient {          // implemented by the CDM; slot 0
 public:
  virtual void onTimerExpired(void *context) = 0;
 protected:
  virtual ~TimerClient() {}
};
class EventListener {
 public:
  virtual void onMessage(const std::string &session, int32_t type, const std::string &msg) = 0;
  virtual void onKeyStatusesChange(const std::string &session, bool has_new_usable_key) = 0;
  virtual void onRemoveComplete(const std::string &session) = 0;
  virtual void onDeferredComplete(const std::string &session, int32_t status) = 0;
  virtual void onDirectIndividualizationRequest(const std::string &session, const std::string &req) = 0;
 protected:
  virtual ~EventListener() {}
};
class Storage {
 public:
  virtual bool read(const std::string &name, std::string *data) = 0;
  virtual bool write(const std::string &name, const std::string &data) = 0;
  virtual bool exists(const std::string &name) = 0;
  virtual bool remove(const std::string &name) = 0;
  virtual int32_t size(const std::string &name) = 0;
  virtual bool list(std::vector<std::string> *names) = 0;
 protected:
  virtual ~Storage() {}
};
class Clock {
 public:
  virtual int64_t now() = 0;   // epoch milliseconds (device cert carries its creation time)
 protected:
  virtual ~Clock() {}
};
class Timer {
 public:
  virtual void setTimeout(int64_t delay_ms, TimerClient *client, void *context) = 0;
  virtual void cancel(TimerClient *client) = 0;
 protected:
  virtual ~Timer() {}
};

// ---- host ------------------------------------------------------------------
// Loads the CDM and drives one instance. Not thread-safe; call from one thread.
class Host {
 public:
  explicit Host(const std::string &storage_dir);
  ~Host();
  // initialize() + create(); returns false with error() set.
  bool open();
  // Provision the device certificate if needed (HTTPS to Google's endpoint).
  bool ensureProvisioned();
  // New temporary session from a Widevine PSSH box; returns the licence
  // request (challenge) in *challenge.
  bool startSession(const std::string &pssh_box, std::string *session, std::string *challenge);
  bool updateSession(const std::string &session, const std::string &licence);
  bool keyStatuses(const std::string &session, std::map<std::string, int32_t> *keys);
  bool decrypt(const std::string &session, const DecryptionBatch &batch, int32_t *status);
  void closeSession(const std::string &session);
  const std::string &error() const { return error_; }
  const char *version() const;

  // HTTP helpers (libcurl); return the HTTP status, 0 on transport error.
  static long httpPost(const std::string &url, const std::string &body,
                       const std::string &content_type, std::string *response);
  // GET with an optional Range ("a-b"); *content_range gets the response's
  // Content-Range value, *content_type its Content-Type.
  static long httpGet(const std::string &url, const std::string &range, std::string *body,
                      std::string *content_range, std::string *content_type);

 private:
  struct Impl;
  Impl *impl_;
  std::string error_;
};

}  // namespace wvcompat
