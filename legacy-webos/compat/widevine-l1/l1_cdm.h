// Host for the TV's Widevine CDM wrapper (/usr/lib/libwvcdm_shared.so)
// through the Chromium CDM interface, version 10 (webOS variant: Decrypt()
// takes a stream type). Initialized for hardware secure codecs: video
// samples come back wrapped for LG's secure video path (an "ibpi" in-band
// header in front of the still-encrypted data), audio comes back clear.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wvl1 {

struct CdmSample {
  const uint8_t *data;
  uint32_t size;
  bool encrypted;
  bool cbcs;
  const std::string *kid;     // 16 bytes
  const uint8_t *iv;          // 16 bytes
  std::vector<std::pair<uint32_t, uint32_t>> subsamples;  // clear, cipher
  uint8_t crypt_blocks, skip_blocks;
  int64_t timestamp_us;
};

class Cdm {
 public:
  Cdm();
  ~Cdm();
  bool open();
  // Service certificate as the licence server returns it (SignedMessage or
  // the bare certificate).
  bool setServiceCertificate(const std::string &cert);
  bool generateRequest(const std::string &pssh, std::string *challenge);
  bool update(const std::string &licence);
  // Video: *out gets the secure-path buffer. Audio: *out gets clear data.
  bool decrypt(const CdmSample &s, bool video, std::vector<uint8_t> *out);
  const std::string &error() const { return error_; }
  const char *version() const;

  struct Impl;  // public so the CDM host callback can reach it

 private:
  Impl *impl_;
  std::string error_;
};

std::string signedMessageBody(const std::string &in);

}  // namespace wvl1
