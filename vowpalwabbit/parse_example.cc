/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */

#include <cmath>
#include <math.h>
#include <ctype.h>
#include <boost/utility/string_view.hpp>
#include "parse_example.h"
#include "hash.h"
#include "unique_sort.h"
#include "global_data.h"
#include "constant.h"

using namespace std;

size_t read_features(vw* all, char*& line, size_t& num_chars)
{
  line = nullptr;
  size_t num_chars_initial = readto(*(all->p->input), line, '\n');
  if (num_chars_initial < 1)
    return num_chars_initial;
  num_chars = num_chars_initial;
  if (line[0] == '\xef' && num_chars >= 3 && line[1] == '\xbb' && line[2] == '\xbf')
  {
    line += 3;
    num_chars -= 3;
  }
  if (num_chars > 0 && line[num_chars - 1] == '\n')
    num_chars--;
  if (num_chars > 0 && line[num_chars - 1] == '\r')
    num_chars--;
  return num_chars_initial;
}

int read_features_string(vw* all, v_array<example*>& examples)
{
  char* line;
  size_t num_chars;
  size_t num_chars_initial = read_features(all, line, num_chars);
  if (num_chars_initial < 1)
    return (int)num_chars_initial;

  boost::string_view example(line, num_chars);
  substring_to_example(all, examples[0], example);

  return (int)num_chars_initial;
}

template <bool audit>
class TC_parser
{
 public:
  const boost::string_view m_line;
  size_t m_read_idx;
  float m_cur_channel_v;
  bool m_new_index;
  size_t m_anon;
  uint64_t m_channel_hash;
  boost::string_view m_base;
  unsigned char m_index;
  float m_v;
  bool m_redefine_some;
  unsigned char (*m_redefine)[256];
  parser* m_p;
  example* m_ae;
  uint64_t* m_affix_features;
  bool* m_spelling_features;
  v_array<char> m_spelling;
  uint32_t m_hash_seed;
  uint64_t m_parse_mask;

  vector<feature_dict*>* m_namespace_dictionaries;

  ~TC_parser() {}

  inline void parserWarning(const char* message, boost::string_view var_msg, const char* message2)
  {
    // string_view will output the entire view into the output stream.
    // That means if there is a null character somewhere in the range, it will terminate
    // the stringstream at that point! Minor hack to give us the behavior we actually want here (i think)..
    // the alternative is to do what the old code was doing.. str(m_line).c_str()...
    // TODO: Find a sane way to handle nulls in the middle of a string (either string_view or substring)
    auto tmp_view = m_line.substr(0, m_line.find('\0'));
    std::stringstream ss;
    ss << message << var_msg << message2 << "in Example #" << this->m_p->end_parsed_examples << ": \"" << tmp_view << "\""
       << endl;
    if (m_p->strict_parse)
    {
      THROW_EX(VW::strict_parse_exception, ss.str());
    }
    else
    {
      cerr << ss.str();
    }
  }

  inline float featureValue()
  {
    if (m_read_idx >= m_line.size() || m_line[m_read_idx] == ' ' || m_line[m_read_idx] == '\t' ||
        m_line[m_read_idx] == '|' || m_line[m_read_idx] == '\r')
      return 1.;
    else if (m_line[m_read_idx] == ':')
    {
      // featureValue --> ':' 'Float'
      ++m_read_idx;
      size_t end_read = 0;
      m_v = parse_float_string_view(m_line.substr(m_read_idx), end_read);
      if (end_read == 0)
      {
        parserWarning("malformed example! Float expected after : \"", m_line.substr(0, m_read_idx), "\"");
      }
      if (std::isnan(m_v))
      {
        m_v = 0.f;
        parserWarning(
            "warning: invalid feature value:\"", m_line.substr(m_read_idx), "\" read as NaN. Replacing with 0.");
      }
      m_read_idx += end_read;
      return m_v;
    }
    else
    {
      // syntax error
      parserWarning(
          "malformed example! '|', ':', space, or EOL expected after : \"", m_line.substr(0, m_read_idx), "\"");
      return 0.f;
    }
  }

  inline boost::string_view read_name()
  {
    size_t name_start = m_read_idx;
    while (!(m_read_idx >= m_line.size() || m_line[m_read_idx] == ' ' || m_line[m_read_idx] == ':' ||
        m_line[m_read_idx] == '\t' || m_line[m_read_idx] == '|' || m_line[m_read_idx] == '\r'))
      ++m_read_idx;

    return m_line.substr(name_start, m_read_idx - name_start);
  }

