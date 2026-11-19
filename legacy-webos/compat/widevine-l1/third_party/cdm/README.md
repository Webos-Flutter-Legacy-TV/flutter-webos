# Chromium CDM interface headers

`content_decryption_module.h` and `content_decryption_module_export.h` are
the Chromium Content Decryption Module interface, as carried (with the
webOS changes behind `TARGET_WEBOS`: module version 10 and a `stream_type`
argument to `Decrypt`) in Kodi's inputstream.adaptive,
https://github.com/xbmc/inputstream.adaptive, commit 81fd9be (2026-11-16),
`lib/cdm/cdm/media/cdm/api/`. They are unchanged.

Copyright (C) 2012, 2017 The Chromium Authors. BSD-3-Clause (LICENSE).
