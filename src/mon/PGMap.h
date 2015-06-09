// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */
 
/*
 * Placement Group Map. Placement Groups are logical sets of objects
 * that are replicated by the same set of devices. pgid=(r,hash(o)&m)
 * where & is a bit-wise AND and m=2^k-1
 */

#ifndef CEPH_PGMAP_H
#define CEPH_PGMAP_H

#include "common/debug.h"
#include "osd/osd_types.h"
#include "common/config.h"
#include <sstream>

#include "MonitorDBStore.h"

namespace ceph { class Formatter; }

class PGMap {
public:
  // the map
  version_t version;
  epoch_t last_osdmap_epoch;   // last osdmap epoch i applied to the pgmap
  epoch_t last_pg_scan;  // osdmap epoch
  ceph::unordered_map<pg_t,pg_stat_t> pg_stat;
  ceph::unordered_map<int32_t,osd_stat_t> osd_stat;
  set<int32_t> full_osds;
  set<int32_t> nearfull_osds;
  float full_ratio;
  float nearfull_ratio;

  // mapping of osd to most recently reported osdmap epoch
  ceph::unordered_map<int32_t,epoch_t> osd_epochs;

  class Incremental {
  public:
    version_t version;
    map<pg_t,pg_stat_t> pg_stat_updates;
    epoch_t osdmap_epoch;
    epoch_t pg_scan;  // osdmap epoch
    set<pg_t> pg_remove;
    float full_ratio;
    float nearfull_ratio;
    utime_t stamp;

  private:
    map<int32_t,osd_stat_t> osd_stat_updates;
    set<int32_t> osd_stat_rm;

    // mapping of osd to most recently reported osdmap epoch
    map<int32_t,epoch_t> osd_epochs;
  public:

    const map<int32_t, osd_stat_t> &get_osd_stat_updates() const {
      return osd_stat_updates;
    }
    const set<int32_t> &get_osd_stat_rm() const {
      return osd_stat_rm;
    }
    const map<int32_t, epoch_t> &get_osd_epochs() const {
      return osd_epochs;
    }

    void update_stat(int32_t osd, epoch_t epoch, const osd_stat_t &stat) {
      osd_stat_updates[osd] = stat;
      osd_epochs[osd] = epoch;
      assert(osd_epochs.size() == osd_stat_updates.size());
    }
    void stat_osd_out(int32_t osd) {
      // 0 the stats for the osd
      osd_stat_updates[osd] = osd_stat_t();
    }
    void stat_osd_down_up(int32_t osd, PGMap& pg_map) {
      // 0 the op_queue_age_hist for this osd
      map<int32_t,osd_stat_t>::iterator p = osd_stat_updates.find(osd);
      if (p != osd_stat_updates.end()) {
	p->second.op_queue_age_hist.clear();
	return;
      }
      ceph::unordered_map<int32_t,osd_stat_t>::iterator q =
	pg_map.osd_stat.find(osd);
      if (q != pg_map.osd_stat.end()) {
	osd_stat_t& t = osd_stat_updates[osd] = q->second;
	t.op_queue_age_hist.clear();
      }
    }
    void rm_stat(int32_t osd) {
      osd_stat_rm.insert(osd);
      osd_epochs.erase(osd);
      osd_stat_updates.erase(osd);
    }
    void encode(bufferlist &bl, uint64_t features=-1) const;
    void decode(bufferlist::iterator &bl);
    void dump(Formatter *f) const;
    static void generate_test_instances(list<Incremental*>& o);

    Incremental() : version(0), osdmap_epoch(0), pg_scan(0),
        full_ratio(0), nearfull_ratio(0) {}
  };


  // aggregate stats (soft state), generated by calc_stats()
  ceph::unordered_map<int,int> num_pg_by_state;
  int64_t num_pg, num_osd;
  ceph::unordered_map<int,pool_stat_t> pg_pool_sum;
  pool_stat_t pg_sum;
  osd_stat_t osd_sum;
  mutable epoch_t min_last_epoch_clean;
  ceph::unordered_map<int,int> blocked_by_sum;
  ceph::unordered_map<int,set<pg_t> > pg_by_osd;

  utime_t stamp;