  inline void maybeFeature()
  {
    if (m_read_idx >= m_line.size() || m_line[m_read_idx] == ' ' || m_line[m_read_idx] == '\t' ||
        m_line[m_read_idx] == '|' || m_line[m_read_idx] == '\r')
    {
      // maybeFeature --> ø
    }
    else
    {
      // maybeFeature --> 'String' FeatureValue
      boost::string_view feature_name = read_name();
      m_v = m_cur_channel_v * featureValue();
      uint64_t word_hash;
      if (!feature_name.empty())
        word_hash = (m_p->hasher(feature_name, m_channel_hash) & m_parse_mask);
      else
        word_hash = m_channel_hash + m_anon++;
      if (m_v == 0)
        return;  // dont add 0 valued features to list of features
      features& fs = m_ae->feature_space[m_index];
      fs.push_back(m_v, word_hash);
      if (audit)
      {
        fs.space_names.push_back(audit_strings_ptr(new audit_strings(m_base, feature_name)));
      }
      if ((m_affix_features[m_index] > 0) && (!feature_name.empty()))
      {
        features& affix_fs = m_ae->feature_space[affix_namespace];
        if (affix_fs.size() == 0)
          m_ae->indices.push_back(affix_namespace);
        uint64_t affix = m_affix_features[m_index];
        while (affix > 0)
        {
          bool is_prefix = affix & 0x1;
          uint64_t len = (affix >> 1) & 0x7;
          boost::string_view affix_name(feature_name);
          if (affix_name.size() > len)
          {
            if (is_prefix)
              affix_name.remove_suffix(affix_name.size() - len);
            else
              affix_name.remove_prefix(affix_name.size() - len);
          }

          word_hash =
              m_p->hasher(affix_name, (uint64_t)m_channel_hash) * (affix_constant + (affix & 0xF) * quadratic_constant);
          affix_fs.push_back(m_v, word_hash);
          if (audit)
          {
            v_array<char> affix_v = v_init<char>();
            if (m_index != ' ')
              affix_v.push_back(m_index);
            affix_v.push_back(is_prefix ? '+' : '-');
            affix_v.push_back('0' + (char)len);
            affix_v.push_back('=');
            push_many(affix_v, affix_name.begin(), affix_name.size());
            affix_v.push_back('\0');
            affix_fs.space_names.push_back(audit_strings_ptr(new audit_strings("affix", affix_v.begin())));
          }
          affix >>= 4;
        }
      }
      if (m_spelling_features[m_index])
      {
        features& spell_fs = m_ae->feature_space[spelling_namespace];
        if (spell_fs.size() == 0)
          m_ae->indices.push_back(spelling_namespace);
        // v_array<char> spelling;
        m_spelling.clear();
        for (char c : feature_name)
        {
          char d = 0;
          if ((c >= '0') && (c <= '9'))
            d = '0';
          else if ((c >= 'a') && (c <= 'z'))
            d = 'a';
          else if ((c >= 'A') && (c <= 'Z'))
            d = 'A';
          else if (c == '.')
            d = '.';
          else
            d = '#';
          // if ((spelling.size() == 0) || (spelling.last() != d))
          m_spelling.push_back(d);
        }

        boost::string_view spelling_strview(m_spelling.begin(), m_spelling.size());
        uint64_t word_hash = hashstring(spelling_strview, (uint64_t)m_channel_hash);
        spell_fs.push_back(m_v, word_hash);
        if (audit)
        {
          v_array<char> spelling_v = v_init<char>();
          if (m_index != ' ')
          {
            spelling_v.push_back(m_index);
            spelling_v.push_back('_');
          }
          push_many(spelling_v, spelling_strview.begin(), spelling_strview.size());
          spelling_v.push_back('\0');
          spell_fs.space_names.push_back(audit_strings_ptr(new audit_strings("spelling", spelling_v.begin())));
        }
      }
      if (m_namespace_dictionaries[m_index].size() > 0)
      {
        for (auto map : m_namespace_dictionaries[m_index])
        {
          uint64_t hash = uniform_hash(feature_name.begin(), feature_name.size(), quadratic_constant);
          features* feats = map->get(feature_name, hash);
          if ((feats != nullptr) && (feats->values.size() > 0))
          {
            features& dict_fs = m_ae->feature_space[dictionary_namespace];
            if (dict_fs.size() == 0)
              m_ae->indices.push_back(dictionary_namespace);
            push_many(dict_fs.values, feats->values.begin(), feats->values.size());
            push_many(dict_fs.indicies, feats->indicies.begin(), feats->indicies.size());
            dict_fs.sum_feat_sq += feats->sum_feat_sq;
            if (audit)
              for (const auto& id : feats->indicies)
              {
                stringstream ss;
                ss << m_index << '_';
                ss << feature_name;
                ss << '=' << id;
                dict_fs.space_names.push_back(audit_strings_ptr(new audit_strings("dictionary", ss.str())));
              }
          }
        }
      }
    }
  }

