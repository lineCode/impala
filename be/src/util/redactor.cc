// Copyright 2015 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "redactor.h"

#include <cerrno>
#include <cstring>  // strcmp
#include <ostream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#include <gutil/strings/substitute.h>
#include <rapidjson/document.h>
#include <rapidjson/filestream.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/reader.h>
#include <re2/re2.h>

#include "common/logging.h"

namespace impala {

using rapidjson::Document;
using rapidjson::Value;
using std::endl;
using std::map;
using std::ostream;
using std::ostringstream;
using std::string;
using std::vector;
using strings::Substitute;

typedef re2::RE2 Regex;

struct Rule {
  // Standard constructor.
  Rule(const string& trigger, const string& search_regex, const string& replacement)
      : trigger(trigger),
        search_pattern(search_regex),
        replacement(replacement) {}

  // For use with vector.
  Rule(const Rule& other)
      : trigger(other.trigger),
        search_pattern(other.search_pattern.pattern()),
        replacement(other.replacement) {}

  const string trigger;
  const Regex search_pattern;
  const string replacement;

  // For use with vector.
  const Rule& operator=(const Rule& other) {
    *this = Rule(other);
    return *this;
  }
};

typedef vector<Rule> Rules;

// The actual rules in effect, if any.
static Rules* g_rules;

string NameOfTypeOfJsonValue(const Value& value) {
  switch (value.GetType()) {
    case rapidjson::kNullType:
      return "Null";
    case rapidjson::kFalseType:
    case rapidjson::kTrueType:
      return "Bool";
    case rapidjson::kObjectType:
      return "Object";
    case rapidjson::kArrayType:
      return "Array";
    case rapidjson::kStringType:
      return "String";
    case rapidjson::kNumberType:
      if (value.IsInt()) return "Integer";
      if (value.IsDouble()) return "Float";
    default:
      DCHECK(false);
      return "Unknown";
  }
}

// This class will parse a version 1 rule file and populate g_rules.
class RulesParser {
 public:
  // Perform the parsing and populate g_rules. Any errors encountered will be written
  // to error_message.
  string Parse(const Document& rules_doc) {
    error_message_.str("");
    bool found_rules = false;
    for (Value::ConstMemberIterator member = rules_doc.MemberBegin();
        member != rules_doc.MemberEnd(); ++member) {
      if (strcmp("rules", member->name.GetString()) == 0) {
        found_rules = true;
        ParseRules(member->value);
      } else if (strcmp("version", member->name.GetString()) == 0) {
        // Ignore
      } else {
        AddDocParseError()
            << "unexpected property '" << member->name.GetString() << "' must be removed";
      }
    }
    if (!found_rules) {
      AddDocParseError() << "an array of rules is required";
    }
    return error_message_.str();
  }

 private:
  ostringstream error_message_;

  // The current index of the rule being parsed.
  int rule_idx_;

  // Parse an array of rules.
  void ParseRules(const Value& rules) {
    if (!rules.IsArray()) {
      AddDocParseError() << "'rules' must be of type Array but is a "
                         << NameOfTypeOfJsonValue(rules);
      return;
    }
    for (rule_idx_ = 0; rule_idx_ < rules.Size(); ++rule_idx_) {
      const Value& rule = rules[rule_idx_];
      if (!rule.IsObject()) {
        AddRuleParseError() << "rule should be a JSON Object but is a "
                            << NameOfTypeOfJsonValue(rule);
        continue;
      }
      ParseRule(rule);
    }
  }