  // recent deltas, and summation
  /**
   * keep track of last deltas for each pool, calculated using
   * @p pg_pool_sum as baseline.
   */
  ceph::unordered_map<uint64_t, list< pair<pool_stat_t, utime_t> > > per_pool_sum_deltas;
  /**
   * keep track of per-pool timestamp deltas, according to last update on
   * each pool.
   */
  ceph::unordered_map<uint64_t, utime_t> per_pool_sum_deltas_stamps;
  /**
   * keep track of sum deltas, per-pool, taking into account any previous
   * deltas existing in @p per_pool_sum_deltas.  The utime_t as second member
   * of the pair is the timestamp refering to the last update (i.e., the first
   * member of the pair) for a given pool.
   */
  ceph::unordered_map<uint64_t, pair<pool_stat_t,utime_t> > per_pool_sum_delta;

  list< pair<pool_stat_t, utime_t> > pg_sum_deltas;
  pool_stat_t pg_sum_delta;
  utime_t stamp_delta;

  void update_global_delta(CephContext *cct,
                           const utime_t ts, const pool_stat_t& pg_sum_old);
  void update_pool_deltas(CephContext *cct,
                          const utime_t ts,
                          const ceph::unordered_map<uint64_t, pool_stat_t>& pg_pool_sum_old);
  void clear_delta();

  void deleted_pool(int64_t pool) {
    pg_pool_sum.erase(pool);
    per_pool_sum_deltas.erase(pool);
    per_pool_sum_deltas_stamps.erase(pool);
    per_pool_sum_delta.erase(pool);
  }

 private:
  void update_delta(CephContext *cct,
                    const utime_t ts,
                    const pool_stat_t& old_pool_sum,
                    utime_t *last_ts,
                    const pool_stat_t& current_pool_sum,
                    pool_stat_t *result_pool_delta,
                    utime_t *result_ts_delta,
                    list<pair<pool_stat_t,utime_t> > *delta_avg_list);

  void update_one_pool_delta(CephContext *cct,
                             const utime_t ts,
                             const uint64_t pool,
                             const pool_stat_t& old_pool_sum);

  epoch_t calc_min_last_epoch_clean() const;

 public:

  set<pg_t> creating_pgs;   // lru: front = new additions, back = recently pinged
  map<int,set<pg_t> > creating_pgs_by_osd;

  // Bits that use to be enum StuckPG
  static const int STUCK_INACTIVE = (1<<0);
  static const int STUCK_UNCLEAN = (1<<1);
  static const int STUCK_UNDERSIZED = (1<<2);
  static const int STUCK_DEGRADED = (1<<3);
  static const int STUCK_STALE = (1<<4);
  
  PGMap()
    : version(0),
      last_osdmap_epoch(0), last_pg_scan(0),
      full_ratio(0), nearfull_ratio(0),
      num_pg(0),
      num_osd(0),
      min_last_epoch_clean(0)
  {}

  void set_full_ratios(float full, float nearfull) {
    if (full_ratio == full && nearfull_ratio == nearfull)
      return;
    full_ratio = full;
    nearfull_ratio = nearfull;
    redo_full_sets();
  }

  version_t get_version() const {
    return version;
  }
  void set_version(version_t v) {
    version = v;
  }
  epoch_t get_last_osdmap_epoch() const {
    return last_osdmap_epoch;
  }
  void set_last_osdmap_epoch(epoch_t e) {
    last_osdmap_epoch = e;
  }
  epoch_t get_last_pg_scan() const {
    return last_pg_scan;
  }
  void set_last_pg_scan(epoch_t e) {
    last_pg_scan = e;
  }
  utime_t get_stamp() const {
    return stamp;
  }
  void set_stamp(utime_t s) {
    stamp = s;
  }

  size_t get_num_pg_by_osd(int osd) const {
    ceph::unordered_map<int,set<pg_t> >::const_iterator p = pg_by_osd.find(osd);
    if (p == pg_by_osd.end())
      return 0;
    else
      return p->second.size();
  }

  pool_stat_t get_pg_pool_sum_stat(int64_t pool) const {
    ceph::unordered_map<int,pool_stat_t>::const_iterator p =
      pg_pool_sum.find(pool);
    if (p != pg_pool_sum.end())
      return p->second;
    return pool_stat_t();
  }

  void update_pg(pg_t pgid, bufferlist& bl);
  void remove_pg(pg_t pgid);
  void update_osd(int osd, bufferlist& bl);
  void remove_osd(int osd);