  inline void nameSpaceInfoValue()
  {
    if (m_read_idx >= m_line.size() || m_line[m_read_idx] == ' ' || m_line[m_read_idx] == '\t' ||
        m_line[m_read_idx] == '|' || m_line[m_read_idx] == '\r')
    {
      // nameSpaceInfoValue -->  ø
    }
    else if (m_line[m_read_idx] == ':')
    {
      // nameSpaceInfoValue --> ':' 'Float'
      ++m_read_idx;
      size_t end_read = 0;
      m_cur_channel_v = parse_float_string_view(m_line.substr(m_read_idx), end_read);
      if (end_read + m_read_idx >= m_line.size())
      {
        parserWarning("malformed example! Float expected after : \"", m_line.substr(0, m_read_idx), "\"");
      }
      if (std::isnan(m_cur_channel_v))
      {
        m_cur_channel_v = 1.f;
        parserWarning(
            "warning: invalid namespace value:\"", m_line.substr(m_read_idx), "\" read as NaN. Replacing with 1.");
      }
      m_read_idx += end_read;
    }
    else
    {
      // syntax error
      parserWarning(
          "malformed example! '|',':', space, or EOL expected after : \"", m_line.substr(0, m_read_idx), "\"");
    }
  }

  inline void nameSpaceInfo()
  {
    if (m_read_idx >= m_line.size() || m_line[m_read_idx] == '|' || m_line[m_read_idx] == ' ' ||
        m_line[m_read_idx] == '\t' || m_line[m_read_idx] == ':' || m_line[m_read_idx] == '\r')
    {
      // syntax error
      parserWarning("malformed example! String expected after : \"", m_line.substr(0, m_read_idx), "\"");
    }
    else
    {
      // NameSpaceInfo --> 'String' NameSpaceInfoValue
      m_index = (unsigned char)(m_line[m_read_idx]);
      if (m_redefine_some)
        m_index = (*m_redefine)[m_index];  // redefine m_index
      if (m_ae->feature_space[m_index].size() == 0)
        m_new_index = true;
      boost::string_view name = read_name();
      if (audit)
      {
        m_base = name;
      }
      m_channel_hash = m_p->hasher(name, this->m_hash_seed);
      nameSpaceInfoValue();
    }
  }

  inline void listFeatures()
  {
    while ((m_read_idx < m_line.size()) && (m_line[m_read_idx] == ' ' || m_line[m_read_idx] == '\t'))
    {
      // listFeatures --> ' ' MaybeFeature ListFeatures
      ++m_read_idx;
      maybeFeature();
    }
    if (!(m_read_idx >= m_line.size() || m_line[m_read_idx] == '|' || m_line[m_read_idx] == '\r'))
    {
      // syntax error
      parserWarning("malformed example! '|',space, or EOL expected after : \"", m_line.substr(0, m_read_idx), "\"");
    }
  }

