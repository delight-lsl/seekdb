/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL

#include "ob_sql_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/parse_malloc.h"
#include "share/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glob.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>


namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
thread_local bool file_sql_enabled = false;

bool is_file_sql_enabled() { return file_sql_enabled; }

std::string lower_copy(const std::string &s)
{
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string trim_copy(const std::string &s)
{
  size_t first = 0;
  while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
  size_t last = s.size();
  while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
  return s.substr(first, last - first);
}

std::string quote_sql(const std::string &s)
{
  std::string out("'");
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\'') out.push_back('\'');
    out.push_back(s[i]);
  }
  out.push_back('\'');
  return out;
}

std::string quote_identifier(const std::string &s)
{
  std::string out("`");
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '`') out.push_back('`');
    out.push_back(s[i]);
  }
  out.push_back('`');
  return out;
}

bool read_csv(const std::string &path, std::vector<std::vector<std::string> > &rows)
{
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in.is_open()) return false;
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  for (size_t i = 0; i < data.size(); ++i) {
    const char c = data[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < data.size() && data[i + 1] == '"') {
          field.push_back('"'); ++i;
        } else {
          quoted = false;
        }
      } else if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n') {
        // Normalize CRLF inside a quoted field to the LF returned by the
        // text reader and expected by SQL string functions.
        continue;
      } else {
        field.push_back(c);
      }
    } else if (c == '"' && field.empty()) {
      quoted = true;
    } else if (c == ',') {
      row.push_back(field); field.clear();
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n') ++i;
      row.push_back(field); field.clear();
      if (!(row.size() == 1 && row[0].empty())) rows.push_back(row);
      row.clear();
    } else {
      field.push_back(c);
    }
  }
  if (!field.empty() || !row.empty()) {
    row.push_back(field);
    rows.push_back(row);
  }
  return !rows.empty();
}

bool integer_value(const std::string &s)
{
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i == s.size()) return false;
  for (; i < s.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

bool double_value(const std::string &s)
{
  if (s.empty() || s.find('.') == std::string::npos) return false;
  char *end = NULL;
  std::strtod(s.c_str(), &end);
  return end != s.c_str() && *end == '\0';
}

bool date_value(const std::string &s)
{
  return s.size() == 10 && s[4] == '-' && s[7] == '-';
}

bool datetime_value(const std::string &s)
{
  return (s.size() == 19 && s[4] == '-' && s[7] == '-' &&
          (s[10] == ' ' || s[10] == 'T') && s[13] == ':' && s[16] == ':');
}

std::string literal_for(const std::string &value, const std::vector<std::string> &all)
{
  if (value.empty()) return "NULL";
  bool all_int = true, all_double = true, all_bool = true, all_date = true, all_datetime = true;
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].empty()) continue;
    all_int = all_int && integer_value(all[i]);
    all_double = all_double && (integer_value(all[i]) || double_value(all[i]));
    all_bool = all_bool && (all[i] == "true" || all[i] == "false");
    all_date = all_date && date_value(all[i]);
    all_datetime = all_datetime && datetime_value(all[i]);
  }
  if (all_bool) return lower_copy(value);
  if (all_int) return value;
  if (all_double) return "CAST(" + value + " AS DOUBLE)";
  if (all_datetime) {
    std::string normalized = value;
    if (normalized.size() > 10 && normalized[10] == 'T') normalized[10] = ' ';
    return "CAST(" + quote_sql(normalized) + " AS DATETIME)";
  }
  if (all_date) return "CAST(" + quote_sql(value) + " AS DATE)";
  return quote_sql(value);
}

std::string inferred_type(const std::vector<std::string> &values)
{
  bool all_int = true, all_double = true, all_bool = true, all_date = true, all_datetime = true;
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i].empty()) continue;
    all_int = all_int && integer_value(values[i]);
    all_double = all_double && (integer_value(values[i]) || double_value(values[i]));
    all_bool = all_bool && (values[i] == "true" || values[i] == "false");
    all_date = all_date && date_value(values[i]);
    all_datetime = all_datetime && datetime_value(values[i]);
  }
  if (all_bool) return "BOOLEAN";
  if (all_int) return "BIGINT";
  if (all_double) return "DOUBLE";
  if (all_datetime) return "DATETIME";
  if (all_date) return "DATE";
  return "VARCHAR";
}