  void apply_incremental(CephContext *cct, const Incremental& inc);
  void redo_full_sets();
  void register_nearfull_status(int osd, const osd_stat_t& s);
  void calc_stats();
  void stat_pg_add(const pg_t &pgid, const pg_stat_t &s, bool sumonly=false,
		   bool sameosds=false);
  void stat_pg_sub(const pg_t &pgid, const pg_stat_t &s, bool sumonly=false,
		   bool sameosds=false);
  void stat_pg_update(const pg_t pgid, pg_stat_t &prev, bufferlist::iterator& blp);
  void stat_osd_add(const osd_stat_t &s);
  void stat_osd_sub(const osd_stat_t &s);
  
  void encode(bufferlist &bl, uint64_t features=-1) const;
  void decode(bufferlist::iterator &bl);

  void dirty_all(Incremental& inc);

  void dump(Formatter *f) const; 
  void dump_basic(Formatter *f) const;
  void dump_pg_stats(Formatter *f, bool brief) const;
  void dump_pool_stats(Formatter *f) const;
  void dump_osd_stats(Formatter *f) const;
  void dump_delta(Formatter *f) const;
  void dump_filtered_pg_stats(Formatter *f, set<pg_t>& pgs);

  void dump_pg_stats_plain(ostream& ss,
			   const ceph::unordered_map<pg_t, pg_stat_t>& pg_stats,
			   bool brief) const;
  void get_stuck_stats(int types, utime_t cutoff,
		       ceph::unordered_map<pg_t, pg_stat_t>& stuck_pgs) const;
  void dump_stuck(Formatter *f, int types, utime_t cutoff) const;
  void dump_stuck_plain(ostream& ss, int types, utime_t cutoff) const;

  void dump(ostream& ss) const;
  void dump_basic(ostream& ss) const;
  void dump_pg_stats(ostream& ss, bool brief) const;
  void dump_pg_sum_stats(ostream& ss, bool header) const;
  void dump_pool_stats(ostream& ss, bool header) const;
  void dump_osd_stats(ostream& ss) const;
  void dump_osd_sum_stats(ostream& ss) const;
  void dump_filtered_pg_stats(ostream& ss, set<pg_t>& pgs);

  void dump_osd_perf_stats(Formatter *f) const;
  void print_osd_perf_stats(std::ostream *ss) const;

  void dump_osd_blocked_by_stats(Formatter *f) const;
  void print_osd_blocked_by_stats(std::ostream *ss) const;

  void get_filtered_pg_stats(const string& state, int64_t poolid, int64_t osdid,
                             bool primary, set<pg_t>& pgs);
  void recovery_summary(Formatter *f, list<string> *psl,
                        const pool_stat_t& delta_sum) const;
  void overall_recovery_summary(Formatter *f, list<string> *psl) const;
  void pool_recovery_summary(Formatter *f, list<string> *psl,
                             uint64_t poolid) const;
  void recovery_rate_summary(Formatter *f, ostream *out,
                             const pool_stat_t& delta_sum,
                             utime_t delta_stamp) const;
  void overall_recovery_rate_summary(Formatter *f, ostream *out) const;
  void pool_recovery_rate_summary(Formatter *f, ostream *out,
                                  uint64_t poolid) const;
  /**
   * Obtain a formatted/plain output for client I/O, source from stats for a
   * given @p delta_sum pool over a given @p delta_stamp period of time.
   */
  void client_io_rate_summary(Formatter *f, ostream *out,
                              const pool_stat_t& delta_sum,
                              utime_t delta_stamp) const;
  /**
   * Obtain a formatted/plain output for the overall client I/O, which is
   * calculated resorting to @p pg_sum_delta and @p stamp_delta.
   */
  void overall_client_io_rate_summary(Formatter *f, ostream *out) const;
  /**
   * Obtain a formatted/plain output for client I/O over a given pool
   * with id @p pool_id.  We will then obtain pool-specific data
   * from @p per_pool_sum_delta.
   */
  void pool_client_io_rate_summary(Formatter *f, ostream *out,
                                   uint64_t poolid) const;

  void print_summary(Formatter *f, ostream *out) const;
  void print_oneline_summary(Formatter *f, ostream *out) const;

  epoch_t get_min_last_epoch_clean() const {
    if (!min_last_epoch_clean)
      min_last_epoch_clean = calc_min_last_epoch_clean();
    return min_last_epoch_clean;
  }

  static void generate_test_instances(list<PGMap*>& o);
};
WRITE_CLASS_ENCODER_FEATURES(PGMap::Incremental)
WRITE_CLASS_ENCODER_FEATURES(PGMap)

inline ostream& operator<<(ostream& out, const PGMap& m) {
  m.print_oneline_summary(NULL, &out);
  return out;
}

#endif
