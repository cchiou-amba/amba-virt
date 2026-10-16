/*
 * UsbWebcamControl.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "UsbWebcamControl.hxx"

#include <errno.h>
#include <stdlib.h>
#include <string>

/* The value of "key" in a flat JSON object: a quoted string without its quotes, or a bare number. */
static bool FindValue(const std::string &text, const char *key, std::string *value)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t at = text.find(needle);

    if (at == std::string::npos)
        return false;
    at = text.find(':', at + needle.size());
    if (at == std::string::npos)
        return false;
    at++;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
        at++;
    if (at >= text.size())
        return false;
    if (text[at] == '"') {
        size_t end = text.find('"', at + 1);

        if (end == std::string::npos)
            return false;
        *value = text.substr(at + 1, end - at - 1);
        return true;
    }
    size_t end = at;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ' ' && text[end] != '\n')
        end++;
    *value = text.substr(at, end - at);
    return !value->empty();
}

static bool ParseNumber(const std::string &s, int *out)
{
    char *end = nullptr;
    long v;

    errno = 0;
    v = strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0' || v < 0 || v > 100000)
        return false;
    *out = static_cast<int>(v);
    return true;
}

int UsbWebcamControl::parseBackend(const char *body, size_t len, int *out)
{
    std::string text(body ? body : "", body ? len : 0), v;

    if (!out || !FindValue(text, "backend", &v))
        return -EINVAL;
    if (v == "hw" || v == "hardware") {
        *out = BackendHardware;
        return 0;
    }
    if (v == "cpu" || v == "software") {
        *out = BackendSoftware;
        return 0;
    }
    return -EINVAL;
}

int UsbWebcamControl::parseExposure(const char *body, size_t len, Exposure *out)
{
    std::string text(body ? body : "", body ? len : 0), v;
    Exposure e{ExposureAuto, -1, -1};

    if (!out || !FindValue(text, "mode", &v))
        return -EINVAL;
    if (v == "auto") {
        e.mode = ExposureAuto;
    } else if (v == "fixed") {
        e.mode = ExposureFixed;
    } else {
        return -EINVAL;
    }
    if (FindValue(text, "exposure", &v) && !ParseNumber(v, &e.exposureAbs))
        return -EINVAL;
    if (FindValue(text, "gain", &v) && !ParseNumber(v, &e.gain))
        return -EINVAL;
    *out = e;
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