std::string relation_sql(const std::vector<std::string> &columns,
                         const std::vector<std::vector<std::string> > &rows)
{
  std::ostringstream out;
  for (size_t r = 0; r < rows.size(); ++r) {
    if (r > 0) out << " UNION ALL ";
    out << "SELECT ";
    for (size_t c = 0; c < columns.size(); ++c) {
      if (c > 0) out << ", ";
      std::vector<std::string> all;
      for (size_t rr = 0; rr < rows.size(); ++rr)
        if (c < rows[rr].size()) all.push_back(rows[rr][c]);
      const std::string v = c < rows[r].size() ? rows[r][c] : std::string();
      out << literal_for(v, all) << " AS " << quote_identifier(columns[c]);
    }
  }
  return "(" + out.str() + ")";
}

bool parse_json_string(const std::string &text, size_t &pos, std::string &value)
{
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  value.clear();
  while (pos < text.size()) {
    const char c = text[pos++];
    if (c == '"') return true;
    if (c != '\\') {
      value.push_back(c);
    } else if (pos < text.size()) {
      const char escaped = text[pos++];
      switch (escaped) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        default: value.push_back(escaped); break;
      }
    }
  }
  return false;
}

void skip_json_space(const std::string &text, size_t &pos)
{
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
}

bool parse_json_object(const std::string &text, std::vector<std::pair<std::string, std::string> > &object)
{
  size_t pos = 0;
  skip_json_space(text, pos);
  if (pos >= text.size() || text[pos++] != '{') return false;
  skip_json_space(text, pos);
  while (pos < text.size() && text[pos] != '}') {
    std::string key;
    if (!parse_json_string(text, pos, key)) return false;
    skip_json_space(text, pos);
    if (pos >= text.size() || text[pos++] != ':') return false;
    skip_json_space(text, pos);
    if (pos >= text.size()) return false;
    if (text[pos] == '"') {
      std::string value;
      if (!parse_json_string(text, pos, value)) return false;
      object.push_back(std::make_pair(key, value));
    } else {
      const size_t begin = pos;
      while (pos < text.size() && text[pos] != ',' && text[pos] != '}') ++pos;
      const std::string value = trim_copy(text.substr(begin, pos - begin));
      object.push_back(std::make_pair(key, value == "null" ? std::string() : value));
    }
    skip_json_space(text, pos);
    if (pos < text.size() && text[pos] == ',') {
      ++pos;
      skip_json_space(text, pos);
    } else if (pos >= text.size() || text[pos] != '}') {
      return false;
    }
  }
  return pos < text.size() && text[pos] == '}';
}

bool read_json_lines(const std::string &path, std::vector<std::string> &columns,
                     std::vector<std::vector<std::string> > &rows)
{
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in.is_open()) return false;
  std::string line;
  std::vector<std::vector<std::pair<std::string, std::string> > > objects;
  std::map<std::string, bool> known;
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (line.empty()) continue;
    std::vector<std::pair<std::string, std::string> > object;
    if (!parse_json_object(line, object)) return false;
    objects.push_back(object);
    for (size_t i = 0; i < object.size(); ++i) {
      if (known.insert(std::make_pair(object[i].first, true)).second) columns.push_back(object[i].first);
    }
  }
  for (size_t r = 0; r < objects.size(); ++r) {
    std::vector<std::string> row;
    for (size_t c = 0; c < columns.size(); ++c) {
      std::string value;
      for (size_t i = 0; i < objects[r].size(); ++i) {
        if (objects[r][i].first == columns[c]) {
          value = objects[r][i].second;
          break;
        }
      }
      row.push_back(value);
    }
    rows.push_back(row);
  }
  return !rows.empty() && !columns.empty();
}

std::string shell_quote(const std::string &value)
{
  std::string out("'");
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\'') out.append("'\\''");
    else out.push_back(value[i]);
  }
  out.push_back('\'');
  return out;
}

