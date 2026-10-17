/*
 * TestDashboardHtml.cxx
 *
 * Unit tests for DashboardHtml.hxx covering the Guest Properties first tab,
 * DOM/XSS invariants, and responsive mobile CSS / viewer classification.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"

#include <cstring>
#include <string>
#include <vector>

#include "../DashboardHtml.hxx"

TEST_GROUP(DashboardHtml)
{
    std::string html;

    void setup() override
    {
        html = HTML_TEMPLATE ? HTML_TEMPLATE : "";
    }

    void teardown() override
    {
    }
};

TEST(DashboardHtml, TabOrderAndInitialActiveTab)
{
    size_t posProps = html.find("id=\"tabPropsBtn\"");
    size_t posLive = html.find("id=\"tabLiveBtn\"");
    size_t posUsb = html.find("id=\"tabUsbBtn\"");
    size_t posUpload = html.find("id=\"tabUploadBtn\"");

    CHECK(posProps != std::string::npos);
    CHECK(posLive != std::string::npos);
    CHECK(posUsb != std::string::npos);
    CHECK(posUpload != std::string::npos);

    /* tabPropsBtn appears before every other tab-btn */
    CHECK(posProps < posLive);
    CHECK(posLive < posUsb);
    CHECK(posUsb < posUpload);

    /* No tab-btn appears before tabPropsBtn */
    size_t firstTabBtn = html.find("class=\"tab-btn");
    CHECK(firstTabBtn != std::string::npos);
    size_t propsButtonStart = html.rfind("<button", posProps);
    size_t firstTabButtonStart = html.rfind("<button", firstTabBtn);
    LONGS_EQUAL(propsButtonStart, firstTabButtonStart);

    /* tabPropsBtn is the only tab-btn with the active class */
    size_t propsTagEnd = html.find('>', posProps);
    std::string propsOpeningTag = html.substr(propsButtonStart, propsTagEnd - propsButtonStart + 1);
    CHECK(propsOpeningTag.find("tab-btn active") != std::string::npos);

    const char *otherTabIds[] = {
        "id=\"tabLiveBtn\"",
        "id=\"tabUsbBtn\"",
        "id=\"tabUploadBtn\""
    };
    for (const char *tabId : otherTabIds) {
        size_t pos = html.find(tabId);
        CHECK(pos != std::string::npos);
        size_t btnStart = html.rfind("<button", pos);
        size_t btnEnd = html.find('>', pos);
        std::string tag = html.substr(btnStart, btnEnd - btnStart + 1);
        CHECK(tag.find("tab-btn") != std::string::npos);
        CHECK(tag.find("active") == std::string::npos);
    }
}

TEST(DashboardHtml, PropsViewAndGuardedStartupStream)
{
    CHECK(html.find("id=\"propsView\"") != std::string::npos);
    CHECK(html.find("id=\"streamMain\"") != std::string::npos);
    CHECK(html.find("<main id=\"streamMain\" style=\"display:none;\">") != std::string::npos);

    /* Required Guest Properties DOM elements and negative-constraint labels */
    const char *requiredIds[] = {
        "id=\"propsRefreshBtn\"",
        "id=\"propsErrorCard\"",
        "id=\"propsErrorText\"",
        "id=\"propsOnlineContent\"",
        "id=\"propTenantCid\"",
        "id=\"propTenantIdx\"",
        "id=\"propServerReachable\"",
        "id=\"propBarSize\"",
        "id=\"propPartitionUsage\"",
        "id=\"propPartitionGauge\"",
        "id=\"propServerAllocMb\"",
        "id=\"propCapsHex\"",
        "id=\"propCapsList\"",
        "id=\"propMapBar\"",
        "id=\"propDevicesBody\"",
        "id=\"propExtentsBody\"",
        "id=\"propLayoutText\""
    };
    for (const char *idStr : requiredIds) {
        CHECK_TEXT(html.find(idStr) != std::string::npos, idStr);
    }

    CHECK(html.find("Allocated by server (incl. MEM_ALLOC)") != std::string::npos);
    CHECK(html.find("ONLINE (Server reachable)") != std::string::npos);
    CHECK(html.find("fetch('/api/guest-props')") != std::string::npos);
    CHECK(html.find("let activeTab = 'props';") != std::string::npos);

    /* Page-load startStream() is guarded by the active tab */
    size_t fetchInfoCall = html.find("fetchInfo();");
    CHECK(fetchInfoCall != std::string::npos);
    std::string startupTail = html.substr(fetchInfoCall);
    size_t guardPos = startupTail.find("if (activeTab === 'live' || activeTab === 'usb')");
    size_t streamCallPos = startupTail.find("startStream();");
    CHECK(guardPos != std::string::npos);
    CHECK(streamCallPos != std::string::npos);
    CHECK(guardPos < streamCallPos);
}

