#include "core/rev/step_map.h"

#include <algorithm>
#include <variant>

namespace heditor {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

FlowPos mapPos(const Step& step, FlowPos pos, Assoc assoc) {
    return std::visit(Overloaded{
                          [&](const InsertText& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.cps.size());
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + n;
                              return assoc == Assoc::After ? s.at + n : s.at;
                          },
                          [&](const InsertFragment& s) -> FlowPos {
                              const FlowPos n = s.content.span();
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + n;
                              return assoc == Assoc::After ? s.at + n : s.at;
                          },
                          [&](const DeleteRange& s) -> FlowPos {
                              if (pos <= s.begin) return pos;
                              if (pos >= s.end) return pos - (s.end - s.begin);
                              return s.begin;
                          },
                          [&](const InsertParaMark& s) -> FlowPos {
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + 1;
                              return assoc == Assoc::After ? s.at + 1 : s.at;
                          },
                          [&](const RemoveParaMark& s) -> FlowPos {
                              if (pos <= s.at) return pos;
                              if (pos >= s.at + 1) return pos - 1;
                              return s.at;
                          },
                          [&](const InsertNode& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.subtree.span());
                              if (pos < s.at_flow) return pos;
                              if (pos > s.at_flow) return pos + n;
                              return assoc == Assoc::After ? s.at_flow + n : s.at_flow;
                          },
                          [&](const RemoveNode& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.removed.span());
                              if (pos <= s.at_flow) return pos;
                              if (pos >= s.at_flow + n) return pos - n;
                              return s.at_flow;
                          },
                          [&](const UnwrapNode& s) -> FlowPos {
                              const FlowPos shrink = s.span_before - s.span_after;
                              if (pos <= s.at_flow) return pos;
                              if (pos >= s.at_flow + s.span_before) return pos - shrink;
                              return s.at_flow +
                                     std::min(pos - s.at_flow, static_cast<FlowPos>(s.span_after));
                          },
                          [&](const WrapNode& s) -> FlowPos {
                              const FlowPos grow = s.span_before - s.span_after;
                              if (pos <= s.at_flow) return pos;
                              if (pos >= s.at_flow + s.span_after) return pos + grow;
                              return pos + 1;
                          },
                          [&](const SetControlProp&) -> FlowPos { return pos; },
                          [&](const SetControlValue&) -> FlowPos { return pos; },
                          [&](const SetControlSpec&) -> FlowPos { return pos; },
                          [&](const SetCellSpec&) -> FlowPos { return pos; },
                          [&](const SetRowSpec&) -> FlowPos { return pos; },
                          [&](const SetTableSpec&) -> FlowPos { return pos; },
                          [&](const SetImageSize&) -> FlowPos { return pos; },
                          [&](const SetSectionSpec&) -> FlowPos { return pos; },
                          [&](const SetImageSpec&) -> FlowPos { return pos; },
                          [&](const SetParaMarkFormat&) -> FlowPos { return pos; },
                          [&](const SetRun&) -> FlowPos { return pos; },
                      },
                      step);
}

}  // namespace heditor