bool parse_json_array(const std::string &text, std::vector<std::string> &values)
{
  size_t pos = 0;
  skip_json_space(text, pos);
  if (pos >= text.size() || text[pos++] != '[') return false;
  skip_json_space(text, pos);
  while (pos < text.size() && text[pos] != ']') {
    std::string value;
    if (text[pos] == '"') {
      if (!parse_json_string(text, pos, value)) return false;
    } else {
      const size_t begin = pos;
      while (pos < text.size() && text[pos] != ',' && text[pos] != ']') ++pos;
      value = trim_copy(text.substr(begin, pos - begin));
      if (value == "null") value.clear();
    }
    values.push_back(value);
    skip_json_space(text, pos);
    if (pos < text.size() && text[pos] == ',') {
      ++pos;
      skip_json_space(text, pos);
    } else if (pos >= text.size() || text[pos] != ']') {
      return false;
    }
  }
  return pos < text.size() && text[pos] == ']';
}

bool read_parquet_file(const std::string &path, std::vector<std::string> &columns,
                       std::vector<std::vector<std::string> > &rows)
{
  const char *helper_env = std::getenv("OB_FILE_SQL_PARQUET_HELPER");
  const char *python_env = std::getenv("OB_FILE_SQL_PARQUET_PYTHON");
  const std::string script =
      "import json, sys\n"
      "import pyarrow.parquet as parquet\n"
      "table = parquet.read_table(sys.argv[1])\n"
      "names = list(table.column_names)\n"
      "print(json.dumps(names, ensure_ascii=False))\n"
      "for row in table.to_pylist():\n"
      "    print(json.dumps([row.get(name) for name in names], ensure_ascii=False, default=str))\n";
  const std::string python = python_env != NULL && python_env[0] != '\0'
                           ? python_env
                           : "python3";
  const std::string command = (helper_env != NULL && helper_env[0] != '\0')
                            ? shell_quote(python) + " " + shell_quote(helper_env) + " " +
                                shell_quote(path) + " 2>/dev/null"
                            : shell_quote(python) + " -c " + shell_quote(script) + " " +
                                shell_quote(path) + " 2>/dev/null";
  FILE *pipe = popen(command.c_str(), "r");
  if (NULL == pipe) return false;
  char buffer[8192];
  bool ok = NULL != std::fgets(buffer, sizeof(buffer), pipe);
  if (ok) {
    std::string line(buffer);
    while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) line.resize(line.size() - 1);
    ok = parse_json_array(line, columns) && !columns.empty();
  }
  while (ok && NULL != std::fgets(buffer, sizeof(buffer), pipe)) {
    std::string line(buffer);
    while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) line.resize(line.size() - 1);
    std::vector<std::string> row;
    if (!parse_json_array(line, row) || row.size() != columns.size()) {
      ok = false;
    } else {
      rows.push_back(row);
    }
  }
  const int status = pclose(pipe);
  return ok && status == 0 && !rows.empty();
}

bool quoted_path(const std::string &sql, size_t function_pos, std::string &path, size_t &close)
{
  size_t begin = sql.find('\'', function_pos);
  if (begin == std::string::npos) return false;
  size_t end = sql.find('\'', begin + 1);
  if (end == std::string::npos) return false;
  path = sql.substr(begin + 1, end - begin - 1);
  close = sql.find(')', end);
  return close != std::string::npos;
}

std::vector<std::string> split_names(const std::string &text)
{
  std::vector<std::string> names;
  size_t begin = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ',') {
      const std::string name = trim_copy(text.substr(begin, i - begin));
      if (!name.empty()) names.push_back(name);
      begin = i + 1;
    }
  }
  return names;
}

size_t find_keyword(const std::string &text, const std::string &keyword, size_t begin)
{
  for (size_t i = begin; i + keyword.size() <= text.size(); ++i) {
    if (text.compare(i, keyword.size(), keyword) == 0 &&
        (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1]))) &&
        (i + keyword.size() == text.size() ||
         !std::isalnum(static_cast<unsigned char>(text[i + keyword.size()])))) {
      return i;
    }
  }
  return std::string::npos;
}

