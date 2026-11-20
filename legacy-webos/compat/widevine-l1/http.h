// HTTP for libwv-l1 through the TV's own libcurl (loaded at run time).
#pragma once

#include <string>

namespace wvl1 {
namespace http {

// POST; returns the HTTP status, 0 on transport error.
long post(const std::string &url, const std::string &body, const std::string &content_type, std::string *response);
// GET with an optional Range ("a-b"); *content_range gets the response's
// Content-Range value, *content_type its Content-Type (either may be null).
long get(const std::string &url, const std::string &range, std::string *body, std::string *content_range,
         std::string *content_type);

}  // namespace http
}  // namespace wvl1
