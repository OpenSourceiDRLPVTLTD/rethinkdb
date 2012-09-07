#include "errors.hpp"
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/make_shared.hpp>

#include "btree/erase_range.hpp"
#include "btree/parallel_traversal.hpp"
#include "btree/slice.hpp"
#include "btree/superblock.hpp"
#include "concurrency/pmap.hpp"
#include "concurrency/wait_any.hpp"
#include "containers/archive/vector_stream.hpp"
#include "protob/protob.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/query_language.hpp"
#include "rpc/semilattice/view/field.hpp"
#include "serializer/config.hpp"

typedef rdb_protocol_details::backfill_atom_t rdb_backfill_atom_t;

typedef rdb_protocol_t::context_t context_t;

typedef rdb_protocol_t::store_t store_t;
typedef rdb_protocol_t::region_t region_t;

typedef rdb_protocol_t::read_t read_t;
typedef rdb_protocol_t::read_response_t read_response_t;

typedef rdb_protocol_t::point_read_t point_read_t;
typedef rdb_protocol_t::point_read_response_t point_read_response_t;

typedef rdb_protocol_t::rget_read_t rget_read_t;
typedef rdb_protocol_t::rget_read_response_t rget_read_response_t;

typedef rdb_protocol_t::distribution_read_t distribution_read_t;
typedef rdb_protocol_t::distribution_read_response_t distribution_read_response_t;

typedef rdb_protocol_t::write_t write_t;
typedef rdb_protocol_t::write_response_t write_response_t;

typedef rdb_protocol_t::point_write_t point_write_t;
typedef rdb_protocol_t::point_write_response_t point_write_response_t;

typedef rdb_protocol_t::point_modify_t point_modify_t;
typedef rdb_protocol_t::point_modify_response_t point_modify_response_t;

typedef rdb_protocol_t::point_delete_t point_delete_t;
typedef rdb_protocol_t::point_delete_response_t point_delete_response_t;

typedef rdb_protocol_t::backfill_chunk_t backfill_chunk_t;

typedef rdb_protocol_t::backfill_progress_t backfill_progress_t;

typedef rdb_protocol_t::rget_read_response_t::stream_t stream_t;
typedef rdb_protocol_t::rget_read_response_t::groups_t groups_t;
typedef rdb_protocol_t::rget_read_response_t::atom_t atom_t;
typedef rdb_protocol_t::rget_read_response_t::length_t length_t;
typedef rdb_protocol_t::rget_read_response_t::inserted_t inserted_t;
typedef rdb_protocol_t::rget_read_response_t::runtime_exc_t runtime_exc_t;

const std::string rdb_protocol_t::protocol_name("rdb");

RDB_IMPL_PROTOB_SERIALIZABLE(Builtin_Range);
RDB_IMPL_PROTOB_SERIALIZABLE(Builtin_Filter);
RDB_IMPL_PROTOB_SERIALIZABLE(Builtin_Map);
RDB_IMPL_PROTOB_SERIALIZABLE(Builtin_ConcatMap);
RDB_IMPL_PROTOB_SERIALIZABLE(Builtin_GroupedMapReduce);
RDB_IMPL_PROTOB_SERIALIZABLE(Mapping);
RDB_IMPL_PROTOB_SERIALIZABLE(Reduction);
RDB_IMPL_PROTOB_SERIALIZABLE(WriteQuery_ForEach);

rdb_protocol_t::context_t::context_t()
    : pool_group(NULL), ns_repo(NULL),
    cross_thread_namespace_watchables(get_num_threads()),
    cross_thread_database_watchables(get_num_threads()),
    signals(get_num_threads())
{ }

rdb_protocol_t::context_t::context_t(extproc::pool_group_t *_pool_group,
          namespace_repo_t<rdb_protocol_t> *_ns_repo,
          boost::shared_ptr<semilattice_readwrite_view_t<cluster_semilattice_metadata_t> > _semilattice_metadata,
          machine_id_t _machine_id)
    : pool_group(_pool_group), ns_repo(_ns_repo),
      cross_thread_namespace_watchables(get_num_threads()),
      cross_thread_database_watchables(get_num_threads()),
      semilattice_metadata(_semilattice_metadata),
      signals(get_num_threads()),
      machine_id(_machine_id)
{
    for (int thread = 0; thread < get_num_threads(); ++thread) {
        cross_thread_namespace_watchables[thread].init(new cross_thread_watchable_variable_t<namespaces_semilattice_metadata_t<rdb_protocol_t> >(
                                                    clone_ptr_t<semilattice_watchable_t<namespaces_semilattice_metadata_t<rdb_protocol_t> > >
                                                        (new semilattice_watchable_t<namespaces_semilattice_metadata_t<rdb_protocol_t> >(
                                                            metadata_field(&cluster_semilattice_metadata_t::rdb_namespaces, _semilattice_metadata))), thread));

        cross_thread_database_watchables[thread].init(new cross_thread_watchable_variable_t<databases_semilattice_metadata_t>(
                                                    clone_ptr_t<semilattice_watchable_t<databases_semilattice_metadata_t> >
                                                        (new semilattice_watchable_t<databases_semilattice_metadata_t>(
                                                            metadata_field(&cluster_semilattice_metadata_t::databases, _semilattice_metadata))), thread));

        signals[thread].init(new cross_thread_signal_t(&interruptor, thread));
    }
}

