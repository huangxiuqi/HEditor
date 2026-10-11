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

}  // namespace heditor
