#include "stdafx.h"
#include "event.h"

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
void Event::AddEvent(std::mutex& mutex, list_t& list, pair_t pair) {
    std::lock_guard<std::mutex> lk(mutex);
    list.insert(std::lower_bound(list.begin(), list.end(), pair, [](const pair_t& lhs, const pair_t& rhs) {
        return !(lhs.first - rhs.first < 0); // sort descending by priority
    }), pair);
}

//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
Event::vector_t Event::GetEvents(std::mutex& mutex, list_t& list) {
    std::lock_guard<std::mutex> lk(mutex);
    vector_t vector;
    for (list_t::iterator it = list.begin(); it != list.end(); ) {
        ptr_t ptr = it->second.lock();
        if (ptr) {
            vector.push_back(ptr);
            ++it;
        } else {
            it = list.erase(it);
        }
    }
    return vector;
}