// Construct a region containing only the specified key
region_t rdb_protocol_t::monokey_region(const store_key_t &k) {
    uint64_t h = hash_region_hasher(k.contents(), k.size());
    return region_t(h, h + 1, key_range_t(key_range_t::closed, k, key_range_t::closed, k));
}

namespace {

/* read_t::get_region implementation */
struct r_get_region_visitor : public boost::static_visitor<region_t> {
    region_t operator()(const point_read_t &pr) const {
        return rdb_protocol_t::monokey_region(pr.key);
    }

    region_t operator()(const rget_read_t &rg) const {
        // TODO: Sam bets this causes problems
        return region_t(rg.key_range);
    }

    region_t operator()(const distribution_read_t &dg) const {
        return region_t(dg.range);
    }
};

}   /* anonymous namespace */

region_t read_t::get_region() const THROWS_NOTHING {
    return boost::apply_visitor(r_get_region_visitor(), read);
}

/* read_t::shard implementation */

namespace {

struct r_shard_visitor : public boost::static_visitor<read_t> {
    explicit r_shard_visitor(const region_t &_region)
        : region(_region)
    { }

    read_t operator()(const point_read_t &pr) const {
        rassert(rdb_protocol_t::monokey_region(pr.key) == region);
        return read_t(pr);
    }

    read_t operator()(const rget_read_t &rg) const {
        rassert(region_is_superset(region_t(rg.key_range), region));
        // TODO: Reevaluate this code.  Should rget_query_t really have a region_t range?
        rget_read_t _rg(rg);
        _rg.key_range = region.inner;
        return read_t(_rg);
    }

    read_t operator()(const distribution_read_t &dg) const {
        rassert(region_is_superset(region_t(dg.range), region));

        // TODO: Reevaluate this code.  Should distribution_get_query_t really have a key_range_t range?
        distribution_read_t _dg(dg);
        _dg.range = region.inner;
        return read_t(_dg);
    }

    const region_t &region;
};

}   /* anonymous namespace */

read_t read_t::shard(const region_t &region) const THROWS_NOTHING {
    return boost::apply_visitor(r_shard_visitor(region), read);
}

/* read_t::unshard implementation */
bool read_response_cmp(const read_response_t &l, const read_response_t &r) {
    const rget_read_response_t *lr = boost::get<rget_read_response_t>(&l.response);
    rassert(lr);
    const rget_read_response_t *rr = boost::get<rget_read_response_t>(&r.response);
    rassert(rr);
    return lr->key_range < rr->key_range;
}

/* A visitor to handle this unsharding process for us. */

class unshard_visitor_t : public boost::static_visitor<void> {
public:
    unshard_visitor_t(const std::vector<read_response_t> &_responses,
                      read_response_t *_response_out, context_t *ctx)
        : responses(_responses), response_out(_response_out),
          env(ctx->pool_group,
              ctx->ns_repo,
              ctx->cross_thread_namespace_watchables[get_thread_id()].get()->get_watchable(),
              ctx->cross_thread_database_watchables[get_thread_id()].get()->get_watchable(),
              ctx->semilattice_metadata,
              boost::make_shared<js::runner_t>(),
              ctx->signals[get_thread_id()].get(),
              ctx->machine_id)
    { }

    void operator()(const point_read_t &) {
        rassert(responses.size() == 1);
        rassert(boost::get<point_read_response_t>(&responses[0].response));
        *response_out = responses[0];
    }

