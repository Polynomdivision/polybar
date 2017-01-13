#pragma once

#include <unordered_map>

#include "common.hpp"
#include "components/logger.hpp"
#include "errors.hpp"
#include "utils/env.hpp"
#include "utils/file.hpp"
#include "utils/string.hpp"
#include "x11/xresources.hpp"

POLYBAR_NS

DEFINE_ERROR(value_error);
DEFINE_ERROR(key_error);

class config {
 public:
  using valuemap_t = std::unordered_map<string, string>;
  using sectionmap_t = std::unordered_map<string, valuemap_t>;

  using make_type = const config&;
  static make_type make(string path = "", string bar = "");

  explicit config(const logger& logger, string&& path = "", string&& bar = "");

  string filepath() const;
  string section() const;

  void warn_deprecated(const string& section, const string& key, string replacement) const;

  /**
   * Returns true if a given parameter exists
   */
  bool has(const string& section, const string& key) const {
    auto it = m_sections.find(section);
    return it != m_sections.end() && it->second.find(key) != it->second.end();
  }

  /**
   * Set parameter value
   */
  void set(const string& section, const string& key, string&& value) {
    auto it = m_sections.find(section);
    if (it == m_sections.end()) {
      valuemap_t values;
      values[key] = value;
      m_sections[section] = move(values);
    }
    auto it2 = it->second.find(key);
    if ((it2 = it->second.find(key)) == it->second.end()) {
      it2 = it->second.emplace_hint(it2, key, value);
    } else {
      it2->second = value;
    }
  }

  /**
   * Get parameter for the current bar by name
   */
  template <typename T = string>
  T get(const string& key) const {
    return get<T>(section(), key);
  }

  /**
   * Get value of a variable by section and parameter name
   */
  template <typename T = string>
  T get(const string& section, const string& key) const {
    auto it = m_sections.find(section);
    if (it == m_sections.end() || it->second.find(key) == it->second.end()) {
      throw key_error("Missing parameter [" + section + "." + key + "]");
    }
    return dereference<T>(section, key, it->second.at(key), convert<T>(string{it->second.at(key)}));
  }

  /**
   * Get value of a variable by section and parameter name
   * with a default value in case the parameter isn't defined
   */
  template <typename T = string>
  T get(const string& section, const string& key, const T& default_value) const {
    try {
      string string_value{get<string>(section, key)};
      T result{convert<T>(string{string_value})};
      return dereference<T>(move(section), move(key), move(string_value), move(result));
    } catch (const key_error& err) {
      return default_value;
    }
  }

  /**
   * Get list of values for the current bar by name
   */
  template <typename T = string>
  vector<T> get_list(const string& key) const {
    return get_list<T>(section(), key);
  }

  /**
   * Get list of values by section and parameter name
   */
  template <typename T = string>
  vector<T> get_list(const string& section, const string& key) const {
    vector<T> results;

    while (true) {
      try {
        string string_value{get<string>(section, key + "-" + to_string(results.size()))};
        T value{convert<T>(string{string_value})};

        if (!string_value.empty()) {
          results.emplace_back(dereference<T>(section, key, move(string_value), move(value)));
        } else {
          results.emplace_back(move(value));
        }
      } catch (const key_error& err) {
        break;
      }
    }

    if (results.empty()) {
      throw key_error("Missing parameter [" + section + "." + key + "-0]");
    }

    return results;
  }

  /**
   * Get list of values by section and parameter name
   * with a default list in case the list isn't defined
   */
  template <typename T = string>
  vector<T> get_list(const string& section, const string& key, const vector<T>& default_value) const {
    vector<T> results;

    while (true) {
      try {
        string string_value{get<string>(section, key + "-" + to_string(results.size()))};
        T value{convert<T>(string{string_value})};

        if (!string_value.empty()) {
          results.emplace_back(dereference<T>(section, key, move(string_value), move(value)));
        } else {
          results.emplace_back(move(value));
        }
      } catch (const key_error& err) {
        break;
      }
    }

    if (!results.empty()) {
      return results;
      ;
    }

    return default_value;
  }

  /**
   * Attempt to load value using the deprecated key name. If successful show a
   * warning message. If it fails load the value using the new key and given
   * fallback value
   */
  template <typename T = string>
  T deprecated(const string& section, const string& old, const string& newkey, const T& fallback) const {
    try {
      T value{get<T>(section, old)};
      warn_deprecated(section, old, newkey);
      return value;
    } catch (const key_error& err) {
      return get<T>(section, newkey, fallback);
    }
  }

