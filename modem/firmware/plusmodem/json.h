// Plus Modem: just enough JSON for the model APIs: read values by path from one small SSE event,
// and write escaped strings for the request body.
#pragma once
#include <stddef.h>
#include <string>

struct JVal {
    char type;                     // 's' string, 'n' number, 'o' object, 'a' array, 't' true, 'f' false, 'z' null; 0 missing
    const char *p;                 // the value's text (strings: between the quotes, still escaped)
    size_t n;
};

// path: keys and array indexes separated by dots, e.g. "choices.0.delta.content". "" = the whole text.
JVal jget(const char *json, size_t n, const char *path);
inline JVal jget(const std::string &json, const char *path) { return jget(json.data(), json.size(), path); }
std::string jstr(const JVal &v);                         // a decoded string (UTF-8), "" if not a string
long jnum(const JVal &v);                                // a number, 0 if not a number
inline std::string jstr(const std::string &json, const char *path) { return jstr(jget(json, path)); }
inline long jnum(const std::string &json, const char *path) { return jnum(jget(json, path)); }

void jquote(std::string &out, const std::string &utf8);  // appends "..." with JSON escapes