    void operator()(const rget_read_t &rg) {
        env.scopes = rg.scopes;
        response_out->response = rget_read_response_t();
        rget_read_response_t &rg_response = boost::get<rget_read_response_t>(response_out->response);
        rg_response.truncated = false;
        rg_response.key_range = read_t(rg).get_region().inner;
        rg_response.last_considered_key = read_t(rg).get_region().inner.left;
        typedef std::vector<read_response_t>::const_iterator rri_t;

        try {
            /* First check to see if any of the responses we're unsharding threw. */
            for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                // TODO: we're ignoring the limit when recombining.
                const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                rassert(_rr);

                if (const runtime_exc_t *e = boost::get<runtime_exc_t>(&(_rr->result))) {
                    throw *e;
                }
            }

            if (!rg.terminal) {
                //A vanilla range get
                rg_response.result = stream_t();
                stream_t *res_stream = boost::get<stream_t>(&rg_response.result);
                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    // TODO: we're ignoring the limit when recombining.
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    rassert(_rr);

                    const stream_t *stream = boost::get<stream_t>(&(_rr->result));

                    res_stream->insert(res_stream->end(), stream->begin(), stream->end());
                    rg_response.truncated = rg_response.truncated || _rr->truncated;
                    if (rg_response.last_considered_key < _rr->last_considered_key) {
                        rg_response.last_considered_key = _rr->last_considered_key;
                    }
                }
            } else if (const Builtin_GroupedMapReduce *gmr = boost::get<Builtin_GroupedMapReduce>(&*rg.terminal)) {
                //GroupedMapreduce
                rg_response.result = groups_t();
                groups_t *res_groups = boost::get<groups_t>(&rg_response.result);
                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const groups_t *groups = boost::get<groups_t>(&(_rr->result));
                    query_language::backtrace_t backtrace;

                    for (groups_t::const_iterator j = groups->begin(); j != groups->end(); ++j) {
                        query_language::new_val_scope_t scope(&env.scopes.scope);
                        Term base = gmr->reduction().base(),
                             body = gmr->reduction().body();

                        env.scopes.scope.put_in_scope(gmr->reduction().var1(), get_with_default(*res_groups, j->first, eval(&base, &env, backtrace)));
                        env.scopes.scope.put_in_scope(gmr->reduction().var2(), j->second);

                        (*res_groups)[j->first] = eval(&body, &env, backtrace);
                    }
                }
            } else if (const Reduction *r = boost::get<Reduction>(&*rg.terminal)) {
                //Normal Mapreduce
                rg_response.result = atom_t();
                atom_t *res_atom = boost::get<atom_t>(&rg_response.result);

                query_language::backtrace_t backtrace;

                Term base = r->base();
                *res_atom = eval(&base, &env, backtrace);

                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const atom_t *atom = boost::get<atom_t>(&(_rr->result));

                    query_language::new_val_scope_t scope(&env.scopes.scope);
                    env.scopes.scope.put_in_scope(r->var1(), *res_atom);
                    env.scopes.scope.put_in_scope(r->var2(), *atom);
                    Term body = r->body();
                    *res_atom = eval(&body, &env, backtrace);
                }
            } else if (boost::get<rdb_protocol_details::Length>(&*rg.terminal)) {
                rg_response.result = atom_t();
                length_t *res_length = boost::get<length_t>(&rg_response.result);
                res_length->length = 0;

                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const length_t *length = boost::get<length_t>(&(_rr->result));

                    res_length->length += length->length;
                }
            } else if (boost::get<WriteQuery_ForEach>(&*rg.terminal)) {
                rg_response.result = atom_t();
                inserted_t *res_inserted = boost::get<inserted_t>(&rg_response.result);
                res_inserted->inserted = 0;

                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const inserted_t *inserted = boost::get<inserted_t>(&(_rr->result));

                    res_inserted->inserted += inserted->inserted;
                }
            } else {
                unreachable();
            }
        } catch (const runtime_exc_t &e) {
            rg_response.result = e;
        }

    }

    void operator()(const distribution_read_t &) {
        rassert(responses.size() > 0);
        rassert(boost::get<distribution_read_response_t>(&responses[0].response));
        rassert(responses.size() == 1 || boost::get<distribution_read_response_t>(&responses[1].response));

        // Asserts that we don't look like a hash-sharded thing.
        rassert(!(responses.size() > 1
                  && boost::get<distribution_read_response_t>(&responses[0].response)->key_counts.begin()->first == boost::get<distribution_read_response_t>(&responses[1].response)->key_counts.begin()->first));

        response_out->response = distribution_read_response_t();
        distribution_read_response_t *response = boost::get<distribution_read_response_t>(&response_out->response);

        for (int i = 0, e = responses.size(); i < e; i++) {
            const distribution_read_response_t *response_piece = boost::get<distribution_read_response_t>(&responses[i].response);
            rassert(response_piece, "Bad boost::get\n");

#ifndef NDEBUG
            for (std::map<store_key_t, int>::const_iterator it = response_piece->key_counts.begin();
                 it != response_piece->key_counts.end();
                 ++it) {
                rassert(!std_contains(response->key_counts, it->first), "repeated key '%*.*s'",
                        static_cast<int>(it->first.size()), static_cast<int>(it->first.size()), it->first.contents());
            }
#endif
            response->key_counts.insert(response_piece->key_counts.begin(), response_piece->key_counts.end());

        }
    }

private:
    const std::vector<read_response_t> &responses;
    read_response_t *response_out;
    query_language::runtime_environment_t env;
};

void read_t::unshard(std::vector<read_response_t> responses, read_response_t *response, context_t *ctx) const THROWS_NOTHING {
    unshard_visitor_t v(responses, response, ctx);
    boost::apply_visitor(v, read);
}

bool rget_data_cmp(const std::pair<store_key_t, boost::shared_ptr<scoped_cJSON_t> >& a,
                   const std::pair<store_key_t, boost::shared_ptr<scoped_cJSON_t> >& b) {
    return a.first < b.first;
}

