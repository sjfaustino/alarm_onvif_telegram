#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include <map>
#include <string>
#include <vector>
#include "webserver_html.h" // FormParams

// Test helper, not used by the firmware: reads rendered form HTML and submits
// it the way a browser would. Unchecked boxes send nothing; everything else
// sends its unescaped value.
class BrowserSubmission : public FormParams {
 public:
  explicit BrowserSubmission(const String& html) { read(html.c_str()); }

  bool has(const char* name) const override { return values_.count(name) > 0; }
  String get(const char* name, const char* fallback) const override {
    auto it = values_.find(name);
    return String(it == values_.end() ? fallback : it->second.c_str());
  }
  // Names that appeared more than once; a browser would send them all.
  const std::vector<std::string>& duplicates() const { return duplicates_; }

 private:
  static std::string unescape(std::string s) {
    static const std::pair<const char*, const char*> kEntities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}};  // &amp; last
    for (auto& e : kEntities) {
      std::string from = e.first;
      for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + 1)) s.replace(p, from.size(), e.second);
    }
    return s;
  }

  static bool attr(const std::string& tag, const char* name, std::string& out) {
    std::string key = std::string(" ") + name + "=\"";
    size_t a = tag.find(key);
    if (a == std::string::npos) return false;
    a += key.size();
    out = unescape(tag.substr(a, tag.find('"', a) - a));
    return true;
  }

  void add(const std::string& name, const std::string& value) {
    if (seen_[name]++) duplicates_.push_back(name);
    values_[name] = value;
  }

  void read(const std::string& html) {
    for (size_t p = html.find("<input "); p != std::string::npos; p = html.find("<input ", p + 1)) {
      std::string tag = html.substr(p, html.find('>', p) - p);
      std::string name, type, value;
      if (!attr(tag, "name", name)) continue;
      attr(tag, "type", type);
      seen_[name];
      if (type == "checkbox") {
        if (tag.find(" checked") != std::string::npos) add(name, "on");
        else if (seen_[name]++) duplicates_.push_back(name);
        continue;
      }
      attr(tag, "value", value);
      add(name, value);
    }
    for (size_t p = html.find("<select "); p != std::string::npos; p = html.find("<select ", p + 1)) {
      std::string tag = html.substr(p, html.find('>', p) - p);
      std::string name, first, chosen;
      if (!attr(tag, "name", name)) continue;
      size_t end = html.find("</select>", p);
      for (size_t o = html.find("<option ", p); o != std::string::npos && o < end; o = html.find("<option ", o + 1)) {
        std::string opt = html.substr(o, html.find('>', o) - o);
        std::string value;
        attr(opt, "value", value);
        if (first.empty()) first = value;
        if (opt.find(" selected") != std::string::npos) chosen = value;
      }
      add(name, chosen.empty() ? first : chosen);
    }
  }

  std::map<std::string, std::string> values_;
  std::map<std::string, int> seen_;
  std::vector<std::string> duplicates_;
};
