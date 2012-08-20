/* Copyright 2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE. */

#ifndef HABITAT_RFC3339_H

#include <string>
#include <ctime>
#include <stdexcept>

using namespace std;

namespace RFC3339 {

class InvalidFormat : public invalid_argument
{
public:
    InvalidFormat() : invalid_argument("RFC3339::InvalidFormat") {};
    InvalidFormat(const string &what) : invalid_argument(what) {};
};

/*
 * This is basically a clone of habitat.utils.rfc3339, except all timestamps
 * are time_t (may have restrictions on 32 bit, and they cannot be floats
 * like they can in python)
 *
 * You should call tzset() before using either _localoffset function, since it
 * calls localtime_r().
 */

bool validate_rfc3339(const string &rfc3339);
long long int rfc3339_to_timestamp(const string &rfc3339);
string timestamp_to_rfc3339_utcoffset(long long int timestamp);
string timestamp_to_rfc3339_localoffset(long long int timestamp);
string now_to_rfc3339_utcoffset();
string now_to_rfc3339_localoffset();

} /* namespace RFC3339 */

#endif /* HABITAT_RFC3339_H */