class multistore_unshard_visitor_t : public boost::static_visitor<void> {
public:
    multistore_unshard_visitor_t(const std::vector<read_response_t> &_responses, 
                                 read_response_t *_response_out, context_t *ctx) 
        : responses(_responses), response_out(_response_out),
          env(ctx->pool_group,
              ctx->ns_repo,
              ctx->cross_thread_namespace_watchables[get_thread_id()].get()->get_watchable(),
              ctx->cross_thread_database_watchables[get_thread_id()].get()->get_watchable(),
              ctx->semilattice_metadata,
              boost::make_shared<js::runner_t>(),
              ctx->signals[get_thread_id()].get(),
              ctx->machine_id)
    { }

    void operator()(const point_read_t &) {
        rassert(responses.size() == 1);
        rassert(boost::get<point_read_response_t>(&responses[0].response));
        *response_out = responses[0];
    }

    void operator()(const rget_read_t &rg) {
        env.scopes = rg.scopes;
        response_out->response = rget_read_response_t();
        rget_read_response_t &rg_response = boost::get<rget_read_response_t>(response_out->response);
        rg_response.truncated = false;
        rg_response.key_range = read_t(rg).get_region().inner;
        rg_response.last_considered_key = read_t(rg).get_region().inner.left;
        typedef std::vector<read_response_t>::const_iterator rri_t;

        try {
            /* First check to see if any of the responses we're unsharding threw. */
            for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                // TODO: we're ignoring the limit when recombining.
                const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                rassert(_rr);

                if (const runtime_exc_t *e = boost::get<runtime_exc_t>(&(_rr->result))) {
                    throw *e;
                }
            }

            if (!rg.terminal) {
                //A vanilla range get (or filter or map)
                rg_response.result = stream_t(); //Set the response to have the correct result type
                stream_t *res_stream = boost::get<stream_t>(&rg_response.result);

                /* An annoyance occurs. We have results from several different hash
                 * shards. We must figure out what the last considered key is,
                 * however that value must be the last considered key for all of
                 * the hash shards, thus we have to take the minimum of all the
                 * shards last considered keys. Observe the picture:
                 *
                 *              A - - - - - - - - - - - - - - - Z
                 * hash shard 1     | -        - -  -  -  |
                 * hash shard 2     |  --       -    -   -|
                 * hash shard 3     |     --- -   -       |
                 * hash shard 4     |-   -         -  - - |
                 *
                 * Here each shard has returned 5 keys. (Each - is a key). Now the
                 * question is what is the last considered key?
                 *
                 *              A - - - - - - - - - - - - - - - Z
                 * hash shard 1     | -        - -  -  -  |
                 * hash shard 2     |  --       -    -   a|
                 * hash shard 3     |     --- -   b       |
                 * hash shard 4     |-   -         -  - - |
                 *
                 * Is it "a" or "b"? The answer is "b"? If we picked "a" then the
                 * next request we got would have "a" as the left side of the
                 * range. And we could miss keys in hash shard 3.
                 */

                /* Figure out what the last considered key actually is. */
                rg_response.last_considered_key = read_t(rg).get_region().inner.last_key_in_range();

                for (rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const stream_t *stream = boost::get<stream_t>(&(_rr->result));
                    guarantee(stream);


                    if (stream->size() == rg.maximum) {
                        if (_rr->last_considered_key < rg_response.last_considered_key) {
                            rg_response.last_considered_key = _rr->last_considered_key;
                        }
                    }
                }

                for (rri_t i = responses.begin(); i != responses.end(); ++i) {
                    // TODO: we're ignoring the limit when recombining.
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    rassert(_rr);

                    const stream_t *stream = boost::get<stream_t>(&(_rr->result));

                    for (stream_t::const_iterator jt  = stream->begin();
                                                  jt != stream->end();
                                                  ++jt) {
                        //Filter out the results that went past our last considered key
                        if (jt->first <= rg_response.last_considered_key) {
                            res_stream->push_back(*jt);
                        }
                    }

                    //res_stream->insert(res_stream->end(), stream->begin(), stream->end());
                    rg_response.truncated = rg_response.truncated || _rr->truncated;
                }
            } else if (const Builtin_GroupedMapReduce *gmr = boost::get<Builtin_GroupedMapReduce>(&*rg.terminal)) {
                //GroupedMapreduce
                rg_response.result = groups_t();
                groups_t *res_groups = boost::get<groups_t>(&rg_response.result);
                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const groups_t *groups = boost::get<groups_t>(&(_rr->result));
                    query_language::backtrace_t backtrace;

                    for (groups_t::const_iterator j = groups->begin(); j != groups->end(); ++j) {
                        query_language::new_val_scope_t scope(&env.scopes.scope);
                        Term base = gmr->reduction().base(),
                             body = gmr->reduction().body();

                        env.scopes.scope.put_in_scope(gmr->reduction().var1(), get_with_default(*res_groups, j->first, eval(&base, &env, backtrace)));
                        env.scopes.scope.put_in_scope(gmr->reduction().var2(), j->second);

                        (*res_groups)[j->first] = eval(&body, &env, backtrace);
                    }
                }
            } else if (const Reduction *r = boost::get<Reduction>(&*rg.terminal)) {
                //Normal Mapreduce
                rg_response.result = atom_t();
                atom_t *res_atom = boost::get<atom_t>(&rg_response.result);

                query_language::backtrace_t backtrace;

                Term base = r->base();
                *res_atom = eval(&base, &env, backtrace);

                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const atom_t *atom = boost::get<atom_t>(&(_rr->result));

                    query_language::new_val_scope_t scope(&env.scopes.scope);
                    env.scopes.scope.put_in_scope(r->var1(), *res_atom);
                    env.scopes.scope.put_in_scope(r->var2(), *atom);
                    Term body = r->body();
                    *res_atom = eval(&body, &env, backtrace);
                }
            } else if (boost::get<rdb_protocol_details::Length>(&*rg.terminal)) {
                rg_response.result = atom_t();
                length_t *res_length = boost::get<length_t>(&rg_response.result);
                res_length->length = 0;

                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const length_t *length = boost::get<length_t>(&(_rr->result));

                    res_length->length += length->length;
                }
            } else if (boost::get<WriteQuery_ForEach>(&*rg.terminal)) {
                rg_response.result = atom_t();
                inserted_t *res_inserted = boost::get<inserted_t>(&rg_response.result);
                res_inserted->inserted = 0;

                for(rri_t i = responses.begin(); i != responses.end(); ++i) {
                    const rget_read_response_t *_rr = boost::get<rget_read_response_t>(&i->response);
                    guarantee(_rr);

                    const inserted_t *inserted = boost::get<inserted_t>(&(_rr->result));

                    res_inserted->inserted += inserted->inserted;
                }
            } else {
                unreachable();
            }
        } catch (const runtime_exc_t &e) {
            rg_response.result = e;
        }
    }

    void operator()(const distribution_read_t &) {
        rassert(responses.size() > 0);
        rassert(boost::get<distribution_read_response_t>(&responses[0].response));
        rassert(responses.size() == 1 || boost::get<distribution_read_response_t>(&responses[1].response));

        // These test properties of distribution queries sharded by hash rather than key.
        rassert(responses.size() > 1);
        rassert(boost::get<distribution_read_response_t>(&responses[0].response)->key_counts.begin()->first == boost::get<distribution_read_response_t>(&responses[1].response)->key_counts.begin()->first);

        response_out->response = distribution_read_response_t();
        distribution_read_response_t *response = boost::get<distribution_read_response_t>(&response_out->response);

        int64_t total_num_keys = 0;
        rassert(responses.size() > 0);

        int64_t total_keys_in_res = 0;
        for (int i = 0, e = responses.size(); i < e; ++i) {
            const distribution_read_response_t *response_piece = boost::get<distribution_read_response_t>(&responses[i].response);
            rassert(response_piece, "Bad boost::get\n");

            int64_t tmp_total_keys = 0;
            for (std::map<store_key_t, int>::const_iterator it = response_piece->key_counts.begin();
                 it != response_piece->key_counts.end();
                 ++it) {
                tmp_total_keys += it->second;
            }

            total_num_keys += tmp_total_keys;

            if (response->key_counts.size() < response_piece->key_counts.size()) {
                *response = *response_piece;
                total_keys_in_res = tmp_total_keys;
            }
        }

        if (total_keys_in_res == 0) {
            return;
        }

        double scale_factor = static_cast<double>(total_num_keys) / static_cast<double>(total_keys_in_res);

        rassert(scale_factor >= 1.0);  // Directly provable from the code above.

        for (std::map<store_key_t, int>::iterator it  = response->key_counts.begin();
                                                  it != response->key_counts.end();
                                                  ++it) {
            it->second = static_cast<int>(it->second * scale_factor);
        }
    }

