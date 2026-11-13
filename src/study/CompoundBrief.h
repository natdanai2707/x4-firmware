#pragma once

#include <string>
#include <vector>

// One Compound brief in the feed's plain text ".cb" format: one record per
// line, fields separated by TAB (see compound-feed tools/compound_epub.py).
struct CompoundBrief {
  struct Metric {
    std::string label, value, change, note;
  };
  struct Section {
    std::string framework, headline, headlineTh, en, th, impact, impactTh;
  };
  struct Term {
    std::string en, th, explain, example;
  };

  std::string type;  // "news" or "concept"
  std::string date;
  std::string title, titleTh, hook, note, core, coreTh;
  std::vector<Metric> metrics;
  std::vector<Section> sections;
  std::vector<Term> terms;
  std::vector<std::string> followups;

  bool isNews() const { return type == "news"; }
  bool empty() const { return title.empty() && sections.empty(); }

  static CompoundBrief parse(const std::string& text);
};