TEST(DashboardHtml, NoInnerHtmlTakesGuestPropsApiFields)
{
    /* 1. renderGuestProps must not contain innerHTML at all */
    size_t renderStart = html.find("function renderGuestProps(");
    size_t fetchStart = html.find("async function fetchGuestProps(");
    CHECK(renderStart != std::string::npos);
    CHECK(fetchStart != std::string::npos);
    CHECK(renderStart < fetchStart);

    std::string renderBody = html.substr(renderStart, fetchStart - renderStart);
    CHECK(renderBody.find("innerHTML") == std::string::npos);
    CHECK(renderBody.find("textContent") != std::string::npos);
    CHECK(renderBody.find("createElement") != std::string::npos);

    /* 2. Across the entire HTML, no innerHTML statement references guest-props fields */
    const std::vector<std::string> forbiddenNearInnerHtml = {
        "props.",
        "d.usage",
        "ext.usage",
        "dev.dev_type_name",
        "data.error",
        "data.tenant_cid",
        "data.tenant_idx",
        "data.cap_names",
        "data.caps_hex",
        "data.caps_unknown_hex",
        "data.bar_size_bytes",
        "data.partition_bytes",
        "data.server_allocated_mb",
        "data.devices",
        "data.extents",
        "data.layout_text"
    };

    size_t searchPos = 0;
    while ((searchPos = html.find("innerHTML", searchPos)) != std::string::npos) {
        size_t windowStart = (searchPos > 120) ? (searchPos - 120) : 0;
        size_t windowLen = std::min<size_t>(280, html.size() - windowStart);
        std::string snippet = html.substr(windowStart, windowLen);
        for (const std::string &token : forbiddenNearInnerHtml) {
            CHECK_TEXT(snippet.find(token) == std::string::npos, token.c_str());
        }
        searchPos += 9;
    }
}

TEST(DashboardHtml, ResponsiveMobileCssAndViewerProfile)
{
    /* Viewport meta with viewport-fit=cover */
    CHECK(html.find("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, viewport-fit=cover\">") != std::string::npos);

    /* Media queries for 900px, 600px, and coarse pointer */
    size_t mq900 = html.find("@media (max-width: 900px)");
    size_t mq600 = html.find("@media (max-width: 600px)");
    size_t mqCoarse = html.find("@media (pointer: coarse)");
    CHECK(mq900 != std::string::npos);
    CHECK(mq600 != std::string::npos);
    CHECK(mqCoarse != std::string::npos);
    CHECK(mq900 < mq600);

    /* Narrow main is one column and viewport-card appears before controls */
    std::string mq900Block = html.substr(mq900, mq600 - mq900);
    CHECK(mq900Block.find("grid-template-columns: 1fr;") != std::string::npos);
    CHECK(mq900Block.find("order: -1;") != std::string::npos);
    CHECK(mq900Block.find("min-height: 0;") != std::string::npos);

    /* 600px breakpoint collapses summary cards to 1fr and uses 0.65rem side padding */
    std::string mq600Block = html.substr(mq600);
    CHECK(mq600Block.find("0.65rem") != std::string::npos);
    CHECK(mq600Block.find("grid-template-columns: 1fr;") != std::string::npos);
    CHECK(mq600Block.find("grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);") != std::string::npos);

    /* Page-level horizontal overflow prevented; tab bar, table, and map wrappers scroll horizontally */
    CHECK(html.find("overflow-x: hidden;") != std::string::npos);
    CHECK(html.find(".tab-bar") != std::string::npos);
    CHECK(html.find(".table-scroll-wrap") != std::string::npos);
    CHECK(html.find(".map-scroll-wrap") != std::string::npos);
    CHECK(html.find("overflow-x: auto;") != std::string::npos);

    /* Touch targets >= 44px and range thumbs >= 24px */
    CHECK(html.find("min-height: 44px;") != std::string::npos);
    CHECK(html.find("min-width: 44px;") != std::string::npos);
    CHECK(html.find("width: 24px;") != std::string::npos);
    CHECK(html.find("height: 24px;") != std::string::npos);

    /* iOS safe-area insets */
    CHECK(html.find("env(safe-area-inset-top)") != std::string::npos);
    CHECK(html.find("env(safe-area-inset-right)") != std::string::npos);
    CHECK(html.find("env(safe-area-inset-bottom)") != std::string::npos);
    CHECK(html.find("env(safe-area-inset-left)") != std::string::npos);

    /* Canvases are width-bounded with automatic height and 16:9 container */
    CHECK(html.find("aspect-ratio: 16 / 9;") != std::string::npos);
    size_t liveCanvasCss = html.find("#liveCanvas {");
    size_t uploadCanvasCss = html.find("#uploadCanvas {");
    CHECK(liveCanvasCss != std::string::npos);
    CHECK(uploadCanvasCss != std::string::npos);
    std::string liveCssBlock = html.substr(liveCanvasCss, 220);
    std::string uploadCssBlock = html.substr(uploadCanvasCss, 220);
    CHECK(liveCssBlock.find("max-width: 100%;") != std::string::npos);
    CHECK(liveCssBlock.find("height: auto;") != std::string::npos);
    CHECK(uploadCssBlock.find("max-width: 100%;") != std::string::npos);
    CHECK(uploadCssBlock.find("height: auto;") != std::string::npos);

    /* Pure viewer classification function and class toggles */
    CHECK(html.find("function detectViewerProfile(navigatorLike, matchMediaFn)") != std::string::npos);
    CHECK(html.find("function applyViewerProfile()") != std::string::npos);
    CHECK(html.find("platform-android") != std::string::npos);
    CHECK(html.find("platform-ios") != std::string::npos);
    CHECK(html.find("touch-view") != std::string::npos);
    CHECK(html.find("mobile-view") != std::string::npos);
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
