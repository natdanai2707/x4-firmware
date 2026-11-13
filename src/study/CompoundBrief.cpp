#include "CompoundBrief.h"

namespace {
std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos) break;
    start = tab + 1;
  }
  return out;
}

const std::string& at(const std::vector<std::string>& f, const size_t i) {
  static const std::string none;
  return i < f.size() ? f[i] : none;
}
}  // namespace

CompoundBrief CompoundBrief::parse(const std::string& text) {
  CompoundBrief b;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto f = splitTabs(line);
    const std::string& key = f[0];
    if (key == "TYPE") {
      b.type = at(f, 1);
    } else if (key == "DATE") {
      b.date = at(f, 1);
    } else if (key == "TITLE") {
      b.title = at(f, 1);
      b.titleTh = at(f, 2);
    } else if (key == "HOOK") {
      b.hook = at(f, 1);
    } else if (key == "NOTE") {
      b.note = at(f, 1);
    } else if (key == "M") {
      b.metrics.push_back({at(f, 1), at(f, 2), at(f, 3), at(f, 4)});
    } else if (key == "CORE") {
      b.core = at(f, 1);
      b.coreTh = at(f, 2);
    } else if (key == "S") {
      Section s;
      s.framework = at(f, 1);
      s.headline = at(f, 2);
      s.headlineTh = at(f, 3);
      b.sections.push_back(std::move(s));
    } else if (key == "EN" && !b.sections.empty()) {
      b.sections.back().en = at(f, 1);
    } else if (key == "TH" && !b.sections.empty()) {
      b.sections.back().th = at(f, 1);
    } else if (key == "IMP" && !b.sections.empty()) {
      b.sections.back().impact = at(f, 1);
      b.sections.back().impactTh = at(f, 2);
    } else if (key == "W") {
      b.terms.push_back({at(f, 1), at(f, 2), at(f, 3), at(f, 4)});
    } else if (key == "Q") {
      b.followups.push_back(at(f, 1));
    }
    // Unknown records are skipped so the feed can add fields later.
  }
  return b;
}