private:
    const std::vector<read_response_t> &responses;
    read_response_t *response_out;
    query_language::runtime_environment_t env;
};

void read_t::multistore_unshard(std::vector<read_response_t> responses, read_response_t *response, context_t *ctx) const THROWS_NOTHING {
    multistore_unshard_visitor_t v(responses, response, ctx);
    boost::apply_visitor(v, read);
}


/* write_t::get_region() implementation */

namespace {

struct w_get_region_visitor : public boost::static_visitor<region_t> {
    region_t operator()(const point_write_t &pw) const {
        return rdb_protocol_t::monokey_region(pw.key);
    }

    region_t operator()(const point_modify_t &pw) const {
        return rdb_protocol_t::monokey_region(pw.key);
    }

    region_t operator()(const point_delete_t &pd) const {
        return rdb_protocol_t::monokey_region(pd.key);
    }
};

}   /* anonymous namespace */

region_t write_t::get_region() const THROWS_NOTHING {
    return boost::apply_visitor(w_get_region_visitor(), write);
}

/* write_t::shard implementation */

namespace {

struct w_shard_visitor : public boost::static_visitor<write_t> {
    explicit w_shard_visitor(const region_t &_region)
        : region(_region)
    { }

    write_t operator()(const point_write_t &pw) const {
        rassert(rdb_protocol_t::monokey_region(pw.key) == region);
        return write_t(pw);
    }
    write_t operator()(const point_modify_t &pw) const {
        rassert(rdb_protocol_t::monokey_region(pw.key) == region);
        return write_t(pw);
    }
    write_t operator()(const point_delete_t &pd) const {
        rassert(rdb_protocol_t::monokey_region(pd.key) == region);
        return write_t(pd);
    }
    const region_t &region;
};

}   /* anonymous namespace */