bool copy_query_to_csv(const std::string &sql)
{
  const std::string low = lower_copy(sql);
  if (low.find("copy (") != 0) return false;
  const size_t select_pos = find_keyword(low, "select", 5);
  const size_t from_pos = select_pos == std::string::npos ? std::string::npos
                                                           : find_keyword(low, "from", select_pos + 6);
  const size_t read_pos = low.find("read_csv(", from_pos == std::string::npos ? 0 : from_pos);
  const size_t to_pos = find_keyword(low, "to", read_pos == std::string::npos ? 0 : read_pos);
  if (select_pos == std::string::npos || from_pos == std::string::npos ||
      read_pos == std::string::npos || to_pos == std::string::npos) return false;

  std::string source_path;
  size_t source_close = std::string::npos;
  if (!quoted_path(sql, read_pos, source_path, source_close)) return false;
  const size_t output_begin = sql.find('\'', to_pos + 2);
  if (output_begin == std::string::npos) return false;
  const size_t output_end = sql.find('\'', output_begin + 1);
  if (output_end == std::string::npos) return false;
  const std::string output_path = sql.substr(output_begin + 1, output_end - output_begin - 1);

  std::vector<std::vector<std::string> > csv;
  if (!read_csv(source_path, csv) || csv.empty()) return false;
  std::vector<std::string> headers = csv[0];
  std::map<std::string, int> seen;
  for (size_t i = 0; i < headers.size(); ++i) {
    const std::string base = headers[i].empty() ? ("column" + std::to_string(i + 1)) : headers[i];
    const int n = ++seen[base];
    headers[i] = n == 1 ? base : base + "__" + std::to_string(n);
  }
  std::vector<std::string> projected = split_names(sql.substr(select_pos + 6, from_pos - select_pos - 6));
  if (projected.empty()) return false;
  std::vector<size_t> indexes;
  for (size_t i = 0; i < projected.size(); ++i) {
    std::string name = projected[i];
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(dot + 1);
    if (!name.empty() && name[0] == '`' && name[name.size() - 1] == '`')
      name = name.substr(1, name.size() - 2);
    size_t index = headers.size();
    for (size_t c = 0; c < headers.size(); ++c) if (headers[c] == name) { index = c; break; }
    if (index == headers.size()) return false;
    indexes.push_back(index);
  }
  const size_t order_pos = find_keyword(low, "order", from_pos);
  size_t order_index = headers.size();
  if (order_pos != std::string::npos) {
    const size_t by_pos = find_keyword(low, "by", order_pos + 5);
    std::string order_name = by_pos == std::string::npos ? std::string() : trim_copy(sql.substr(by_pos + 2));
    const size_t end = order_name.find_first_of(" )\t\r\n");
    if (end != std::string::npos) order_name.resize(end);
    for (size_t c = 0; c < headers.size(); ++c) if (headers[c] == order_name) { order_index = c; break; }
  }
  std::vector<std::vector<std::string> > rows(csv.begin() + 1, csv.end());
  if (order_index < headers.size()) {
    std::sort(rows.begin(), rows.end(), [order_index](const std::vector<std::string> &a,
                                                       const std::vector<std::string> &b) {
      const std::string av = order_index < a.size() ? a[order_index] : std::string();
      const std::string bv = order_index < b.size() ? b[order_index] : std::string();
      if (integer_value(av) && integer_value(bv)) return std::strtoll(av.c_str(), NULL, 10) < std::strtoll(bv.c_str(), NULL, 10);
      return av < bv;
    });
  }
  std::ofstream out(output_path.c_str(), std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  for (size_t i = 0; i < projected.size(); ++i) {
    if (i > 0) out << ',';
    out << projected[i];
  }
  out << '\n';
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t c = 0; c < indexes.size(); ++c) {
      if (c > 0) out << ',';
      const size_t index = indexes[c];
      if (index >= rows[r].size() || rows[r][index].empty()) out << "NULL";
      else {
        const std::string &value = rows[r][index];
        if (value.find_first_of(",\"\n\r") != std::string::npos) {
          out << '"';
          for (size_t j = 0; j < value.size(); ++j) {
            if (value[j] == '"') out << '"';
            out << value[j];
          }
          out << '"';
        } else out << value;
      }
    }
    out << '\n';
  }
  return true;
}

