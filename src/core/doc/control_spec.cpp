#include "core/doc/control_spec.h"

namespace heditor {

const char* valueShapeName(ValueShape shape) {
    switch (shape) {
        case ValueShape::Text:
            return "text";
        case ValueShape::Bool:
            return "bool";
        case ValueShape::Enum:
            return "enum";
        case ValueShape::List:
            return "list";
        case ValueShape::Resource:
            return "resource";
    }
    return "（未知）";
}

const char* controlPropName(ControlProp prop) {
    switch (prop) {
        case ControlProp::Hidden:
            return "hidden";
        case ControlProp::Readonly:
            return "readonly";
        case ControlProp::Required:
            return "required";
        case ControlProp::StructureLock:
            return "structureLock";
    }
    return "（未知）";
}

const char* overflowName(Overflow o) {
    switch (o) {
        case Overflow::Expand:
            return "expand";
        case Overflow::Clip:
            return "clip";
        case Overflow::Spill:
            return "spill";
    }
    return "（未知）";
}

const char* underlineName(Underline u) {
    switch (u) {
        case Underline::None:
            return "none";
        case Underline::Whole:
            return "whole";
        case Underline::BlankOnly:
            return "blankOnly";
    }
    return "（未知）";
}

const char* alignName(Align a) {
    switch (a) {
        case Align::Start:
            return "start";
        case Align::Center:
            return "center";
        case Align::End:
            return "end";
    }
    return "（未知）";
}

const char* borderStyleName(BorderStyle s) {
    switch (s) {
        case BorderStyle::Solid:
            return "solid";
        case BorderStyle::Dashed:
            return "dashed";
        case BorderStyle::Dotted:
            return "dotted";
    }
    return "（未知）";
}

bool borderStyleFromName(const std::string& name, BorderStyle* out) {
    if (out == nullptr) return false;
    const BorderStyle all[] = {BorderStyle::Solid, BorderStyle::Dashed, BorderStyle::Dotted};
    for (BorderStyle s : all) {
        if (name == borderStyleName(s)) {
            *out = s;
            return true;
        }
    }
    return false;
}

bool valueShapeFromName(const std::string& name, ValueShape* out) {
    if (out == nullptr) return false;
    const ValueShape all[] = {ValueShape::Text, ValueShape::Bool, ValueShape::Enum,
                              ValueShape::List, ValueShape::Resource};
    for (ValueShape s : all) {
        if (name == valueShapeName(s)) {
            *out = s;
            return true;
        }
    }
    return false;
}

bool overflowFromName(const std::string& name, Overflow* out) {
    if (out == nullptr) return false;
    const Overflow all[] = {Overflow::Expand, Overflow::Clip, Overflow::Spill};
    for (Overflow o : all) {
        if (name == overflowName(o)) {
            *out = o;
            return true;
        }
    }
    return false;
}

bool underlineFromName(const std::string& name, Underline* out) {
    if (out == nullptr) return false;
    const Underline all[] = {Underline::None, Underline::Whole, Underline::BlankOnly};
    for (Underline u : all) {
        if (name == underlineName(u)) {
            *out = u;
            return true;
        }
    }
    return false;
}

bool alignFromName(const std::string& name, Align* out) {
    if (out == nullptr) return false;
    const Align all[] = {Align::Start, Align::Center, Align::End};
    for (Align a : all) {
        if (name == alignName(a)) {
            *out = a;
            return true;
        }
    }
    return false;
}

}  // namespace heditor
