#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <set>
#include <cstdint>
#include <getopt.h>
#include <cmath>
#include <boost/version.hpp>
#include <cstdio>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <memory>
#include <cassert>

#include "common.h"
#include "request.h"
#include "fifo.h"
#include "shadowlru.h"
#include "shadowslab.h"
#include "lru.h"
#include "slab.h"
#include "mc.h"

namespace ch = std::chrono;
typedef ch::high_resolution_clock hrc;

const char* policy_names[5] = { "shadowlru"
                              , "fifo"
                              , "lru"
															, "slab"
                              , "shadowslab"
                              };
enum pol_typ { SHADOWLRU = 0, FIFO, LRU, SLAB, SHADOWSLAB };

// globals
std::set<uint16_t>    apps{};                           // apps to consider
bool                  all_apps = true;                  // run all by default 
bool                  roundup  = false;                 // no rounding default
float                 lsm_util = 1.0;                   // default util factor
std::string           trace    = "data/m.cap.out";      // default filepath
std::string           app_str;                          // for logging apps
double                hit_start_time = 86400;           // default time 
double                global_mem = 0;
pol_typ               p_type;                           // policy type
bool                  verbose = false;
double                gfactor = 1.25;                   // def slab growth fact
bool                  memcachier_classes = false;

// Only parse this many requests from the CSV file before breaking.
// Helpful for limiting runtime when playing around.
int request_limit = 0;


  
#ifdef DEBUG
  bool debug = true;
#else
  bool debug = false;
#endif

const std::string     usage  = "-f    specify file path\n"
                               "-a    specify apps to eval\n"
                               "-r    enable rounding\n"
                               "-u    specify utilization\n"
                               "-w    specify warmup period\n"
                               "-l    number of requests after warmup\n"
                               "-s    simulated cache size in bytes\n"
                               "-p    policy 0, 1, 2; shadowlru, fifo, lru\n"
                               "-v    incremental output\n"
                               "-g    specify slab growth factor\n"
                               "-M    use memcachier slab classes\n";

// memcachier slab allocations at t=86400 (24 hours)
const int orig_alloc[15] = {
  1664, 2304, 512, 17408, 266240, 16384, 73728, 188416, 442368,
  3932160, 11665408, 34340864, 262144, 0 , 0
};


// returns true if an ID is in the spec'd set
// returns true if set is empty
bool valid_id(const request *r) {
  if (all_apps)
    return true;
  else
    return apps.count(r->appid);
}

int main(int argc, char *argv[]) {

  // just checking boost
  std::cerr << "Using Boost "     
            << BOOST_VERSION / 100000     << "."  // major version
            << BOOST_VERSION / 100 % 1000 << "."  // minor version
            << BOOST_VERSION % 100                // patch level
            << std::endl;

  // calculate global memory
  for (int i = 0; i < 15; i++)
    global_mem += orig_alloc[i];
  global_mem *= lsm_util;

  // parse cmd args
  int c;
  std::string sets;
  while ((c = getopt(argc, argv, "p:s:l:f:a:ru:w:vhg:M")) != -1) {
    switch (c)
    {
      case 'f':
        trace = optarg;
        break;
      case 'p':
        p_type = pol_typ(atoi(optarg)); 
        break;
      case 's':
        global_mem = atoi(optarg);
        break;
      case 'l':
        request_limit = atoi(optarg); 
        break;
      case 'a':
        {
          string_vec v;
          app_str = optarg;
          csv_tokenize(std::string(optarg), &v);
          all_apps = false;
          for (const auto& e : v)
            apps.insert(stoi(e));
          break;
        }
      case 'r':
        roundup = true;
        break;
      case 'u':
        lsm_util = atof(optarg);
        break;   
      case 'w':
        hit_start_time = atof(optarg);
        break;
      case 'v':
        verbose = true;
        break;
      case 'h': 
        std::cerr << usage << std::endl;
        break;
      case 'g':
        gfactor = atof(optarg);  
        break;
      case 'M':
        memcachier_classes = true;
        break;
    }
  }

  assert(apps.size() == 1);

  std::string filename_suffix = "-app" + std::to_string(*apps.cbegin());
  if (p_type == SHADOWSLAB)
    filename_suffix += memcachier_classes ? "-memcachier" : "-memcached";


  // instantiate a policy
  std::unique_ptr<policy> policy{};
  switch(p_type) {
    case SHADOWLRU:
      policy.reset(new shadowlru(filename_suffix, global_mem));
      break;
    case FIFO : 
      policy.reset(new fifo(filename_suffix, global_mem));
      break;
    case LRU : 
      policy.reset(new lru(filename_suffix, global_mem));
      break;
    case SLAB :
      policy.reset(new slab(filename_suffix, global_mem));
      break;
    case SHADOWSLAB:
      policy.reset(new shadowslab(filename_suffix,
                                  gfactor,
                                  memcachier_classes));
      break;
  }

   if (!policy) {
    std::cerr << "No valid policy selected!" << std::endl;
    exit(-1);
  }

  // List input parameters
  std::cerr << "performing trace analysis on apps: " << app_str << std::endl
            << "policy: " << policy_names[p_type] << std::endl
            << "using trace file: " << trace << std::endl
            << "rounding: " << (roundup ? "on" : "off") << std::endl
            << "utilization rate: " << lsm_util << std::endl
            << "start counting hits at t = " << hit_start_time << std::endl
            << "request limit: " << request_limit << std::endl
            << "global mem: " << global_mem << std::endl;
  if (p_type == SHADOWSLAB) {
    if (memcachier_classes)
      std::cerr << "slab growth factor: memcachier" << std::endl;
    else
      std::cerr << "slab growth factor: " << gfactor << std::endl;
  }

  // proc file line by line
  std::ifstream t_stream(trace);
  std::string line;

  int i = 0;
  auto start = hrc::now();
  auto last_progress = start;
  size_t bytes = 0;
  
 
  while (std::getline(t_stream, line) &&
         (request_limit == 0 || i < request_limit))
  {
    request r{line};
    bytes += line.size();

    if (verbose && ((i & ((1 << 18) - 1)) == 0)) {
      auto now  = hrc::now();
      double seconds =
        ch::duration_cast<ch::nanoseconds>(now - last_progress).count() / 1e9;
      std::cerr << "Progress: " << r.time << " "
                << "Rate: " << bytes / (1 << 20) / seconds << " MB/s"
                << std::endl;
      bytes = 0;
      last_progress = now;
    }

    // Only process requests for specified apps, of type GET,
    // and values of size > 0, after time 'hit_start_time'.
    if ((r.type != request::GET) || !valid_id(&r) || (r.val_sz <= 0))
      continue;

    policy->proc(&r, r.time < hit_start_time);

    ++i;
  }
  auto stop = hrc::now();

  policy->log();

  double seconds =
    ch::duration_cast<ch::milliseconds>(stop - start).count() / 1000.;
  std::cerr << "total execution time: " << seconds << std::endl;

  return 0;
}