write_t write_t::shard(const region_t &region) const THROWS_NOTHING {
    return boost::apply_visitor(w_shard_visitor(region), write);
}

void write_t::unshard(std::vector<write_response_t> responses, write_response_t *response, context_t *) const THROWS_NOTHING {
    rassert(responses.size() == 1);
    *response = responses[0];
}

void write_t::multistore_unshard(const std::vector<write_response_t>& responses, write_response_t *response, context_t *ctx) const THROWS_NOTHING {
    return unshard(responses, response, ctx);
}

store_t::store_t(io_backender_t *io_backend,
                 const std::string& filename,
                 bool create,
                 perfmon_collection_t *parent_perfmon_collection,
                 context_t *_ctx) :
    btree_store_t<rdb_protocol_t>(io_backend, filename, create, parent_perfmon_collection, _ctx),
    ctx(_ctx)
{ }

store_t::~store_t() {
    assert_thread();
}

namespace {

// TODO: get rid of this extra response_t copy on the stack
struct read_visitor_t : public boost::static_visitor<read_response_t> {
    read_response_t operator()(const point_read_t &get) {
        return read_response_t(rdb_get(get.key, btree, txn, superblock));
    }

    read_response_t operator()(const rget_read_t &rget) {
        env.scopes = rget.scopes;
        return read_response_t(rdb_rget_slice(btree, rget.key_range, 1000, txn, superblock, &env, rget.transform, rget.terminal));
    }

    read_response_t operator()(const distribution_read_t &dg) {
        distribution_read_response_t dstr = rdb_distribution_get(btree, dg.max_depth, dg.range.left, txn, superblock);
        for (std::map<store_key_t, int>::iterator it  = dstr.key_counts.begin();
                                                  it != dstr.key_counts.end();
                                                  /* increments done in loop */) {
            if (!dg.range.contains_key(store_key_t(it->first))) {
                dstr.key_counts.erase(it++);
            } else {
                ++it;
            }
        }

        return read_response_t(dstr);
    }

    read_visitor_t(btree_slice_t *btree_, transaction_t *txn_, superblock_t *superblock_, rdb_protocol_t::context_t *ctx) :
        btree(btree_), txn(txn_), superblock(superblock_),
        env(ctx->pool_group,
            ctx->ns_repo,
            ctx->cross_thread_namespace_watchables[get_thread_id()].get()->get_watchable(),
            ctx->cross_thread_database_watchables[get_thread_id()].get()->get_watchable(),
            ctx->semilattice_metadata,
            boost::make_shared<js::runner_t>(),
            ctx->signals[get_thread_id()].get(),
            ctx->machine_id)
    { }

private:
    btree_slice_t *btree;
    transaction_t *txn;
    superblock_t *superblock;
    query_language::runtime_environment_t env;
};

}   /* anonymous namespace */

void store_t::protocol_read(const read_t &read,
                            read_response_t *response,
                            btree_slice_t *btree,
                            transaction_t *txn,
                            superblock_t *superblock) {
    read_visitor_t v(btree, txn, superblock, ctx);
    *response = boost::apply_visitor(v, read.read);
}

namespace {

// TODO: get rid of this extra response_t copy on the stack
struct write_visitor_t : public boost::static_visitor<write_response_t> {
    write_response_t operator()(const point_write_t &w) {
        return write_response_t(
            rdb_set(w.key, w.data, btree, timestamp, txn, superblock));
    }

    write_response_t operator()(const point_modify_t &m) {
        debugf("BEGIN modify...\n");
        m.key.debug_print();
        debugf("%s\n", m.primary_key.c_str());
        env.scopes = m.scopes;
        write_response_t res(rdb_modify(m.primary_key, m.key, m.op, &env, m.mapping, btree, timestamp, txn, superblock));
        debugf("END modify...\n");
        return res;
    }

    write_response_t operator()(const point_delete_t &d) {
        return write_response_t(
            rdb_delete(d.key, btree, timestamp, txn, superblock));
    }

    write_visitor_t(btree_slice_t *btree_, transaction_t *txn_, superblock_t *superblock_,
                    repli_timestamp_t timestamp_, rdb_protocol_t::context_t *ctx) :
        btree(btree_), txn(txn_), superblock(superblock_), timestamp(timestamp_),
        env(ctx->pool_group,
            ctx->ns_repo,
            ctx->cross_thread_namespace_watchables[get_thread_id()].get()->get_watchable(),
            ctx->cross_thread_database_watchables[get_thread_id()].get()->get_watchable(),
            ctx->semilattice_metadata,
            boost::make_shared<js::runner_t>(),
            ctx->signals[get_thread_id()].get(),
            ctx->machine_id)
    { }

private:
    btree_slice_t *btree;
    transaction_t *txn;
    superblock_t *superblock;
    repli_timestamp_t timestamp;
    query_language::runtime_environment_t env;
};

}   /* anonymous namespace */