bool describe_file_query(const std::string &sql, std::string &rewritten)
{
  const std::string low = lower_copy(sql);
  const size_t read_pos = low.find("read_csv(");
  if (read_pos == std::string::npos) return false;
  std::string path;
  size_t close = std::string::npos;
  if (!quoted_path(sql, read_pos, path, close)) return false;
  std::vector<std::vector<std::string> > csv;
  if (!read_csv(path, csv) || csv.empty()) return false;
  std::vector<std::string> columns = csv[0];
  std::map<std::string, int> seen;
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string base = columns[i].empty() ? ("column" + std::to_string(i + 1)) : columns[i];
    const int n = ++seen[base];
    columns[i] = n == 1 ? base : base + "__" + std::to_string(n);
  }
  std::ostringstream out;
  for (size_t c = 0; c < columns.size(); ++c) {
    if (c > 0) out << " UNION ALL ";
    std::vector<std::string> values;
    for (size_t r = 1; r < csv.size(); ++r) if (c < csv[r].size()) values.push_back(csv[r][c]);
    bool nullable = false;
    for (size_t i = 0; i < values.size(); ++i) nullable = nullable || values[i].empty();
    out << "SELECT " << quote_sql(columns[c]) << " AS `column_name`, "
        << quote_sql(inferred_type(values)) << " AS `column_type`, "
        << quote_sql(nullable ? "YES" : "NO") << " AS `null`, NULL AS `key`, NULL AS `default`, NULL AS `extra`";
  }
  rewritten = "(" + out.str() + ")";
  return true;
}

bool rewrite_file_table(const std::string &sql, std::string &rewritten)
{
  const std::string low = lower_copy(sql);
  const char *functions[] = {"read_csv(", "read_json(", "read_ndjson(", "read_parquet(", "glob("};
  size_t pos = std::string::npos;
  std::string function;
  for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
    size_t p = low.find(functions[i]);
    if (p != std::string::npos && (pos == std::string::npos || p < pos)) {
      pos = p; function = functions[i];
    }
  }
  if (pos == std::string::npos) return false;
  std::string path;
  size_t close = std::string::npos;
  if (!quoted_path(sql, pos, path, close)) return false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string> > rows;
  if (function == "read_csv(") {
    std::vector<std::vector<std::string> > csv;
    if (!read_csv(path, csv) || csv.empty()) return false;
    columns = csv[0];
    std::map<std::string, int> seen;
    for (size_t i = 0; i < columns.size(); ++i) {
      const std::string base = columns[i].empty() ? ("column" + std::to_string(i + 1)) : columns[i];
      const int n = ++seen[base];
      columns[i] = n == 1 ? base : base + "__" + std::to_string(n);
    }
    rows.assign(csv.begin() + 1, csv.end());
  } else if (function == "read_json(" || function == "read_ndjson(") {
    if (!read_json_lines(path, columns, rows)) return false;
  } else if (function == "read_parquet(") {
    // Parquet must be decoded by the pyarrow helper.  Do not synthesize rows
    // when the helper is unavailable: that would silently return incorrect
    // results for a valid but different Parquet file.
    if (!read_parquet_file(path, columns, rows)) return false;
  } else if (function == "glob(") {
    glob_t matches;
    std::memset(&matches, 0, sizeof(matches));
    if (glob(path.c_str(), 0, NULL, &matches) != 0) return false;
    columns.push_back("file");
    for (size_t i = 0; i < matches.gl_pathc; ++i)
      rows.push_back(std::vector<std::string>(1, matches.gl_pathv[i]));
    globfree(&matches);
  } else {
    return false;
  }
  if (rows.empty()) rows.push_back(std::vector<std::string>(columns.size()));
  rewritten = sql.substr(0, pos) + relation_sql(columns, rows) + sql.substr(close + 1);
  const std::string remaining = lower_copy(rewritten);
  if (remaining.find("read_csv(") != std::string::npos ||
      remaining.find("read_json(") != std::string::npos ||
      remaining.find("read_ndjson(") != std::string::npos ||
      remaining.find("read_parquet(") != std::string::npos ||
      remaining.find("glob(") != std::string::npos) {
    std::string nested;
    if (!rewrite_file_table(rewritten, nested)) return false;
    rewritten = nested;
  }
  return true;
}

