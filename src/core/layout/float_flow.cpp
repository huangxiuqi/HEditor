#include "core/layout/float_flow.h"

#include <algorithm>
#include <cmath>

namespace heditor {

void FloatFlow::shiftFrom(size_t begin, float dy) {
    for (size_t i = begin; i < bands.size(); ++i) {
        bands[i].f0 += dy;
        bands[i].f1 += dy;
    }
}

void FloatFlow::prune(float at_flow_pt) {
    if (bands.empty()) return;
    std::vector<FloatBand> keep;
    keep.reserve(bands.size());
    for (const FloatBand& band : bands) {
        if (band.f1 < at_flow_pt) continue;
        keep.push_back(band);
    }
    bands.swap(keep);
}

float floatFlowY(float page_stride_pt, bool continuous, int page, float local_y_pt) {
    if (continuous) return local_y_pt;
    return static_cast<float>(page) * page_stride_pt + local_y_pt;
}

int floatFlowPage(float page_stride_pt, float flow_y_pt, int fallback_page) {
    if (!(page_stride_pt > 0.0f)) return fallback_page;
    return std::max(0, static_cast<int>(std::floor(flow_y_pt / page_stride_pt)));
}

float floatFlowLocal(float page_stride_pt, int page, float flow_y_pt) {
    return flow_y_pt - static_cast<float>(page) * page_stride_pt;
}

bool floatBandOverlapsLine(const FloatBand& band, float flow_y_pt, float line_h_pt) {
    return !(flow_y_pt + 0.5f >= band.f1 || flow_y_pt + line_h_pt <= band.f0);
}

void pushLineBelowFloats(const FloatFlow& flow, float line_h_pt, float* flow_y_pt) {
    if (flow_y_pt == nullptr) return;
    for (int guard = 0; guard < 4; ++guard) {
        bool pushed = false;
        for (const FloatBand& band : flow.bands) {
            if (band.square) continue;
            if (!floatBandOverlapsLine(band, *flow_y_pt, line_h_pt)) continue;
            *flow_y_pt = band.f1;
            pushed = true;
        }
        if (!pushed) break;
    }
}

float nextSquareFloatBottom(const FloatFlow& flow, float flow_y_pt, float line_h_pt) {
    float next = flow_y_pt;
    for (const FloatBand& band : flow.bands) {
        if (!band.square) continue;
        if (!floatBandOverlapsLine(band, flow_y_pt, line_h_pt)) continue;
        next = std::max(next, band.f1);
    }
    return next;
}

void cutSpanRange(std::vector<LineSpan>* spans, float lo_pt, float hi_pt) {
    if (spans == nullptr) return;
    std::vector<LineSpan> keep;
    keep.reserve(spans->size() + 1);
    for (const LineSpan& s : *spans) {
        const float a = s.x;
        const float b = s.x + s.w;
        if (hi_pt <= a || lo_pt >= b) {
            keep.push_back(s);
            continue;
        }
        if (lo_pt > a) keep.push_back(LineSpan{a, std::min(lo_pt, b) - a});
        if (hi_pt < b) keep.push_back(LineSpan{std::max(hi_pt, a), b - std::max(hi_pt, a)});
    }
    spans->swap(keep);
}

void cutSpansAroundFloats(const FloatFlow& flow, float flow_y_pt, float line_h_pt, float gap_pt,
                          std::vector<LineSpan>* spans) {
    if (spans == nullptr) return;
    for (const FloatBand& band : flow.bands) {
        if (!band.square) continue;
        if (!floatBandOverlapsLine(band, flow_y_pt, line_h_pt)) continue;
        cutSpanRange(spans, band.x0 - gap_pt, band.x1 + gap_pt);
    }
}

}  // namespace heditor
