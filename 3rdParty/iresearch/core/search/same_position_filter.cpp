//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "shared.hpp"
#include "same_position_filter.hpp"
#include "term_query.hpp"
#include "conjunction.hpp"

#include "index/field_meta.hpp"

#include "analysis/token_attributes.hpp"

#include <boost/functional/hash.hpp>

NS_ROOT

template<typename Conjunction>
class same_position_iterator final : public Conjunction {
 public:
  typedef typename Conjunction::doc_iterator doc_iterator_t;
  typedef typename Conjunction::traits_t traits_t;
  typedef typename std::enable_if<
    std::is_base_of<
      detail::conjunction<doc_iterator_t, traits_t>, 
      Conjunction
    >::value, Conjunction
  >::type conjunction_t;

  typedef std::vector<position::cref> positions_t;

  same_position_iterator(
      typename conjunction_t::doc_iterators_t&& itrs,
      const order::prepared& ord,
      positions_t&& pos)
    : conjunction_t(std::move(itrs), ord),
      pos_(std::move(pos)) {
    assert(!pos_.empty());
  }

#if defined(_MSC_VER)
  #pragma warning(disable : 4706)
#elif defined (__GNUC__)
  #pragma GCC diagnostic ignored "-Wparentheses"
#endif

  virtual bool next() override {
    bool next = false;
    while(true == (next = conjunction_t::next()) && !find_same_position()) {}
    return next;
  }

#if defined(_MSC_VER)
  #pragma warning(default : 4706)
#elif defined (__GNUC__)
  #pragma GCC diagnostic pop
#endif

  virtual doc_id_t seek(doc_id_t target) override {
    const auto doc = conjunction_t::seek(target);

    if (type_limits<type_t::doc_id_t>::eof(doc) || find_same_position()) {
      return doc; 
    }

    next();
    return this->value();
  }

 private:
  bool find_same_position() {
    auto target = type_limits<type_t::pos_t>::min();

    for (auto begin = pos_.begin(), end = pos_.end(); begin != end;) {
      const position& pos = *begin;

      if (target != pos.seek(target)) {
        target = pos.value();
        if (type_limits<type_t::pos_t>::eof(target)) {
          return false;
        }
        begin = pos_.begin();
      } else {
        ++begin;
      }
    }

    return true;
  }

  positions_t pos_;
}; // same_position_iterator

// per segment terms state
typedef std::vector<reader_term_state> terms_states_t;

class same_position_query final : public filter::prepared {
 public:
  typedef states_cache<terms_states_t> states_t;
  typedef std::vector<attribute_store> stats_t;

  DECLARE_SPTR(same_position_query);

  explicit same_position_query(states_t&& states, stats_t&& stats)
    : states_(std::move(states)), stats_(std::move(stats)) {
  }

  using filter::prepared::execute;

  virtual score_doc_iterator::ptr execute(
      const sub_reader& segment,
      const order::prepared& ord) const override {
    typedef detail::conjunction<score_wrapper<
      score_doc_iterator::ptr>
    > conjunction_t;
    typedef same_position_iterator<conjunction_t> same_position_iterator_t;

    // get query state for the specified reader
    auto query_state = states_.find(segment);
    if (!query_state) {
      // invalid state 
      return score_doc_iterator::empty();
    }

    // get features required for query & order
    auto features = ord.features() | by_same_position::features();

    same_position_iterator_t::doc_iterators_t itrs;
    itrs.reserve(query_state->size());

    same_position_iterator_t::positions_t positions;
    positions.reserve(itrs.size());

    auto term_stats = stats_.begin();
    for (auto& term_state : *query_state) {
      auto term = term_state.reader->iterator();

      // use bytes_ref::nil here since we do not need just to "jump"
      // to cached state, and we are not interested in term value itself */
      if (!term->seek(bytes_ref::nil, *term_state.cookie)) {
        return score_doc_iterator::empty();
      }

      // get postings
      auto docs = term->postings(features);

      // get needed postings attributes
      auto& pos = docs->attributes().get<position>();
      if (!pos) {
        // positions not found
        return score_doc_iterator::empty();
      }
      positions.emplace_back(std::cref(*pos));

      // add base iterator
      itrs.emplace_back(score_doc_iterator::make<basic_score_iterator>(
        segment,
        *term_state.reader,
        *term_stats,
        std::move(docs), 
        ord, 
        term_state.estimation
      ));

      ++term_stats;
    }

    return detail::make_conjunction<same_position_iterator_t>(
      std::move(itrs), ord, std::move(positions)
    );
  }