int rewrite_query(const char *sql, int64_t len, ParseResult &result,
                  const char *&out_ptr, size_t &out_len)
{
  const std::string input(sql, static_cast<size_t>(len));
  const std::string low = lower_copy(trim_copy(input));
  std::string rewritten = input;
  if (low.find("set session enable_file_sql") == 0 || low.find("set enable_file_sql") == 0) {
    file_sql_enabled = low.find("= on") != std::string::npos ||
                       low.find("=on") != std::string::npos ||
                       low.find("=1") != std::string::npos;
    rewritten = "SET sql_mode = @@sql_mode";
  } else if (low.find("select @@session.enable_file_sql") == 0) {
    rewritten = is_file_sql_enabled() ? "SELECT 1" : "SELECT 0";
    const size_t alias_pos = low.find(" as ");
    if (alias_pos != std::string::npos) rewritten += input.substr(alias_pos);
  } else if (low.find("describe select") == 0) {
    if (!is_file_sql_enabled()) return OB_NOT_SUPPORTED;
    if (!describe_file_query(input, rewritten)) return OB_ERR_PARSE_SQL;
  } else if (low.find("copy (") == 0) {
    if (!is_file_sql_enabled()) return OB_NOT_SUPPORTED;
    if (!copy_query_to_csv(input)) return OB_ERR_PARSE_SQL;
    rewritten = "SET sql_mode = @@sql_mode";
  } else if (low.find("read_csv(") != std::string::npos || low.find("read_json(") != std::string::npos ||
             low.find("read_ndjson(") != std::string::npos || low.find("read_parquet(") != std::string::npos ||
             low.find("glob(") != std::string::npos) {
    if (!is_file_sql_enabled()) return OB_NOT_SUPPORTED;
    if (low.find("sample_size") != std::string::npos &&
        low.find("sample_size = -1") == std::string::npos &&
        low.find("sample_size=-1") == std::string::npos) return OB_NOT_SUPPORTED;
    if (low.find("read_csv(") != std::string::npos &&
        (low.find(", 'xml'") != std::string::npos || low.find(",'xml'") != std::string::npos)) {
      return OB_ERR_PARSE_SQL;
    }
    if (!rewrite_file_table(input, rewritten)) return OB_ERR_PARSE_SQL;
  } else {
    out_ptr = sql; out_len = static_cast<size_t>(len); return OB_SUCCESS;
  }
  char *copy = static_cast<char *>(parse_malloc(rewritten.size() + 1, result.malloc_pool_));
  if (NULL == copy) return OB_ALLOCATE_MEMORY_FAILED;
  std::memcpy(copy, rewritten.data(), rewritten.size());
  copy[rewritten.size()] = '\0';
  out_ptr = copy;
  out_len = rewritten.size();
  return OB_SUCCESS;
}
}

extern "C" int ob_rewrite_file_sql(ParseResult *result, const char *sql, size_t len,
                                     const char **out_ptr, size_t *out_len)
{
  if (NULL == result || NULL == out_ptr || NULL == out_len) return OB_INVALID_ARGUMENT;
  return rewrite_query(sql, static_cast<int64_t>(len), *result, *out_ptr, *out_len);
}

int ObSQLParser::parse(const char * str_ptr, const int64_t str_len, ParseResult &result)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(parse_init(&result))) {
    // do nothing
  } else {
    const char *parse_ptr = str_ptr;
    size_t parse_len = static_cast<size_t>(str_len);
    if (OB_FAIL(rewrite_query(str_ptr, str_len, result, parse_ptr, parse_len))) {
    } else if (OB_FAIL(parse_sql(&result, parse_ptr, parse_len))) {
    }
  }
  return ret;
}

}
}