  inline void nameSpace()
  {
    m_cur_channel_v = 1.0;
    m_index = 0;
    m_new_index = false;
    m_anon = 0;
    if (m_read_idx >= m_line.size() || m_line[m_read_idx] == ' ' || m_line[m_read_idx] == '\t' ||
        m_line[m_read_idx] == '|' || m_line[m_read_idx] == '\r')
    {
      // NameSpace --> ListFeatures
      m_index = (unsigned char)' ';
      if (m_ae->feature_space[m_index].size() == 0)
        m_new_index = true;
      if (audit)
      {
        // TODO: c++17 allows string_view literals, eg: " "sv
        static const char* space = " ";
        m_base = space;
      }
      m_channel_hash = this->m_hash_seed == 0 ? 0 : uniform_hash("", 0, this->m_hash_seed);
      listFeatures();
    }
    else if (m_line[m_read_idx] != ':')
    {
      // NameSpace --> NameSpaceInfo ListFeatures
      nameSpaceInfo();
      listFeatures();
    }
    else
    {
      // syntax error
      parserWarning(
          "malformed example! '|',String,space, or EOL expected after : \"", m_line.substr(0, m_read_idx), "\"");
    }
    if (m_new_index && m_ae->feature_space[m_index].size() > 0)
      m_ae->indices.push_back(m_index);
  }

  inline void listNameSpace()
  {
    while (
        (m_read_idx < m_line.size()) && (m_line[m_read_idx] == '|'))  // ListNameSpace --> '|' NameSpace ListNameSpace
    {
      ++m_read_idx;
      nameSpace();
    }
    if (m_read_idx < m_line.size() && m_line[m_read_idx] != '\r')
    {
      // syntax error
      parserWarning("malformed example! '|' or EOL expected after : \"", m_line.substr(0, m_read_idx), "\"");
    }
  }

  TC_parser(boost::string_view line, vw& all, example* ae) : m_line(line)
  {
    m_spelling = v_init<char>();
    if (!m_line.empty())
    {
      this->m_read_idx = 0;
      this->m_p = all.p;
      this->m_redefine_some = all.redefine_some;
      this->m_redefine = &all.redefine;
      this->m_ae = ae;
      this->m_affix_features = all.affix_features;
      this->m_spelling_features = all.spelling_features;
      this->m_namespace_dictionaries = all.namespace_dictionaries;
      this->m_hash_seed = all.hash_seed;
      this->m_parse_mask = all.parse_mask;
      listNameSpace();
    }
  }
};

void substring_to_example(vw* all, example* ae, boost::string_view example)
{
  all->p->lp.default_label(&ae->l);

  size_t bar_idx = example.find('|');

  all->p->words.clear();
  if (bar_idx != 0)
  {
    boost::string_view label_space(example);
    if (bar_idx != boost::string_view::npos)
    {
      // a little bit iffy since bar_idx is based on example and we're working off label_space
      // but safe as long as this is the first manipulation after the copy
      label_space.remove_suffix(label_space.size() - bar_idx);
    }
    size_t tab_idx = label_space.find('\t');
    if (tab_idx != boost::string_view::npos)
    {
      label_space.remove_prefix(tab_idx + 1);
    }

    std::vector<boost::string_view> tokenized;
    tokenize(' ', label_space, all->p->words);
    if (all->p->words.size() > 0 &&
        (all->p->words.last().end == label_space.end() ||
            *(all->p->words.last().begin) == '\''))  // The last field is a tag, so record and strip it off
    {
      boost::string_view tag = all->p->words.pop();
      if (*tag.begin == '\'')
        tag.begin++;
      push_many(ae->tag, tag.begin(), tag.size());
    }
  }

  if (all->p->words.size() > 0)
    all->p->lp.parse_label(all->p, all->sd, &ae->l, all->p->words);

  if (bar_idx != boost::string_view::npos)
  {
    if (all->audit || all->hash_inv)
      TC_parser<true> parser_line(example.substr(bar_idx), *all, ae);
    else
      TC_parser<false> parser_line(example.substr(bar_idx), *all, ae);
  }
}

namespace VW
{
void read_line(vw& all, example* ex, boost::string_view line)
{
  while (line.size() > 0 && line.back() == '\n') line.remove_suffix(1);
  substring_to_example(&all, ex, line);
}

void read_line(vw& all, example* ex, char* line) { return read_line(all, ex, boost::string_view(line)); }

void read_lines(vw* all, char* line, size_t /*len*/, v_array<example*>& examples)
{
  std::vector<boost::string_view> lines;
  tokenize('\n', line, lines);
  for (size_t i = 0; i < lines.size(); i++)
  {
    // Check if a new empty example needs to be added.
    if (examples.size() < i + 1)
    {
      examples.push_back(&VW::get_unused_example(all));
    }
    read_line(*all, examples[i], lines[i]);
  }
}

}  // namespace VW
