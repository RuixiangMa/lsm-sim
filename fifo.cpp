#include "fifo.h"

fifo::fifo(uint64_t size)
  : policy(size)
  , accesses{}
  , hits{}
  , current_size{}
  , hash{}
  , queue{}
{
}

fifo::~fifo () {
}

// checks the hashmap for membership, if the key is found
// returns a hit, otherwise the key is added to the hash
// and to the FIFO queue and returns a miss.
void fifo::proc(const request *r) {
  ++accesses;

  auto it = hash.find(r->kid);
  if (it != hash.end()) {
    request* prior_request = it->second;
    if (prior_request->size() == r->size()) {
      ++hits;
      return;
    } else {
      // Size has changed. Even though it is in cache it must have already been
      // evicted or shotdown. Since then it must have already been replaced as
      // well. This means that there must have been some intervening get miss
      // for it. So we need to count an extra access here (but not an extra
      // hit). We do need to remove prior_request from the hash table, but
      // it gets overwritten below anyway when r gets put in the cache.

      // Count the get miss that came between r and prior_request.
      ++accesses;
      // Finally, we need to really put the correct sized value somewhere
      // in the FIFO queue. So fall through to the evict+insert clauses.
    }
  }

  // Throw out enough junk to make room for new record.
  while (global_mem - current_size < uint32_t(r->size())) {
    // If the queue is already empty, then we are in trouble. The cache
    // just isn't big enough to hold this object under any circumstances.
    // Though, you probably shouldn't be setting the cache size smaller
    // than the max memcache object size.
    if (queue.empty())
      return;

    request* victim = &queue.back();
    current_size -= victim->size();
    hash.erase(victim->kid);
    queue.pop_back();
  }

  // Add the new request.
  queue.emplace_front(*r);
  hash[r->kid] = &queue.front();
  current_size += r->size();
 
  // Count this request as a hit.
  ++hits;

  return;
}

uint32_t fifo::get_size() {
  uint32_t size = 0;
  for (const auto& r: queue)
    size += r.size();

  return size;
}

void fifo::log_header() {
  std::cout << "util util_oh cachesize hitrate" << std::endl;
}

void fifo::log() {
  std::cout << double(current_size) / global_mem << " "
            << double(current_size) / global_mem << " "
            << global_mem << " "
            << double(hits) / accesses << std::endl;
}