  /**
   * @see deprecated<T>
   */
  template <typename T = string>
  T deprecated_list(const string& section, const string& old, const string& newkey, const vector<T>& fallback) const {
    try {
      vector<T> value{get_list<T>(section, old)};
      warn_deprecated(section, old, newkey);
      return value;
    } catch (const key_error& err) {
      return get_list<T>(section, newkey, fallback);
    }
  }

 protected:
  void parse_file();
  void copy_inherited();

  template <typename T>
  T convert(string&& value) const;

  /**
   * Dereference value reference
   */
  template <typename T>
  T dereference(const string& section, const string& key, const string& var, const T& fallback) const {
    if (var.substr(0, 2) != "${" || var.substr(var.length() - 1) != "}") {
      return fallback;
    }

    auto path = var.substr(2, var.length() - 3);
    size_t pos;

    if (path.compare(0, 4, "env:") == 0) {
      return dereference_env<T>(path.substr(4), fallback);
    } else if (path.compare(0, 5, "xrdb:") == 0) {
      return dereference_xrdb<T>(path.substr(5), fallback);
    } else if (path.compare(0, 5, "file:") == 0) {
      return dereference_file<T>(path.substr(5), fallback);
    } else if ((pos = path.find(".")) != string::npos) {
      return dereference_local<T>(path.substr(0, pos), path.substr(pos + 1), section);
    } else {
      throw value_error("Invalid reference defined at [" + section + "." + key + "]");
    }
  }

  /**
   * Dereference local value reference defined using:
   *  ${root.key}
   *  ${self.key}
   *  ${section.key}
   */
  template <typename T>
  T dereference_local(string section, const string& key, const string& current_section) const {
    if (section == "BAR") {
      m_log.warn("${BAR.key} is deprecated. Use ${root.key} instead");
    }

    section = string_util::replace(section, "BAR", this->section(), 0, 3);
    section = string_util::replace(section, "root", this->section(), 0, 4);
    section = string_util::replace(section, "self", current_section, 0, 4);

    try {
      string string_value{get<string>(section, key)};
      T result{convert<T>(string{string_value})};
      return dereference<T>(string(section), move(key), move(string_value), move(result));
    } catch (const key_error& err) {
      throw value_error("Unexisting reference defined [" + section + "." + key + "]");
    }
  }

  /**
   * Dereference environment variable reference defined using:
   *  ${env:key}
   *  ${env:key:fallback value}
   */
  template <typename T>
  T dereference_env(string var, const T&) const {
    size_t pos;
    string env_default{""};

    if ((pos = var.find(":")) != string::npos) {
      env_default = var.substr(pos + 1);
      var.erase(pos);
    }

    if (env_util::has(var.c_str())) {
      string env_value{env_util::get(var.c_str())};
      m_log.info("Found matching environment variable ${" + var + "} with the value \"" + env_value + "\"");
      return convert<T>(move(env_value));
    } else if (!env_default.empty()) {
      m_log.info("The environment variable ${" + var + "} is undefined or empty, using defined fallback value \"" +
                 env_default + "\"");
    } else {
      m_log.info("The environment variable ${" + var + "} is undefined or empty");
    }

    return convert<T>(move(env_default));
  }

  /**
   * Dereference X resource db value defined using:
   *  ${xrdb:key}
   *  ${xrdb:key:fallback value}
   */
  template <typename T>
  T dereference_xrdb(string var, const T& fallback) const {
    const auto& xrm = xresource_manager::make();
    size_t pos;

    if ((pos = var.find(":")) != string::npos) {
      return convert<T>(xrm->get_string(var.substr(0, pos), var.substr(pos + 1)));
    }

    string str{xrm->get_string(var, "")};
    return str.empty() ? fallback : convert<T>(move(str));
  }

  /**
   * Dereference file reference by reading its contents
   *  ${file:/absolute/file/path}
   */
  template <typename T>
  T dereference_file(string var, const T& fallback) const {
    string filename{move(var)};

    if (file_util::exists(filename)) {
      return convert<T>(string_util::trim(file_util::contents(filename), '\n'));
    }

    return fallback;
  }

 private:
  static constexpr const char* KEY_INHERIT{"inherit"};

  const logger& m_log;
  string m_file;
  string m_barname;
  sectionmap_t m_sections{};
};

POLYBAR_NS_END