 private:
  states_t states_;
  stats_t stats_;
}; // same_position_query

DEFINE_FILTER_TYPE(by_same_position);
DEFINE_FACTORY_DEFAULT(by_same_position);

/* static */ const flags& by_same_position::features() {
  static flags features{ frequency::type(), position::type() };
  return features;
}

by_same_position::by_same_position() 
  : filter(by_same_position::type()) {
}

bool by_same_position::equals(const filter& rhs) const {
  const auto& trhs = static_cast<const by_same_position&>(rhs);
  return filter::equals(rhs) && terms_ == trhs.terms_;
}

size_t by_same_position::hash() const {
  size_t seed = 0;
  ::boost::hash_combine(seed, filter::hash());
  for (auto& term : terms_) {
    ::boost::hash_combine(seed, term.first);
    ::boost::hash_combine(seed, term.second);
  }
  return seed;
}
  
by_same_position& by_same_position::push_back(
    const std::string& field, 
    const bstring& term) {
  terms_.emplace_back(field, term);
  return *this;
}

by_same_position& by_same_position::push_back(
    const std::string& field, 
    bstring&& term) {
  terms_.emplace_back(field, std::move(term));
  return *this;
}

by_same_position& by_same_position::push_back(
    std::string&& field,
    const bstring& term) {
  terms_.emplace_back(std::move(field), term);
  return *this;
}

by_same_position& by_same_position::push_back(
    std::string&& field,
    bstring&& term) {
  terms_.emplace_back(std::move(field), std::move(term));
  return *this;
}

filter::prepared::ptr by_same_position::prepare(
    const index_reader& index,
    const order::prepared& ord,
    boost_t boost) const {
  if (terms_.empty()) {
    // empty field or phrase
    return filter::prepared::empty();
  }

  // per segment query state
  same_position_query::states_t query_states(index.size());

  // per segment terms states
  terms_states_t term_states;
  term_states.reserve(terms_.size());

  // prepare phrase stats (collector for each term)
  std::vector<order::prepared::stats> query_stats;
  query_stats.reserve(terms_.size());
  for(auto size = terms_.size(); size; --size) {
    query_stats.emplace_back(ord.prepare_stats());
  }
  
  for (const auto& segment : index) {
    auto term_stats = query_stats.begin();
    for (const auto& branch : terms_) {
      // get term dictionary for field
      const term_reader* field = segment.field(branch.first);
      if (!field) {
        continue;
      }

      // check required features
      if (!features().is_subset_of(field->meta().features)) {
        continue;
      }

      // find terms
      seek_term_iterator::ptr term = field->iterator();
      // get term metadata
      auto& meta = term->attributes().get<term_meta>();

      if (!term->seek(branch.second)) {
        if (ord.empty()) {
          break;
        } else {
          // continue here because we should collect 
          // stats for other terms in phrase
          continue;
        }
      }
      
      term->read(); // read term attributes
      term_stats->field(segment, *field); // collect field stats
      term_stats->term(term->attributes()); // collect term stats

      term_states.emplace_back();
      auto& state = term_states.back();
      state.cookie = term->cookie();
      state.estimation = meta ? meta->docs_count : cost::MAX;
      state.reader = field;

      ++term_stats;
    }

    if (term_states.size() != terms_.size()) {
      // we have not found all needed terms
      term_states.clear();
      continue;
    }

    auto& state = query_states.insert(segment);
    state = std::move(term_states);
    term_states.reserve(terms_.size());
  }

  // finish stats
  same_position_query::stats_t stats(terms_.size());
  auto term_stats = query_stats.begin();
  for(auto& stat : stats) {
    term_stats->finish(index, stat);
    ++term_stats;
  }

  auto q = memory::make_unique<same_position_query>(
    std::move(query_states), std::move(stats)
  );
  
  // apply boost
  iresearch::boost::apply(q->attributes(), this->boost() * boost);

  return std::move(q);
}

NS_END // ROOT