void store_t::protocol_write(const write_t &write,
                             write_response_t *response,
                             transition_timestamp_t timestamp,
                             btree_slice_t *btree,
                             transaction_t *txn,
                             superblock_t *superblock) {
    write_visitor_t v(btree, txn, superblock, timestamp.to_repli_timestamp(), ctx);
    *response = boost::apply_visitor(v, write.write);
}

namespace {

struct backfill_chunk_get_region_visitor_t : public boost::static_visitor<region_t> {
    region_t operator()(const backfill_chunk_t::delete_key_t &del) {
        return rdb_protocol_t::monokey_region(del.key);
    }

    region_t operator()(const backfill_chunk_t::delete_range_t &del) {
        return del.range;
    }

    region_t operator()(const backfill_chunk_t::key_value_pair_t &kv) {
        return rdb_protocol_t::monokey_region(kv.backfill_atom.key);
    }
};

}   /* anonymous namespace */

region_t backfill_chunk_t::get_region() const {
    backfill_chunk_get_region_visitor_t v;
    return boost::apply_visitor(v, val);
}

namespace {

struct backfill_chunk_get_btree_repli_timestamp_visitor_t : public boost::static_visitor<repli_timestamp_t> {
    repli_timestamp_t operator()(const backfill_chunk_t::delete_key_t &del) {
        return del.recency;
    }

    repli_timestamp_t operator()(const backfill_chunk_t::delete_range_t &) {
        return repli_timestamp_t::invalid;
    }

    repli_timestamp_t operator()(const backfill_chunk_t::key_value_pair_t &kv) {
        return kv.backfill_atom.recency;
    }
};

}   /* anonymous namespace */

repli_timestamp_t backfill_chunk_t::get_btree_repli_timestamp() const THROWS_NOTHING {
    backfill_chunk_get_btree_repli_timestamp_visitor_t v;
    return boost::apply_visitor(v, val);
}

struct rdb_backfill_callback_impl_t : public rdb_backfill_callback_t {
public:
    typedef backfill_chunk_t chunk_t;

    explicit rdb_backfill_callback_impl_t(chunk_fun_callback_t<rdb_protocol_t> *_chunk_fun_cb)
        : chunk_fun_cb(_chunk_fun_cb) { }
    ~rdb_backfill_callback_impl_t() { }

    void on_delete_range(const key_range_t &range, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        chunk_fun_cb->send_chunk(chunk_t::delete_range(region_t(range)), interruptor);
    }

    void on_deletion(const btree_key_t *key, repli_timestamp_t recency, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        chunk_fun_cb->send_chunk(chunk_t::delete_key(to_store_key(key), recency), interruptor);
    }

    void on_keyvalue(const rdb_backfill_atom_t& atom, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        chunk_fun_cb->send_chunk(chunk_t::set_key(atom), interruptor);
    }

protected:
    store_key_t to_store_key(const btree_key_t *key) {
        return store_key_t(key->size, key->contents);
    }

private:
    chunk_fun_callback_t<rdb_protocol_t> *chunk_fun_cb;

    DISABLE_COPYING(rdb_backfill_callback_impl_t);
};

static void call_rdb_backfill(int i, btree_slice_t *btree, const std::vector<std::pair<region_t, state_timestamp_t> > &regions,
        rdb_backfill_callback_t *callback, transaction_t *txn, superblock_t *superblock, backfill_progress_t *progress,
        signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    parallel_traversal_progress_t *p = new parallel_traversal_progress_t;
    scoped_ptr_t<traversal_progress_t> p_owned(p);
    progress->add_constituent(&p_owned);
    repli_timestamp_t timestamp = regions[i].second.to_repli_timestamp();
    try {
        rdb_backfill(btree, regions[i].first.inner, timestamp, callback, txn, superblock, p, interruptor);
    } catch (interrupted_exc_t) {
        /* do nothing; `protocol_send_backfill()` will notice that interruptor
        has been pulsed */
    }
}

void store_t::protocol_send_backfill(const region_map_t<rdb_protocol_t, state_timestamp_t> &start_point,
                                     chunk_fun_callback_t<rdb_protocol_t> *chunk_fun_cb,
                                     superblock_t *superblock,
                                     btree_slice_t *btree,
                                     transaction_t *txn,
                                     backfill_progress_t *progress,
                                     signal_t *interruptor)
                                     THROWS_ONLY(interrupted_exc_t) {
    rdb_backfill_callback_impl_t callback(chunk_fun_cb);
    std::vector<std::pair<region_t, state_timestamp_t> > regions(start_point.begin(), start_point.end());
    refcount_superblock_t refcount_wrapper(superblock, regions.size());
    pmap(regions.size(), boost::bind(&call_rdb_backfill, _1,
        btree, regions, &callback, txn, &refcount_wrapper, progress, interruptor));

    /* If interruptor was pulsed, `call_rdb_backfill()` exited silently, so we
    have to check directly. */
    if (interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }
}

