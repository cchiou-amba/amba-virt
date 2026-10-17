/*
 * DumpDashboardHtml.cxx
 *
 * Host test helper that writes HTML_TEMPLATE to stdout for JavaScript syntax
 * and viewer-profile verification.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <cstdio>
#include <cstring>

#include "../DashboardHtml.hxx"

int main()
{
    size_t len = std::strlen(HTML_TEMPLATE);
    size_t written = std::fwrite(HTML_TEMPLATE, 1, len, stdout);
    if (written != len) {
        return 1;
    }
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