  // Parse a rule and populate g_rules.
  void ParseRule(const Value& json_rule) {
    bool found_replace = false;
    string search_text, replace, trigger;
    for (Value::ConstMemberIterator member = json_rule.MemberBegin();
        member != json_rule.MemberEnd(); ++member) {
      if (strcmp("search", member->name.GetString()) == 0) {
        if (!ReadRuleProperty("search", json_rule, &search_text)) return;
        if (search_text.empty()) {
          AddRuleParseError() << "search property must be a non-empty regex";
          return;
        }
      } else if (strcmp("replace", member->name.GetString()) == 0) {
        found_replace = true;
        if (!ReadRuleProperty("replace", json_rule, &replace)) return;
      } else if (strcmp("trigger", member->name.GetString()) == 0) {
        if (!ReadRuleProperty("trigger", json_rule, &trigger, /*required*/ false)) return;
      } else if (strcmp("description", member->name.GetString()) == 0) {
        // Ignore, this property is for user documentation.
      } else {
        AddRuleParseError() << "unexpected property '" << member->name.GetString()
                            << "' must be removed";
        return;
      }
    }
    if (search_text.empty()) {  // Only empty if not found
      AddRuleParseError() << "a 'search' property is required";
      return;
    } else if (!found_replace) {
      AddRuleParseError() << "a 'replace' property is required";
      return;
    }
    Rule rule(trigger, search_text, replace);
    if (!rule.search_pattern.ok()) {
      AddRuleParseError() << "search regex is invalid; " << rule.search_pattern.error();
      return;
    }
    (*g_rules).push_back(rule);
  }

  // Parse a rule property that should have a string value. A true return value indicates
  // success.
  bool ReadRuleProperty(const string& name, const Value& rule,
      string* value, bool required = true) {
    const Value& json_value = rule[name.c_str()];
    if (json_value.IsNull()) {
      if (required) {
        AddRuleParseError() << name << " property is required and cannot be null";
        return false;
      }
      return true;
    }
    if (!json_value.IsString()) {
      AddRuleParseError() << name << " property must be of type String but is a "
                          << NameOfTypeOfJsonValue(json_value);
      return false;
    }
    *value = json_value.GetString();
    return true;
  }

  ostream& AddDocParseError() {
    if (error_message_.tellp()) error_message_ << endl;
    error_message_ << "Error parsing redaction rules; ";
    return error_message_;
  }

  ostream& AddRuleParseError() {
    if (error_message_.tellp()) error_message_ << endl;
    error_message_ << "Error parsing redaction rule #" << (rule_idx_ + 1) <<  "; ";
    return error_message_;
  }
};

string SetRedactionRulesFromFile(const string& rules_file_path) {
  if (g_rules == NULL) g_rules = new Rules();
  g_rules->clear();

  // Read the file.
  FILE* rules_file = fopen(rules_file_path.c_str(), "r");
  if (rules_file == NULL) {
    return Substitute("Could not open redaction rules file '$0'; $1",
        rules_file_path, strerror(errno));
  }
  // Check for an empty file and ignore it. This is done to play nice with automated
  // cluster configuration tools that will generate empty files when no rules are in
  // effect. Without this the JSON parser would produce an error.
  struct stat rules_file_stats;
  if (fstat(fileno(rules_file), &rules_file_stats)) {
    fclose(rules_file);
    return Substitute("Error reading redaction rules file; $0", strerror(errno));
  }
  if (rules_file_stats.st_size == 0) {
    fclose(rules_file);
    return "";
  }

  rapidjson::FileStream stream(rules_file);
  Document rules_doc;
  rules_doc.ParseStream<rapidjson::kParseDefaultFlags>(stream);
  fclose(rules_file);
  if (rules_doc.HasParseError()) {
    return Substitute("Error parsing redaction rules; $0", rules_doc.GetParseError());
  }
  if (!rules_doc.IsObject()) {
    return "Error parsing redaction rules; root element must be a JSON Object.";
  }
  const Value& version = rules_doc["version"];
  if (version.IsNull()) {
    return "Error parsing redaction rules; a document version is required.";
  }
  if (!version.IsInt()) {
    return Substitute("Error parsing redaction rules; version must be an Integer but "
        "is a $0", NameOfTypeOfJsonValue(version));
  }
  if (version.GetInt() != 1) {
    return "Error parsing redaction rules; only version 1 is supported.";
  }

  RulesParser rules_parser;
  return rules_parser.Parse(rules_doc);
}

void Redact(string* value) {
  DCHECK(value != NULL);
  if (g_rules == NULL || g_rules->empty()) return;
  for (Rules::const_iterator rule = g_rules->begin(); rule != g_rules->end(); ++rule) {
    if (value->find(rule->trigger) == string::npos) continue;
    re2::RE2::GlobalReplace(value, rule->search_pattern, rule->replacement);
  }
}

}
