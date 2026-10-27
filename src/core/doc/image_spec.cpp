/**
 * 图片记录的名字互换（声明与理由见 image_spec.h）。
 */
#include "core/doc/image_spec.h"

namespace heditor {

const char* imageAnchorName(ImageAnchor a) {
    switch (a) {
        case ImageAnchor::Inline:
            return "inline";
        case ImageAnchor::Float:
            return "float";
    }
    return "inline";
}

const char* imageWrapName(ImageWrap w) {
    switch (w) {
        case ImageWrap::None:
            return "none";
        case ImageWrap::Square:
            return "square";
        case ImageWrap::Behind:
            return "behind";
        case ImageWrap::InFront:
            return "inFront";
    }
    return "none";
}

bool imageAnchorFromName(const std::string& name, ImageAnchor* out) {
    if (out == nullptr) return false;
    if (name == "inline") {
        *out = ImageAnchor::Inline;
        return true;
    }
    if (name == "float") {
        *out = ImageAnchor::Float;
        return true;
    }
    return false;
}

bool imageWrapFromName(const std::string& name, ImageWrap* out) {
    if (out == nullptr) return false;
    if (name == "none") {
        *out = ImageWrap::None;
        return true;
    }
    if (name == "square") {
        *out = ImageWrap::Square;
        return true;
    }
    if (name == "behind") {
        *out = ImageWrap::Behind;
        return true;
    }
    if (name == "inFront") {
        *out = ImageWrap::InFront;
        return true;
    }
    return false;
}

}  // namespace heditor