namespace {

struct receive_backfill_visitor_t : public boost::static_visitor<> {
    receive_backfill_visitor_t(btree_slice_t *btree_, transaction_t *txn_, superblock_t *superblock_, signal_t *interruptor_) :
      btree(btree_), txn(txn_), superblock(superblock_), interruptor(interruptor_) { }

    void operator()(const backfill_chunk_t::delete_key_t& delete_key) const {
        rdb_delete(delete_key.key, btree, delete_key.recency, txn, superblock);
    }

    void operator()(const backfill_chunk_t::delete_range_t& delete_range) const {
        range_key_tester_t tester(delete_range.range);
        rdb_erase_range(btree, &tester, delete_range.range.inner, txn, superblock);
    }

    void operator()(const backfill_chunk_t::key_value_pair_t& kv) const {
        const rdb_backfill_atom_t& bf_atom = kv.backfill_atom;
        rdb_set(bf_atom.key, bf_atom.value,
                btree, bf_atom.recency,
                txn, superblock);
    }

private:
    /* TODO: This might be redundant. I thought that `key_tester_t` was only
    originally necessary because in v1.1.x the hashing scheme might be different
    between the source and destination machines. */
    struct range_key_tester_t : public key_tester_t {
        explicit range_key_tester_t(const region_t& delete_range_) : delete_range(delete_range_) { }
        bool key_should_be_erased(const btree_key_t *key) {
            uint64_t h = hash_region_hasher(key->contents, key->size);
            return delete_range.beg <= h && h < delete_range.end
                && delete_range.inner.contains_key(key->contents, key->size);
        }

        const region_t& delete_range;
    };

    btree_slice_t *btree;
    transaction_t *txn;
    superblock_t *superblock;
    signal_t *interruptor;  // FIXME: interruptors are not used in btree code, so this one ignored.
};

}   /* anonymous namespace */

void store_t::protocol_receive_backfill(btree_slice_t *btree,
                                        transaction_t *txn,
                                        superblock_t *superblock,
                                        signal_t *interruptor,
                                        const backfill_chunk_t &chunk) {
    boost::apply_visitor(receive_backfill_visitor_t(btree, txn, superblock, interruptor), chunk.val);
}

void store_t::protocol_reset_data(const region_t& subregion,
                                  btree_slice_t *btree,
                                  transaction_t *txn,
                                  superblock_t *superblock) {
    always_true_key_tester_t key_tester;
    rdb_erase_range(btree, &key_tester, subregion.inner, txn, superblock);
}

region_t rdb_protocol_t::cpu_sharding_subspace(int subregion_number, int num_cpu_shards) {
    rassert(subregion_number >= 0);
    rassert(subregion_number < num_cpu_shards);

    // We have to be careful with the math here, to avoid overflow.
    uint64_t width = HASH_REGION_HASH_SIZE / num_cpu_shards;

    uint64_t beg = width * subregion_number;
    uint64_t end = subregion_number + 1 == num_cpu_shards ? HASH_REGION_HASH_SIZE : beg + width;

    return region_t(beg, end, key_range_t::universe());
}

namespace {

struct backfill_chunk_shard_visitor_t : public boost::static_visitor<rdb_protocol_t::backfill_chunk_t> {
public:
    explicit backfill_chunk_shard_visitor_t(const rdb_protocol_t::region_t &_region) : region(_region) { }
    rdb_protocol_t::backfill_chunk_t operator()(const rdb_protocol_t::backfill_chunk_t::delete_key_t &del) {
        rdb_protocol_t::backfill_chunk_t ret(del);
        rassert(region_is_superset(region, ret.get_region()));
        return ret;
    }
    rdb_protocol_t::backfill_chunk_t operator()(const rdb_protocol_t::backfill_chunk_t::delete_range_t &del) {
        rdb_protocol_t::region_t r = region_intersection(del.range, region);
        rassert(!region_is_empty(r));
        return rdb_protocol_t::backfill_chunk_t(rdb_protocol_t::backfill_chunk_t::delete_range_t(r));
    }
    rdb_protocol_t::backfill_chunk_t operator()(const rdb_protocol_t::backfill_chunk_t::key_value_pair_t &kv) {
        rdb_protocol_t::backfill_chunk_t ret(kv);
        rassert(region_is_superset(region, ret.get_region()));
        return ret;
    }
private:
    const rdb_protocol_t::region_t &region;

    DISABLE_COPYING(backfill_chunk_shard_visitor_t);
};

}   /* anonymous namespace */

rdb_protocol_t::backfill_chunk_t rdb_protocol_t::backfill_chunk_t::shard(const rdb_protocol_t::region_t &region) const THROWS_NOTHING {
    backfill_chunk_shard_visitor_t v(region);
    return boost::apply_visitor(v, val);
}
